// What the buttons and the touch screen do, and what changes by itself from one frame to the next.
#include "app_internal.h"

#include <cmath>

#include "../core/remote.h"

namespace hbp {

using namespace app;

void App::update(const InputState& input, double dt) {
    _time += dt;
    u32 pressed = input.held & ~_lastHeld;

    if (_resultsReady.exchange(false)) {
        _screen = Screen::Results;
        _resultTop = 0;
        _dialog.reset();
        _menu.reset();
        _dirty = true;
    } else if (_screen == Screen::Job && !_workerRunning) {
        // A job only ends without results if something unexpected got in its way.
        _screen = Screen::Main;
        std::string why;
        {
            sys::Lock lock(_mutex);
            why = _unexpected;
        }
        showMessage("操作未成功", (why.empty() ? std::string("发生了意外错误。") : why + ".") + (!_logProblem.empty() ? "且日志无法写入：" + _logProblem : _log ? "更多内容见" + _logPath : std::string()));
    }

    catchUpWithDrives();
    catchUpWithPcs();

    if (_toastUntil > 0 && _time >= _toastUntil) {
        _toastUntil = 0;
        _toast.clear();
        _dirty = true;
    }
    showWhatsWaiting();

    // Set from here, so that it all happens on one thread.
    JobKind kind = _jobKind;
    bool working = _analystRunning || (kind != JobKind::None && kind != JobKind::Scanning && kind != JobKind::Starting);
    if (working != _working) {
        _working = working;
        _platform.setWorking(working, working && _settings.boost);
    }

    handleTouch(input);

    if (_dialog) {
        updateDialog(pressed);
    } else if (_menu) {
        updateMenu(pressed, input, dt);
    } else {
        switch (_screen) {
            case Screen::Main:
                updateMain(pressed, input, dt);
                break;
            case Screen::Job:
                // Half a game removed is of no use to anyone, and it doesn't take long.
                if ((pressed & ButtonB) && !stopAsked() && _jobKind != JobKind::Removing)
                    askToStop();
                break;
            case Screen::Results:
                updateResults(pressed, input, dt);
                break;
            case Screen::NoKeys:
                if (pressed & (ButtonPlus | ButtonB))
                    pressQuit();
                if (pressed & ButtonA)
                    lookForKeysAgain();
                break;
            case Screen::Browse:
                updateBrowse(pressed, input, dt);
                break;
            case Screen::Welcome:
                updateWelcome(pressed, input, dt);
                break;
        }
    }

    if (!_savedTicks.empty() && _screen == Screen::Main && !_dialog && !_menu && !_afterWork && !busy())
        restoreTicks();
    startWhatsDue();
    animate(dt);
    _lastHeld = input.held;
}

/// Whatever there is to say that came up while something else was on the screen: a save that failed, an update, and what happened last time.
void App::showWhatsWaiting() {
    if (_dialog || _menu)
        return;
    if (_screen == Screen::Main) {
        std::string problem;
        {
            sys::Lock lock(_logMutex);
            problem.swap(_saveProblem);
            _saveProblemShown = _saveProblemShown || !problem.empty();
        }
        if (!problem.empty()) {
            showMessage("保存失败", problem);
            return;
        }

        std::string title, text, program;
        {
            sys::Lock lock(_mutex);
            title.swap(_updateNoticeTitle);
            text.swap(_updateNoticeText);
            program.swap(_updatedProgram);
        }
        if (!title.empty()) {
            showMessage(title, text);
            if (!program.empty()) {
                _dialog->choices.clear();
                _dialog->choices.push_back({"立即重启", [this, program] {
                    if (_platform.restartAfterExit(program))
                        _quit = true;
                    else
                        showMessage("无法从此处重启", "请关闭并重新打开以使用新版本。");
                }});
                _dialog->choices.push_back({"稍后", nullptr});
                _dialog->cancelChoice = 1;
            }
            return;
        }
    }

    std::string notes;
    {
        sys::Lock lock(_mutex);
        for (auto& note : _notes)
            notes += (notes.empty() ? "" : " ") + note;
        _notes.clear();
    }
    if (!notes.empty()) {
        showMessage("上次运行时", notes);
        return;
    }

    // Once each time it's opened as an applet, as that's what has it short of memory, and the way round it isn't anything anyone would guess.
    if (_memoryLimited && !_appletSaid && _screen == Screen::Main) {
        _appletSaid = true;
        showMessage("以小程序模式启动", format("从相册打开时，本程序只能使用 %s 内存，大型游戏可能无法处理，压缩也会更慢，因为每份新副本都要从存储卡读回。想使用全部内存，请在启动任意游戏时按住 R 键：此时会打开自制程序菜单，从这里启动本程序可获得约 3 GB 内存。",
                                                  formatSize(sys::workMemory()).c_str()));
    }
}

/// A USB drive can come and go at any time. Anything being written to one when it goes fails by itself, and otherwise the lists catch up.
void App::catchUpWithDrives() {
    _drivesChanged = _platform.drivesChanged() || _drivesChanged;
    JobKind running = _jobKind;
    bool jobRunning = running == JobKind::Compressing || running == JobKind::Extracting || running == JobKind::Checking || running == JobKind::Removing;
    if (!_drivesChanged || jobRunning)
        return;
    _drivesChanged = false;
    quietenWorker();
    size_t before = _drives.size();
    _drives = _platform.drives();
    forgetFileScan();
    {
        sys::Lock lock(_mutex);
        for (auto& row : _rows) {
            if (row.tab == TabFiles)
                row.ready = false;
        }
    }
    if (_drives.size() != before)
        showToast(_drives.size() > before ? "检测到 USB 可移动设备" : "USB 可移动设备已拔出");
    if (_browser && _browser->path.empty())
        browseTo("");
}

/// So can a PC sharing its games: it's heard from a second or so after this opens, which is after the list has first been looked
/// through, and its sending service can stop or start at any time with the cable left in. The cable watcher probes it every few
/// seconds, so one fresh look is enough to clear or bring back its games without unplugging anything.
void App::catchUpWithPcs() {
    bool fromPcs = (_settings.fromPcNetwork || _settings.fromPcCable) && _settings.filesFolder.empty();
    // While choosing a folder, the PCs are among what there is to choose from.
    bool choosingWhere = _screen == Screen::Browse && _browser && _browser->path.empty();
    if ((fromPcs || choosingWhere) && _time - _lastPcCheck >= PcCheckSeconds) {
        _lastPcCheck = _time;
        _pcsChanged = remote::peersChanged() || _pcsChanged;
    }
    JobKind running = _jobKind;
    bool jobRunning = running == JobKind::Compressing || running == JobKind::Extracting || running == JobKind::Checking || running == JobKind::Removing;
    if (!_pcsChanged || jobRunning)
        return;
    _pcsChanged = false;
    if (_browser && _browser->path.empty())
        browseTo("");
    std::vector<std::string> now = fromPcs ? pcRoots() : std::vector<std::string>();
    std::vector<std::string> before;
    {
        sys::Lock lock(_pcsMutex);
        before = _pcsLookedIn;
    }
    // A service can stop and start again with the cable still plugged in, and the files it shares
    // can change while its folder name (pc:USB/) stays the same. Any change the watcher reports
    // therefore gets one fresh look through the list, not just a change of folder names.
    if (fromPcs) {
        quietenWorker();
        forgetFileScan();
        showToast(now.size() > before.size() ? "发现正在共享游戏的 PC"
                  : now.size() < before.size() ? "一台 PC 已停止共享游戏"
                  : "PC 共享内容已更新，正在刷新列表");
    }
}

// -------------------------------------------------------------------------------------------------------------------
// Buttons

bool App::repeated(u32 held, u32 button, double dt) {
    constexpr double FirstDelay = 0.34;
    constexpr double NextDelay = 0.07;
    if (!(held & button)) {
        if (_repeatButton == button)
            _repeatButton = 0;
        return false;
    }
    if (!(_lastHeld & button)) {
        _repeatButton = button;
        _repeatTimer = FirstDelay;
        return true;
    }
    if (_repeatButton != button)
        return false;
    _repeatTimer -= dt;
    if (_repeatTimer <= 0) {
        _repeatTimer += NextDelay;
        return true;
    }
    return false;
}

void App::updateMain(u32 pressed, const InputState& input, double dt) {
    if (pressed & ButtonL)
        setTab((_tab + TabCount - 1) % TabCount);
    if (pressed & ButtonR)
        setTab((_tab + 1) % TabCount);
    if (pressed & ButtonB)
        pressQuit();
    if (_tab == TabSettings) {
        updateOptions(pressed, input, dt);
        return;
    }

    int count;
    {
        sys::Lock lock(_mutex);
        count = (int)visibleRowsLocked(_tab).size();
    }
    int& focus = _focus[_tab];
    if (focus >= count)
        focus = std::max(count - 1, 0);
    if (repeated(input.held, ButtonUp, dt))
        focus = std::max(focus - 1, 0);
    if (repeated(input.held, ButtonDown, dt))
        focus = std::min(focus + 1, std::max(count - 1, 0));
    if (repeated(input.held, ButtonZL, dt))
        focus = std::max(focus - PageRows, 0);
    if (repeated(input.held, ButtonZR, dt))
        focus = std::min(focus + PageRows, std::max(count - 1, 0));

    // Whatever moved it, a button, a tap or the list being put in a new order, it stays in sight. Dragging only moves the list.
    if (focus != _focusSeen[_tab]) {
        _focusSeen[_tab] = focus;
        keepFocusVisible();
        _dirty = true;
    }
    double most = std::max(0.0, (double)count * RowH - ListH);
    if (_scroll[_tab] > most)
        _scroll[_tab] = most;

    if (pressed & ButtonA)
        toggleRow(_focus[_tab]);
    if (pressed & ButtonY)
        cycleTicks();
    if (pressed & ButtonX)
        pressCompress();
    if (pressed & ButtonPlus)
        openMenu();
}

/// Left and right change a setting, and A changes it too, or does what it's for.
void App::updateOptionItem(OptionItem& item, u32 pressed, const InputState& input, double dt) {
    bool left = item.repeats ? repeated(input.held, ButtonLeft, dt) : (pressed & ButtonLeft) != 0;
    bool right = item.repeats ? repeated(input.held, ButtonRight, dt) : (pressed & ButtonRight) != 0;
    if (left && !item.action) {
        item.change(-1);
        _dirty = true;
    }
    if ((pressed & ButtonA) || (right && !item.action)) {
        item.change(1);
        _dirty = true;
    }
}

void App::updateOptions(u32 pressed, const InputState& input, double dt) {
    if (repeated(input.held, ButtonUp, dt) && _optionFocus > 0) {
        _optionFocus--;
        _dirty = true;
    }
    // Not while one of them is being used, which is what asks for it.
    if (_rebuildOptions) {
        _rebuildOptions = false;
        // The one that asked for it stays where the focus is, or More or Fewer settings, which changes its name when it's pressed.
        std::string focused = _optionFocus >= 0 && _optionFocus < (int)_options.size() ? _options[(size_t)_optionFocus].label : "";
        buildOptions();
        _optionFocus = std::min(_optionFocus, (int)_options.size() - 1);
        int found = -1;
        for (int i = 0; i < (int)_options.size() && found < 0; i++) {
            if (_options[(size_t)i].label == focused)
                found = i;
        }
        for (int i = 0; i < (int)_options.size() && found < 0; i++) {
            if (_options[(size_t)i].label == "更多设置" || _options[(size_t)i].label == "收起设置")
                found = i;
        }
        if (found >= 0)
            _optionFocus = found;
        _dirty = true;
    }
    if (repeated(input.held, ButtonDown, dt) && _optionFocus < (int)_options.size() - 1) {
        _optionFocus++;
        _dirty = true;
    }
    updateOptionItem(_options[(size_t)_optionFocus], pressed, input, dt);
}

void App::updateWelcome(u32 pressed, const InputState& input, double dt) {
    if (repeated(input.held, ButtonUp, dt) && _welcomeFocus > 0) {
        _welcomeFocus--;
        _dirty = true;
    }
    if (repeated(input.held, ButtonDown, dt) && _welcomeFocus < (int)_welcome.size() - 1) {
        _welcomeFocus++;
        _dirty = true;
    }
    updateOptionItem(_welcome[(size_t)_welcomeFocus], pressed, input, dt);
    if (pressed & ButtonPlus)
        finishWelcome();
    else if (pressed & ButtonB)
        pressQuit();
}

void App::updateBrowse(u32 pressed, const InputState& input, double dt) {
    Browser& browser = *_browser;
    int count = (int)browser.entries.size();
    int focus = browser.focus;
    if (repeated(input.held, ButtonUp, dt))
        focus = std::max(focus - 1, 0);
    if (repeated(input.held, ButtonDown, dt))
        focus = std::min(focus + 1, std::max(count - 1, 0));
    if (repeated(input.held, ButtonZL, dt))
        focus = std::max(focus - 8, 0);
    if (repeated(input.held, ButtonZR, dt))
        focus = std::min(focus + 8, std::max(count - 1, 0));
    if (focus != browser.focus) {
        browser.focus = focus;
        _dirty = true;
    }
    if (pressed & ButtonMinus)
        closeBrowser();
    else if ((pressed & ButtonA) && count > 0)
        browseInto(browser.focus);
    else if (pressed & ButtonB)
        browseUp();
    else if (pressed & ButtonX)
        browseUse();
    else if (pressed & ButtonY)
        browseUseDefault();
}

void App::updateResults(u32 pressed, const InputState& input, double dt) {
    int count;
    {
        sys::Lock lock(_mutex);
        count = (int)_results.size();
    }
    int most = std::max(count - VisibleRows, 0);
    if (repeated(input.held, ButtonUp, dt) && _resultTop > 0) {
        _resultTop--;
        _dirty = true;
    }
    if (repeated(input.held, ButtonDown, dt) && _resultTop < most) {
        _resultTop++;
        _dirty = true;
    }
    if (pressed & (ButtonA | ButtonB)) {
        _screen = Screen::Main;
        _dirty = true;
    }
}

void App::updateDialog(u32 pressed) {
    Dialog& dialog = *_dialog;
    if (pressed & (ButtonLeft | ButtonUp)) {
        dialog.focus = std::max(dialog.focus - 1, 0);
        _dirty = true;
    }
    if (pressed & (ButtonRight | ButtonDown)) {
        dialog.focus = std::min(dialog.focus + 1, (int)dialog.choices.size() - 1);
        _dirty = true;
    }
    int chosen = (pressed & ButtonA) ? dialog.focus : (pressed & ButtonB) ? dialog.cancelChoice : -1;
    if (chosen >= 0) {
        auto action = dialog.choices[(size_t)chosen].action;
        _dialog.reset();
        _dirty = true;
        if (action)
            action();
    }
}

void App::updateMenu(u32 pressed, const InputState& input, double dt) {
    Menu& menu = *_menu;
    if (repeated(input.held, ButtonUp, dt)) {
        menu.focus = std::max(menu.focus - 1, 0);
        _dirty = true;
    }
    if (repeated(input.held, ButtonDown, dt)) {
        menu.focus = std::min(menu.focus + 1, (int)menu.items.size() - 1);
        _dirty = true;
    }
    Menu::Item& item = menu.items[(size_t)menu.focus];
    if (item.change && (pressed & (ButtonLeft | ButtonRight))) {
        item.change(pressed & ButtonLeft ? -1 : 1);
        _dirty = true;
    }
    if (pressed & ButtonA) {
        if (item.change) {
            item.change(1);
            _dirty = true;
            return;
        }
        auto action = item.action;
        _menu.reset();
        _dirty = true;
        if (action)
            action();
    } else if (pressed & ButtonB && menu.page != MenuMain) {
        openMenu();
    } else if (pressed & (ButtonB | ButtonPlus)) {
        _menu.reset();
        _dirty = true;
    }
}

// -------------------------------------------------------------------------------------------------------------------
// The touch screen

void App::handleTouch(const InputState& input) {
    constexpr int TapDistance = 16;
    if (input.touching && !_touchWasDown) {
        _touchStartX = input.touchX;
        _touchStartY = input.touchY;
        _touchLastY = input.touchY;
        _touchDragged = false;
    } else if (input.touching) {
        if (abs(input.touchY - _touchStartY) > TapDistance || abs(input.touchX - _touchStartX) > TapDistance)
            _touchDragged = true;
        if (_touchDragged && _screen == Screen::Browse && _browser && !_dialog) {
            _browser->scroll = std::max(0.0, _browser->scroll - (input.touchY - _touchLastY));
            _dirty = true;
        } else if (_touchDragged && _screen == Screen::Main && _tab == TabSettings && !_dialog) {
            _optionScroll = std::max(0.0, _optionScroll - (input.touchY - _touchLastY));
            _dirty = true;
        } else if (_touchDragged && _screen == Screen::Main && !_dialog && !_menu) {
            int count;
            {
                sys::Lock lock(_mutex);
                count = (int)visibleRowsLocked(_tab).size();
            }
            double most = std::max(0.0, (double)count * RowH - ListH);
            _scroll[_tab] = std::max(0.0, std::min(most, _scroll[_tab] - (input.touchY - _touchLastY)));
            _scrollShown = _scroll[_tab];
            _dirty = true;
        }
        _touchLastY = input.touchY;
    } else if (_touchWasDown && !_touchDragged) {
        // Whatever was drawn last is on top.
        for (auto it = _hits.rbegin(); it != _hits.rend(); ++it) {
            if (it->area.contains(_touchStartX, _touchStartY)) {
                auto action = it->action;
                _dirty = true;
                if (action)
                    action();
                break;
            }
        }
    }
    _touchWasDown = input.touching;
}

// -------------------------------------------------------------------------------------------------------------------
// Movement

void App::keepFocusVisible() {
    double top = _focus[_tab] * (double)RowH;
    if (top < _scroll[_tab])
        _scroll[_tab] = top;
    if (top + RowH > _scroll[_tab] + ListH)
        _scroll[_tab] = top + RowH - ListH;
}

double App::glide(int list, double target) {
    if (list != _glideList) {
        _glideList = list;
        _glideShown = target;
    }
    return glideTowards(_glideShown, target);
}

double App::glideTowards(double& shown, double target) {
    constexpr double CloseEnough = 0.5;
    shown += (target - shown) * _glideStep;
    if (fabs(shown - target) < CloseEnough)
        shown = target;
    else
        _dirty = true;
    return shown;
}

void App::animate(double dt) {
    double k = 1 - exp(-dt * 18);
    _glideStep = k;
    auto approach = [&](double& value, double target, double close) {
        if (value == target)
            return;
        value += (target - value) * k;
        if (fabs(value - target) < close)
            value = target;
        _dirty = true;
    };
    approach(_tabShown, _tab, 0.004);
    approach(_focusShown, _focus[_tab], 0.004);
    approach(_scrollShown, _scroll[_tab], 0.5);
}

bool App::needsDrawing() {
    // A spinner wants to keep turning, while a job wants the processor for itself.
    double now = sys::now();
    bool busy = this->busy();
    double interval = _screen == Screen::Job || _analystRunning ? 0.25 : 0.1;
    if (_dirty.exchange(false) || (busy && now - _lastWorkDraw >= interval)) {
        _lastWorkDraw = now;
        return true;
    }
    return false;
}

// -------------------------------------------------------------------------------------------------------------------
// For the tests

App::Snapshot App::snapshot() {
    static const char* const screens[] = {"Main", "Job", "Results", "NoKeys", "Browse", "Welcome"};
    Snapshot s;
    s.screen = _screen == Screen::Main && _tab == TabSettings ? "Options" : screens[(int)_screen];
    s.tab = _tab;
    s.analyzing = _analystRunning;
    s.scanning = _jobKind == JobKind::Scanning || _jobKind == JobKind::Starting;
    s.dialog = _dialog ? _dialog->title : "";
    if (_dialog) {
        for (auto& paragraph : _dialog->paragraphs)
            s.dialogText += paragraph + "\n";
    }
    s.toast = _toast;
    s.actionWord = verb();
    if (_browser) {
        s.browsing = _browser->path;
        for (auto& entry : _browser->entries)
            s.browseEntries.push_back(entry.name + (entry.note.empty() ? "" : " (" + entry.note + ")"));
    }
    for (auto& item : _welcome)
        s.welcome.push_back(item.label + ": " + item.value());
    s.welcomeFocus = _welcomeFocus;
    for (auto& item : _options)
        s.options.push_back(item.label + ": " + item.value());
    s.optionFocus = _optionFocus;
    if (_menu) {
        for (auto& item : _menu->items)
            s.menu.push_back(item.value ? item.label + ": " + item.value() : item.label);
        s.menuFocus = _menu->focus;
    }
    sys::Lock lock(_mutex);
    s.updateState = _updateState;
    s.sortedBy = sortDescription();
    s.tickHint = tickActionLabel(nextTickActionLocked());
    s.label = _jobLabel;
    s.overall = overallLocked(nullptr);
    s.resultsTitle = _resultsTitle;
    s.resultsDetail = _resultsDetail;
    for (auto& result : _results)
        s.results.push_back(result.name + ": " + result.outcome + (result.message.empty() ? "" : " (" + result.message + ")"));
    for (auto* row : visibleRowsLocked(_tab)) {
        RowView view;
        view.name = row->name;
        view.ticked = row->ticked;
        view.hasEstimate = row->hasEstimate;
        view.compressed = isCompressed(*row);
        view.note = !row->blocked.empty() ? row->blocked : !row->message.empty() ? row->message : row->belowMinimum;
        view.detail = row->detail;
        view.notes = (int)row->notes.size();
        s.rows.push_back(view);
    }
    s.compressedHidden = doneHiddenLocked(_tab < TabCount ? _tab : TabInstalled);
    return s;
}

}  // namespace hbp
