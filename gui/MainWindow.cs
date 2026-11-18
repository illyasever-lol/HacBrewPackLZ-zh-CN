namespace HacBrewPackLz;

/// <summary>The window people see: a list, and what can be done with it. Everything else is a click further away.</summary>
internal sealed class MainWindow : AppWindow
{
    private readonly Session _session;
    private readonly TitleList _list;
    private readonly Button _addFiles;
    private readonly Button _addFolder;
    private readonly Button _search;
    private readonly Button _add;
    private readonly Button _options;
    private readonly Button _more;
    private readonly Button _chooseKeys;
    private readonly Button _analyze;
    private readonly Button _compress;
    /// <summary>In sight beside Compress, as the games don't have to be compressed first: the Switch app does it as it installs them.</summary>
    private readonly Button _send;
    private readonly Button _stop;
    private readonly Button _details;
    private readonly Button _showOutput;
    /// <summary>From what a job came to, back to the list, where what it compressed is under Done.</summary>
    private readonly Button _backToList;
    private readonly ProgressLine _progress;
    private readonly LogView _log;
    private int _refreshPending;
    private SessionSnapshot _state;
    private bool _showDetails;
    /// <summary>v21：只在「列表 / 页面」切换时批量重置可见性，避免每次 Relayout 都让被隐藏的控件先闪一下。</summary>
    private bool _layoutPaged;
    /// <summary>v21：任务进行中把日志按钮钉住，避免 1.5.1 清空日志的瞬间它消失一帧。</summary>
    private bool _detailsSticky;
    private bool _closeWhenIdle;
    /// <summary>Everything ticked is compressed already, so the big button sends it to the Switch instead.</summary>
    private bool _sendNext;
    /// <summary>The bar along the bottom with how it's going and what to do next. An empty list doesn't have it.</summary>
    private Rect _bar;
    private bool _barShown;
    /// <summary>Where the words about how it's going go, inside the panel.</summary>
    private Rect _statusArea;
    private Timer? _animation;
    /// <summary>Only while sending: time-based state (Settled / Reading) needs fresh snapshots even with no events.</summary>
    private Timer? _sendTick;
    /// <summary>
    /// The screen in place of the list, as on the Switch: the welcome the first time it opens, the options, checking an NSP,
    /// searching a location or what this is. Null for the list.
    /// </summary>
    private Page? _page;
    private readonly Button _back;

    public MainWindow(Session session)
    {
        _session = session;
        _state = session.Snapshot();
        // Tall enough for the welcome to fit without scrolling.
        CreateTopLevel(AppInfo.Name, 1024, 768, null);

        _list = Add(new TitleList(row => _session.Toggle(row), all => _session.SelectAll(all), row => _session.Remove(row), issue => _session.Remove(issue), Explorer.Show,
            OpenDetails, GameMenu, view => _session.SetView(view)));
        _addFiles = Add(new Button("添加文件", ButtonStyle.Secondary, AddFiles, Glyph.Add));
        _addFolder = Add(new Button("添加文件夹", ButtonStyle.Secondary, AddFolder, Glyph.Folder));
        _search = Add(new Button("搜索位置", ButtonStyle.Secondary, OpenSearch, Glyph.Search));
        _add = Add(new Button("添加", ButtonStyle.Secondary, OpenAddMenu, Glyph.Add));
        _options = Add(new Button("选项", ButtonStyle.Ghost, OpenOptions, Glyph.Settings));
        _more = Add(new Button("", ButtonStyle.Ghost, OpenMore, Glyph.More));
        _chooseKeys = Add(new Button("选择密钥", ButtonStyle.Secondary, ChooseKeys));
        // What happened as the last job went. Details is a game's own page now, as in every version.
        _details = Add(new Button("日志", ButtonStyle.Ghost, ToggleDetails, Glyph.Log));
        _analyze = Add(new Button("分析", ButtonStyle.Secondary, () => _session.Analyze()));
        _compress = Add(new Button("压缩", ButtonStyle.Primary, PressCompress) { Menu = TickedMenu });
        _send = Add(new Button("发送到 Switch", ButtonStyle.Secondary, SendToSwitch, Glyph.Send));
        _stop = Add(new Button("停止", ButtonStyle.Secondary, () => _session.Stop()));
        _showOutput = Add(new Button("在文件夹中显示", ButtonStyle.Ghost, ShowLastOutput, Glyph.FolderOpen));
        _backToList = Add(new Button("返回列表", ButtonStyle.Secondary, () => _session.BackToList(), Glyph.Back));
        _progress = Add(new ProgressLine());
        _log = Add(new LogView { Visible = false });
        _back = Add(new Button("", ButtonStyle.Ghost, Back, Glyph.Back) { Visible = false });
        if (!session.Settings.Welcomed)
            _page = new WelcomePage(this, session);
        else if (session.Settings.AskAboutProblems)
            _page = new ProblemsQuestionPage(this, session);

        session.Changed += () =>
        {
            if (Interlocked.Exchange(ref _refreshPending, 1) == 0)
                PostFromWorker();
        };
        DropTarget.Register(Handle, hover =>
        {
            _list.DropHover = hover;
            Invalidate();
        }, files =>
        {
            // Games dropped on any page are for the list, and on the welcome they're as good as Start.
            ClosePage();
            _session.AddInputs(files);
        });

        Refresh();
        Relayout();
    }

    protected override (int width, int height) MinimumSize => (660, 460);

    protected override void OnMessageFromWorker()
    {
        // The timer only asks for the moving parts to be drawn again.
        if (Interlocked.Exchange(ref _refreshPending, 0) == 1)
            Refresh();
        else
            Invalidate();
        _page?.OnWorker();
    }

    /// <summary>A screen in place of the list until it's left. Only one at a time: another one takes its place.</summary>
    public void OpenPage(Page page)
    {
        if (_page is not null)
        {
            _page.Leaving();
            _page.RemoveWidgets();
        }
        _page = page;
        FadeIn();
        Relayout();
        Invalidate();
    }


    /// <summary>Back from a page: where it says, as from Look inside to the game's Details, or else to the list.</summary>
    private void Back()
    {
        if (_page?.GoBack() == true)
            return;
        ClosePage();
    }

    /// <summary>Back to the list, keeping whatever the page had been given.</summary>
    public void ClosePage()
    {
        if (_page is null)
            return;
        var page = _page;
        _page = null;
        page.Leaving();
        page.RemoveWidgets();
        FadeIn();
        Relayout();
        Invalidate();
    }

    /// <summary>Keeps what moves moving while there's no telling how far along a job is, such as reading files, so it doesn't look stuck.</summary>
    private void Animate(bool on)
    {
        if (on && _animation is null)
            _animation = new Timer(_ => PostFromWorker(), null, 0, 33);
        else if (!on && _animation is not null)
        {
            _animation.Dispose();
            _animation = null;
        }
    }

    /// <summary>
    /// A snapshot's Settled / Reading are functions of time, but Refresh() only runs on session.Changed.
    /// The Switch stops reading without raising an event, so without a tick the last snapshot freezes
    /// at 66%/93% and "正在发送 XX.nsp" stays on the bar forever (the v16 bug).
    /// </summary>
    private void TickSending(bool on)
    {
        if (on && _sendTick is null)
            _sendTick = new Timer(_ =>
            {
                if (Interlocked.Exchange(ref _refreshPending, 1) == 0)
                    PostFromWorker();
            }, null, 0, 1000);
        else if (!on && _sendTick is not null)
        {
            _sendTick.Dispose();
            _sendTick = null;
        }
    }

    protected override void OnThemeChanged() => _log.ApplyTheme();

    /// <summary>What the big button does: compress while anything ticked is left to do, and then send it to the Switch.</summary>
    private bool Compressing => _session.Settings.Get("--nocompress") != "1";

    private void PressCompress()
    {
        if (_sendNext)
        {
            SendToSwitch();
            return;
        }
        Compress();
    }

    private void Compress()
    {
        if (!_state.Rows.Any(r => r.LeftToCompress(Compressing)))
            return;
        var names = _session.TickedWithoutUpdate();
        if (names.Length > 0)
        {
            const int NamesShown = 6;
            string list = string.Join("\n", names.Take(NamesShown)) + (names.Length > NamesShown ? $"\n另有 {names.Length - NamesShown} 个" : "");
            int answer = Win32.MessageBoxW(Handle, $"关闭压缩时，重打包只能靠把更新补丁并入本体来缩小体积，而以下游戏没有更新补丁：\n\n{list}\n\n是否取消勾选它们？\n\n选择「否」则一并重打包。", AppInfo.Name, Win32.MB_YESNOCANCEL | Win32.MB_ICONINFORMATION);
            if (answer == Win32.IDCANCEL)
                return;
            if (answer == Win32.IDYES)
                _session.UntickWithoutUpdate();
        }
        _session.Compress();
    }

    protected override bool OnClosing()
    {
        // Reading and measuring write nothing, so there's nothing to finish first.
        if (!_state.IsBusy || _state.Activity is SessionActivity.Reading or SessionActivity.Analyzing or SessionActivity.Measuring)
            return true;
        if (Win32.MessageBoxW(Handle, "仍有任务在运行。确定停止并关闭吗？\n\n已完成的游戏会保留。", AppInfo.Name, Win32.MB_YESNO | Win32.MB_ICONWARNING) != Win32.IDYES)
            return false;
        _session.Stop();
        _closeWhenIdle = true;
        return false;
    }

    protected override void OnClosed()
    {
        Animate(false);
        TickSending(false);
        Win32.PostQuitMessage(0);
    }

    protected override bool OnKey(int key)
    {
        if (_page is not null)
        {
            if (key == Win32.VK_ESCAPE && _page.CanGoBack)
            {
                Back();
                return true;
            }
            return _page.OnKey(key);
        }
        if (key == Win32.VK_ESCAPE && _state.IsBusy)
        {
            _session.Stop();
            return true;
        }
        // From what a job came to, Esc goes back to the list, as on the home screen.
        if (key == Win32.VK_ESCAPE && _state.Job is { Running: false })
        {
            _session.BackToList();
            return true;
        }
        return false;
    }

    private void Refresh()
    {
        var state = _session.Snapshot();
        int ticked = state.Rows.Count(r => r.Selected);
        bool canCompress = state.Rows.Any(r => r.LeftToCompress(Compressing));
        // Whatever's ticked is compressed already, so it's ready for the Switch.
        bool sendNext = !canCompress && state.Rows.Any(r => r.Selected && (r.IsDone(BatchMode.Convert) || r.IsCompressed));
        string compressLabel = sendNext ? "发送到 Switch"
            : (_session.Settings.Get("--nocompress") == "1" ? "重打包" : "压缩") + (ticked < state.Rows.Length && ticked > 0 ? $" {ticked}" : "");
        bool analyzable = Analyzable(state);
        bool layoutChanged = compressLabel != _compress.Text || sendNext != _sendNext || analyzable != _analyze.Visible
            || state.View != _state.View || (ticked == 0) != !_state.Rows.Any(r => r.Selected)
            || state.Rows.Length > 0 != _state.Rows.Length > 0 || state.IsBusy != _state.IsBusy || state.Activity != _state.Activity
            || state.ShowingJob != _state.ShowingJob || state.Job?.Running != _state.Job?.Running
            || _list.IsEmpty != (state.EverythingCount == 0 && state.Issues.Length == 0 && state.Notes.Length == 0) || (state.Log.Length > 0) != (_state.Log.Length > 0)
            || (state.KeyProblem is null) != (_state.KeyProblem is null) || (state.Summary is null) != (_state.Summary is null)
            || (state.LastOutput is null) != (_state.LastOutput is null) || (state.Issues.Length == 0) != (_state.Issues.Length == 0);

        _state = state;
        _sendNext = sendNext;
        _list.Repacking = !Compressing;
        _list.SetData(state);
        _log.SetLines(state.Log);
        _compress.Text = compressLabel;
        // v19.1：Refresh 不一定触发 Relayout，这里也把主操作按钮的文字钉死，杜绝残留图标态。
        _send.Text = "发送到 Switch";
        _compress.Icon = sendNext ? Glyph.Send : '\0';
        UpdateEnabled();
        TaskbarProgress.Set(Handle, ProgressFraction(state));
        bool sliding = state.IsBusy && ProgressFraction(state) is null;
        _progress.Sliding = sliding;
        Animate(sliding);
        TickSending(state.Activity == SessionActivity.Sending);
        // What the bar says may need more room, or less, than it has.
        layoutChanged |= _barShown && _page is null && BarHeight(_statusWidth) != _bar.H;
        if (layoutChanged)
            Relayout();
        else
            WriteTestLayout();
        Invalidate();

        if (_closeWhenIdle && !state.IsBusy)
            Win32.DestroyWindow(Handle);
    }

    /// <summary>Analyze is there while a ticked game has no answer yet, or a thorough analysis has games left to read in full.</summary>
    private bool Analyzable(SessionSnapshot state) =>
        state.Rows.Any(r => r.Selected && r.NeedsEstimate && !r.IsCompressed) || (!state.IsBusy && state.Rows.Length > 0 && _session.CanMeasure);

    /// <summary>How far along the job is, for the bar along the bottom and the taskbar. While sending, that's the file the Switch is reading.</summary>
    private static double? ProgressFraction(SessionSnapshot state)
    {
        if (state.Activity == SessionActivity.Sending)
        {
            if (state.Send is { Reading: { } file } send && send.Files.TryGetValue(file, out var sent) && sent.length > 0)
                return (double)sent.read / sent.length;
            return null;
        }
        // A job's own view goes by all of it together, as the game being done shows how far it's got in its row.
        if (state.Job is { Running: true } job)
            return job.Overall;
        if (state.Activity is SessionActivity.Compressing or SessionActivity.Extracting or SessionActivity.Verifying or SessionActivity.Analyzing or SessionActivity.Measuring
            && state.Progress is { Total: > 0 } progress)
            return (double)progress.Done / progress.Total;
        return null;
    }

    private void UpdateEnabled()
    {
        bool idle = !_state.IsBusy;
        // Measuring goes on behind the list, and gives way to anything asked for.
        bool free = idle || (_state.Activity == SessionActivity.Measuring && !_state.ShowingJob);
        _addFiles.Enabled = _addFolder.Enabled = _search.Enabled = _options.Enabled = _more.Enabled = _add.Enabled = free;
        _analyze.Enabled = idle;
        _send.Enabled = free;
        _backToList.Enabled = _state.Job is { Running: false };
        // The arrow still works when the big button has nothing to do.
        _compress.Enabled = free && _state.Rows.Length > 0;
        _compress.MainEnabled = _sendNext || _state.Rows.Any(r => r.LeftToCompress(Compressing));
    }

    protected override void Layout(int width, int height)
    {
        if (_list is null)
            return;
        bool paged = _page is not null;
        _back.Visible = paged && _page!.CanGoBack;
        // v21：这里原来每次 Relayout 都把所有控件先设成 !paged，随后又按条件把日志、
        // 日志按钮、右侧按钮设回隐藏。压缩时状态文字变化使 Relayout 高频发生，于是这些
        // 控件每一帧都经历「显示一瞬间→消失」，右侧可见闪烁。批量重置只在页面/列表
        // 真正切换时做一次；条件可见性全部由本方法后面的赋值决定。
        if (paged != _layoutPaged)
        {
            foreach (var widget in new Widget[] { _list, _add, _options, _more, _chooseKeys, _analyze, _compress, _send, _stop, _details, _showOutput, _backToList, _progress, _log, _addFiles, _addFolder, _search })
                widget.Visible = !paged;
            _layoutPaged = paged;
        }
        if (paged)
        {
            LayoutPage(width, height);
            return;
        }

        int pad = S(20);
        int top = S(62);
        bool busy = _state.IsBusy;

        // Header: the name on the left, and Add, Options and the rest on the right.
        int headerY = S(16);
        int buttonHeight = S(34);
        _more.Bounds = new Rect(width - pad - S(36), headerY, S(36), buttonHeight);
        int optionsWidth = _options.PreferredWidth();
        _options.Bounds = new Rect(_more.Bounds.X - optionsWidth - S(4), headerY, optionsWidth, buttonHeight);
        int addWidth = _add.PreferredWidth();
        _add.Bounds = new Rect(_options.Bounds.X - addWidth - S(8), headerY, addWidth, buttonHeight);

        // Keys banner
        bool keysMissing = _state.KeyProblem is not null;
        _chooseKeys.Visible = keysMissing;
        if (keysMissing)
        {
            int chooseWidth = _chooseKeys.PreferredWidth();
            _chooseKeys.Bounds = new Rect(width - pad - S(12) - chooseWidth, top + S(11), chooseWidth, buttonHeight);
            top += S(56) + S(12);
        }

        // An empty list is a drop area with the ways to add right in it, and needs nothing else.
        bool empty = _list.IsEmpty;
        // While a job runs they can't be used, so they're not there rather than greyed out. Measuring gives way to them, so they stay.
        bool held = busy && !(_state.Activity == SessionActivity.Measuring && !_state.ShowingJob);
        _add.Visible = !empty && !held;
        _options.Visible = _more.Visible = !held;
        // Only with something on the list to act on: a list with every game hidden just says so, with a way to show them.
        _barShown = _state.Rows.Length + _state.Issues.Length > 0 || busy && _state.Activity != SessionActivity.Reading;

        bool listed = _state.Rows.Length > 0;
        // A job's own view has only what goes with it: stopping it, and once it's over, sending what it made and going back to the list.
        bool job = _state.ShowingJob;
        // Reading games onto an empty list has no bar, so its Stop goes under what it says in the middle.
        bool readingAlone = !_barShown && _state.Activity == SessionActivity.Reading;
        _stop.Visible = busy && (_barShown || readingAlone);
        // Measuring goes on behind the list, which can be compressed or sent meanwhile.
        bool blocked = busy && _state.Activity != SessionActivity.Measuring;
        _compress.Visible = !blocked && listed && (!job || _sendNext);
        // Once Compress has become Send to Switch, it's said once.
        _send.Visible = !blocked && listed && !_sendNext && !job;
        // On Done with nothing ticked and nothing to compress, Compress has nothing to do: it's not there, rather than greyed out, and sending
        // the list is the main thing left.
        bool nothingToCompress = !job && _state.View == GamesShown.Done && !_state.Rows.Any(r => r.Selected) && !_state.Rows.Any(r => r.LeftToCompress(Compressing));
        _compress.Visible = _compress.Visible && !nothingToCompress;
        _send.Style = nothingToCompress ? ButtonStyle.Primary : ButtonStyle.Secondary;
        // Only while a ticked game can use it, or there's more to read in full, rather than greyed out.
        _analyze.Visible = !busy && !job && Analyzable(_state);
        _backToList.Visible = job && _state.Job is { Running: false };
        // The main thing to do next is the one in the accent colour: sending what was made, or else going back to the list.
        _backToList.Style = _compress.Visible ? ButtonStyle.Secondary : ButtonStyle.Primary;
        // v21：1.5.1 每个 job 开始会清空日志；若此刻 IsBusy 有瞬时空档，日志按钮会消失一帧
        // 再出现，右侧按钮组跟着跳。任务进行中把它钉住，直到彻底空闲且日志为空。
        if (busy)
            _detailsSticky = true;
        else if (_state.Log.Length == 0)
            _detailsSticky = false;
        _details.Visible = _barShown && (busy || _state.Log.Length > 0 || _detailsSticky);
        _showOutput.Visible = _barShown && !busy && _state.LastOutput is not null;
        _progress.Visible = busy && _barShown;

        int bottom = height - pad;
        if (_barShown)
        {
            // Along the bottom, as wide as the list: what's going on on the left, and what can be done next on the right,
            // the main thing at the far end. In a narrow window, Details and Show in folder become icons first, so what's
            // said keeps its room; Send to Switch always keeps its words, it's the main action (v19.1).
            // The bar is as tall as all of what it says takes, so none of it is cut short.
            _bar = new Rect(pad, bottom - S(84), width - pad * 2, S(84));
            int statusLeft = _bar.X + S(20);
            bool compact = false;
            int x = PlaceBarButtons(compact);
            if (x - statusLeft < S(300))
                x = PlaceBarButtons(compact = true);
            // While a job runs, its progress goes along the bottom of what's said about it, inside the bar.
            int statusWidth = Math.Max(x - S(24) - statusLeft, 0);
            _statusWidth = statusWidth;
            int barHeight = BarHeight(statusWidth);
            _bar = new Rect(pad, bottom - barHeight, width - pad * 2, barHeight);
            PlaceBarButtons(compact);
            bottom = _bar.Y - S(12);
            _progress.Bounds = new Rect(statusLeft, _bar.Bottom - S(18), statusWidth, S(4));
            _statusArea = new Rect(statusLeft, _bar.Y + S(12), statusWidth, barHeight - S(24) - (busy ? S(14) : 0));
        }
        else
        {
            _bar = default;
            _statusArea = default;
        }

        // Details drawer, below the list while it's asked for.
        bool showLog = _showDetails && _details.Visible;
        if (showLog)
        {
            int logHeight = Math.Clamp((bottom - top) / 2, S(100), S(220));
            _log.Bounds = new Rect(pad, bottom - logHeight, width - pad * 2, logHeight);
            bottom -= logHeight + S(10);
        }
        _log.Visible = showLog;

        _list.FooterHeight = 0;
        _list.Bounds = new Rect(pad, top, width - pad * 2, bottom - top);
        var card = _list.Bounds;

        // While the list is empty, the ways to add files sit in the middle of it. Otherwise they're under Add at the top.
        _addFiles.Visible = _addFolder.Visible = _search.Visible = empty && _state.Activity != SessionActivity.Reading;
        if (_addFiles.Visible)
        {
            int[] widths = [_addFiles.PreferredWidth(), _addFolder.PreferredWidth(), _search.PreferredWidth()];
            int total = widths.Sum() + S(8) * 2;
            int y = card.Y + card.H / 2 + S(22);
            int left = card.X + (card.W - total) / 2;
            _addFiles.Bounds = new Rect(left, y, widths[0], S(36));
            _addFolder.Bounds = new Rect(_addFiles.Bounds.Right + S(8), y, widths[1], S(36));
            _search.Bounds = new Rect(_addFolder.Bounds.Right + S(8), y, widths[2], S(36));
        }
        if (readingAlone)
        {
            int stopWidth = Math.Max(_stop.PreferredWidth(), S(120));
            _stop.Bounds = new Rect(card.X + (card.W - stopWidth) / 2, card.Y + card.H / 2 + S(22), stopWidth, S(36));
        }

        WriteTestLayout();
    }

    private void WriteTestLayout()
    {
        if (_page is not null)
            return;
        TestLayout.Write(this, _list.TestParts().Append(("status", _statusArea, SecondLine())).Append(FirstLineForTest()), ("add", _add), ("options", _options), ("more", _more), ("compress", _compress), ("analyze", _analyze),
            ("stop", _stop), ("send", _send), ("details", _details), ("show_output", _showOutput), ("back_to_list", _backToList), ("add_files", _addFiles), ("add_folder", _addFolder), ("search", _search));
    }

    /// <summary>A page's card where the list goes, and the bar along the bottom with its buttons, the way the list has them.</summary>
    private void LayoutPage(int width, int height)
    {
        int pad = S(20);
        int top = S(62);
        int barHeight = S(84);
        _barShown = true;
        _bar = new Rect(pad, height - pad - barHeight, width - pad * 2, barHeight);
        _statusArea = default;
        _back.Bounds = new Rect(pad, S(16), S(36), S(34));
        var card = new Rect(pad, top, width - pad * 2, Math.Max(_bar.Y - S(12) - top, S(40)));
        _page!.Layout(card, _bar);
        TestLayout.Write(this, _page.TestParts, [("back", _back), .. _page.Named]);
    }

    private Rect PageCard => new(S(20), S(62), ClientWidth - S(40), Math.Max(_bar.Y - S(12) - S(62), S(40)));

    protected override void OnWheel(int delta) => _page?.OnWheel(delta);

    /// <summary>The bar's buttons from its right end, returning where the leftmost one starts.</summary>
    private int PlaceBarButtons(bool compact)
    {
        _details.Text = compact ? "" : "日志";
        _showOutput.Text = compact ? "" : "在文件夹中显示";
        // v19.1：发送是主操作，永远保留文字。折叠时把 Text 写成空串会留下一个只有图标的按钮，
        // 而 Text 是持久状态，只有下一次 Relayout 才恢复，所以看起来要「点一下」才完整显示。
        _send.Text = "发送到 Switch";
        int x = _bar.Right - S(16);
        int y = _bar.Y + (_bar.H - S(36)) / 2;
        foreach (var button in new[] { _stop, _compress, _backToList, _send, _analyze, _details, _showOutput })
        {
            if (!button.Visible)
                continue;
            // The main button keeps one width whatever it says, so it doesn't jump about as its words change.
            int w = button == _compress || button == _stop ? Math.Max(button.PreferredWidth(), S(176)) : button.PreferredWidth();
            x -= w;
            button.Bounds = new Rect(x, y, w, S(36));
            x -= S(8);
        }
        return x + S(8);
    }

    protected override void PaintWindow(Gfx g)
    {
        var p = Theme.P;
        int pad = S(20);
        if (_page is { CanGoBack: true } page)
        {
            // The way back, and where this is.
            g.Text(page.Title, new Rect(_back.Bounds.Right + S(10), S(16), ClientWidth - _back.Bounds.Right - S(10) - pad, S(34)), Fonts.Title, p.Text, TextFlags.Line);
        }
        else
        {
            // The program's name, or the welcome, which says it.
            string title = _page?.Title ?? AppInfo.Name;
            int titleWidth = TextWidth(title, Fonts.Title);
            g.Text(title, new Rect(pad + S(4), S(16), titleWidth + S(4), S(34)), Fonts.Title, p.Text);
            g.Text(AppInfo.Version, new Rect(pad + S(4) + titleWidth + S(10), S(21), S(60), S(30)), Fonts.Small, p.Faint);
        }
        if (_page is not null)
        {
            g.FillRound(_bar, S(10), p.Surface);
            _page.Paint(g, PageCard, _bar);
            return;
        }

        if (_state.KeyProblem is not null)
        {
            var banner = new Rect(pad, S(62), ClientWidth - pad * 2, S(56));
            g.FillRound(banner, S(8), p.WarnSurface);
            g.Icon(Glyph.Warning, new Rect(banner.X + S(12), banner.Y, S(24), banner.H), 16, p.Warn);
            int textWidth = _chooseKeys.Bounds.X - banner.X - S(56);
            g.Text("需要先提供密钥", new Rect(banner.X + S(46), banner.Y + S(8), textWidth, S(20)), Fonts.Strong, p.Text);
            g.Text("请把 prod.keys 放在本程序旁边，或直接拖到本窗口。", new Rect(banner.X + S(46), banner.Y + S(28), textWidth, S(18)), Fonts.Small, p.SubText);
        }

        if (_barShown)
        {
            g.FillRound(_bar, S(10), p.Surface);
            PaintStatus(g);
        }
    }

    /// <summary>The bar's height for what it says at a width: its line, and all of what's below it, however many lines that takes.</summary>
    private int BarHeight(int width)
    {
        var (first, _, firstFont, _) = StatusText();
        first = ProgressReporter.KeepFiguresWhole(first);
        string second = SecondLine();
        int said = (first.Length > 0 ? FirstHeight(first, firstFont, width) : 0) + (second.Length > 0 && width > 0 ? (first.Length > 0 ? S(2) : 0) + TextHeight(second, Fonts.Small, width) : 0);
        return Math.Max(S(84), said + S(24) + (_state.IsBusy ? S(14) : 0));
    }

    /// <summary>How tall the line that says what's going on is: one line while a job runs, where only a game's name may be shortened,
    /// and otherwise as many as it takes, so a summary is never cut short.</summary>
    private int FirstHeight(string first, Font font, int width) => _state.IsBusy || width <= 0 ? S(22) : Math.Max(S(22), TextHeight(first, font, width));

    /// <summary>For the test: the line that says what's going on, as wide as it needs and as tall as it's given.</summary>
    private (string, Rect, string) FirstLineForTest()
    {
        var (first, _, font, _) = StatusText();
        first = ProgressReporter.KeepFiguresWhole(first);
        return ("status_first", _statusArea with { W = TextWidth(first, font), H = FirstHeight(first, font, _statusArea.W) }, first);
    }

    private int _statusWidth;

    /// <summary>A line that says what's going on, and below it a word more, over as many lines as it needs.</summary>
    private void PaintStatus(Gfx g)
    {
        var p = Theme.P;
        var (first, firstColor, firstFont, _) = StatusText();
        first = ProgressReporter.KeepFiguresWhole(first);
        string second = SecondLine();
        var area = _statusArea;
        if (area.W <= 0 || area.H <= 0)
            return;
        int lineHeight = first.Length > 0 ? FirstHeight(first, firstFont, area.W) : S(22);
        int room = area.H - (first.Length > 0 ? lineHeight + S(2) : 0);
        int secondHeight = second.Length == 0 ? 0 : Math.Min(TextHeight(second, Fonts.Small, area.W), Math.Max(room, 0));
        // Measured against a line of the font itself: two lines of it come out under any fixed guess, and were then cut to one.
        bool oneLine = secondHeight <= TextHeight("Ag", Fonts.Small, area.W);
        int used = (first.Length > 0 ? lineHeight : 0) + (first.Length > 0 && secondHeight > 0 ? S(2) : 0) + secondHeight;
        int y = area.Y + Math.Max((area.H - used) / 2, 0);
        if (first.Length > 0)
        {
            g.Text(first, new Rect(area.X, y, area.W, lineHeight), firstFont, firstColor, lineHeight > S(22) ? TextFlags.Wrapped : TextFlags.Line);
            y += lineHeight + S(2);
        }
        if (secondHeight > 0)
            g.Text(second, new Rect(area.X, y, area.W, secondHeight), Fonts.Small, p.SubText, oneLine ? TextFlags.Line : TextFlags.Wrapped);
    }

    /// <summary>The word more below what's going on, as it's shown: wrapped where it needs to be, but never inside a figure.</summary>
    private string SecondLine() => ProgressReporter.KeepFiguresWhole(StatusText().second);

    private (string first, Rgb firstColor, Font firstFont, string second) StatusText()
    {
        var p = Theme.P;
        var s = _state;
        if (s.Activity == SessionActivity.Reading)
            return (s.ReadingText + "…", p.SubText, Fonts.Body, "");

        if (s.Activity == SessionActivity.Sending)
            return SendingText(s);

        // A job's own view: the game it's on and all of it together while it runs, which the bar shows. Once it's over, what it came to, and
        // where the games are once the list is gone back to.
        if (s.Job is { } job)
        {
            if (job.Running)
            {
                _progress.Fraction = job.Overall;
                string doing = s.CurrentCount > 1 ? $"{job.Doing} 第 {s.CurrentIndex} / {s.CurrentCount} 个：{s.CurrentTitle}" : $"{job.Doing} {s.CurrentTitle}";
                return (doing, p.Text, Fonts.Strong, $"总进度 {job.Overall * 100:0}%  ·  {job.Progress}");
            }
            var made = s.Rows.Where(r => r.State == RowState.Done).ToList();
            if (made.Count == 0)
                return (s.Summary ?? job.Headline("→").what, job.Outcome == UiLevel.Error ? p.Bad : p.Warn, Fonts.Strong,
                    job.Kind == SessionActivity.Verifying ? "检查结果列在每个游戏旁，也会写入日志。"
                        : "返回列表后，未完成的项目仍在「待处理」中。" + (job.Failed > 0 ? " 日志中记录了详细情况。" : ""));
            // A check changes nothing, so all there is to say is what it found.
            if (job.Kind == SessionActivity.Verifying)
                return (s.Summary ?? job.Headline("→").what, job.Outcome == UiLevel.Success ? p.Good : p.Bad, Fonts.Strong,
                    job.Failed > 0 ? "检查结果列在每个游戏旁，也会写入日志。" : "返回列表后，所有游戏保持原状。");
            if (job.Kind == SessionActivity.Extracting)
                return (s.Summary ?? job.Headline("→").what, p.Good, Fonts.Strong, "「在文件夹中显示」可打开文件所在位置。");
            string sizes = $"{ProgressReporter.Format(made.Sum(r => r.InputBytes))} → {ProgressReporter.Format(made.Sum(r => r.OutputBytes))}";
            if (job.Repacking)
                return (s.Summary ?? job.Headline("→").what, p.Good, Fonts.Strong, sizes + ".");
            return (job.SavedBytes > 0 ? $"已节省 {ProgressReporter.Format(job.SavedBytes)}" : job.Headline("→").what, job.Outcome == UiLevel.Success ? p.Good : p.Warn, Fonts.Strong,
                $"{sizes}。返回列表后，它们位于「已完成」中。");
        }

        if (s.IsBusy)
        {
            string name = s.Activity switch
            {
                SessionActivity.Analyzing => "正在分析",
                SessionActivity.Measuring => "正在测量",
                SessionActivity.Compressing => _session.Settings.Get("--nocompress") == "1" ? "正在重打包" : "正在压缩",
                SessionActivity.Extracting => "正在解包",
                _ => "正在校验",
            };
            string first = s.CurrentCount > 0 ? $"{name} 第 {s.CurrentIndex} / {s.CurrentCount} 个：{s.CurrentTitle}" : name + "…";
            string second = "";
            if (s.Progress is { } progress)
            {
                _progress.Fraction = ProgressFraction(s) ?? 1;
                var parts = new List<string> { progress.Label };
                if (progress.ShowSizes)
                    parts.Add($"{ProgressReporter.Format(progress.Done)} / {ProgressReporter.Format(progress.Total)}");
                if (progress.ShowSizes && progress.BytesPerSecond > 0)
                    parts.Add($"{progress.BytesPerSecond / 1048576:0} MB/s");
                if (Terminal.TimeLeft(progress) is { Length: > 0 } left)
                    parts.Add(left);
                second = string.Join("  ·  ", parts);
            }
            return (first, p.Text, Fonts.Strong, second);
        }

        string output = _session.Settings.Get("--nspdir") is { Length: > 0 } folder ? $"保存到 {folder}" : "保存在源文件旁";
        if (s.UpdateNote is { } note)
            return (note, p.Text, Fonts.Strong, output);
        if (s.Summary is not null)
        {
            var color = s.SummaryLevel switch { UiLevel.Success => p.Good, UiLevel.Warning => p.Warn, _ => p.Bad };
            // Where they are, without saying "saved" again under what was saved.
            string saved = _session.Settings.Get("--nspdir") is { Length: > 0 } nspFolder ? $"新文件位于 {nspFolder}" : "新文件位于原始文件旁";
            return (s.Summary, color, Fonts.Strong, s.LastOutput is not null ? saved + "." : output);
        }

        // Everything ticked is compressed already, so what's left is putting it on the Switch, which the big button then offers.
        if (_sendNext)
            return ("已准备好发送到 Switch", p.Text, Fonts.Strong, "「发送到 Switch」会把游戏共享给 Switch，由它从这里安装。");
        // Games a first look isn't sure of aren't ticked, and may save more than it seems: a thorough analysis reads them in full, and ticks the
        // ones worth it. Compressing only does what's ticked.
        int notSure = s.Rows.Count(r => r.NotSure && !r.Selected && !r.IsDone(BatchMode.Convert) && !r.IsFolder);
        string maybe = notSure == 0 ? "" : $"{notSure} 个游戏尚未确定，可能还能省更多：「完整分析」会完整读取它们的全部数据。";
        // On Done with nothing ticked, what compressing them came to, as on the Switch.
        int compressed = s.View == GamesShown.Done ? s.Rows.Count(r => r.IsCompressed || r.IsDone(BatchMode.Convert)) : 0;
        if (compressed > 0 && !s.Rows.Any(r => r.Selected))
        {
            long freedAll = s.Rows.Sum(r => r.State == RowState.Done && r.OutputBytes < r.InputBytes ? r.InputBytes - r.OutputBytes : r.Freed);
            return ($"{compressed} 个游戏已压缩{(freedAll > 0 ? $"  ·  共释放 {ProgressReporter.Format(freedAll)}" : "")}",
                p.Text, Fonts.Strong, $"勾选要解包或检查的游戏。  {output}。");
        }
        if (s.Rows.Length > 0 && !s.Rows.Any(r => r.Selected))
            return ("未勾选任何游戏", p.Text, Fonts.Strong, $"{maybe}勾选要处理的游戏。  {output}。");

        var known = s.Rows.Where(r => r.Selected && r.Estimate is not null).ToList();
        if (known.Count > 0 && known.All(r => r.NotSure))
        {
            // Ticked by hand: nothing to add up until they're measured, and compressing does them whatever they save.
            return (known.Count == 1 ? "尚未确定可节省多少" : "尚未确定可节省多少", p.Text, Fonts.Strong,
                $"已勾选，将按实际节省量压缩。  {output}。");
        }
        if (known.Count > 0)
        {
            // What they're sure to save, as the list has it, which is all an estimate can promise: a game can save more. One that isn't sure
            // yet counts as staying as it is.
            long before = known.Sum(r => r.InputBytes);
            long after = known.Sum(r => r.SureAfterBytes!.Value);
            bool atLeast = known.Any(r => r.Estimate!.AtLeast);
            string scope = known.Count == s.Rows.Length ? "" : $"（{s.Rows.Length} 个游戏中的 {known.Count} 个）";
            string change = after <= before ? $"{(atLeast ? "至少 " : "")}{TitleList.Percent(before, after)} 更小" : $"{TitleList.Percent(before, after).TrimStart('+')} 更大";
            string estimate = atLeast ? "这是估算值，实际可能省得更多。" : "";
            return ($"{ProgressReporter.Format(before)} → {ProgressReporter.Format(after)}，{change}{scope}", p.Text, Fonts.Strong, $"{estimate}{maybe}尚未写入任何文件。  {output}。");
        }

        return s.Rows.Length > 0
            ? (s.Rows.Count(r => r.Selected) is var ticked && ticked < s.Rows.Length
                ? $"已勾选 {ticked} / {s.Rows.Length} 个游戏，{ProgressReporter.Format(s.Rows.Where(r => r.Selected).Sum(r => r.InputBytes))}"
                : $"{s.Rows.Length} 个游戏，{ProgressReporter.Format(s.Rows.Sum(r => r.InputBytes))}", p.Text, Fonts.Strong,
                // In the word the big button goes by, which is Repack with compression off.
                $"可直接发送到 Switch，或在此{(_session.Settings.Get("--nocompress") == "1" ? "重打包" : "压缩")}它们。")
            : ("", p.Text, Fonts.Body, output + "。");
    }

    /// <summary>Everything about sending is said here: whether the Switch is there yet, how to get it there, and what it's reading.</summary>
    private (string first, Rgb firstColor, Font firstFont, string second) SendingText(SessionSnapshot s)
    {
        var p = Theme.P;
        if (s.Send is not { } send)
            return ("正在准备发送到 Switch…", p.Text, Fonts.Strong, "");
        int compressed = s.Rows.Count(r => r.IsCompressed);
        string sharing = $"正在共享 {s.Rows.Length} 个游戏{(compressed > 0 ? $"，其中 {compressed} 个已压缩" : "")}";
        if (send.Connected.Length == 0)
        {
            _progress.Fraction = 0;
            // How to get there, and only what's wrong with the cable if anything is: the network or a cable both just work.
            string how = send.UsbProblem is { } problem ? $"USB 数据线：{problem}" : $"请在 Switch 上打开 hacBrewPackLZ：这些游戏位于「游戏文件」，与 {send.Machine} 上一致。";
            return ($"{sharing}，等待 Switch 连接", p.Text, Fonts.Strong, how);
        }

        string connected = $"Switch 已通过 {string.Join(" 和 ", send.Connected)} 连接";
        if (send.Reading is not { } file || !send.Files.TryGetValue(file, out var sent))
        {
            _progress.Fraction = 0;
            var all = send.Files.Values;
            long done = all.Sum(f => f.read);
            return ($"{sharing}。{connected}", p.Text, Fonts.Strong, done > 0
                ? $"已读取 {ProgressReporter.Format(done)}。请在 Switch 上选择游戏进行安装。"
                : "在 Switch 上选择要安装的游戏，请保持本窗口打开直到完成。");
        }

        _progress.Fraction = sent.length > 0 ? (double)sent.read / sent.length : 0;
        var parts = new List<string> { $"{ProgressReporter.Format(sent.read)} / {ProgressReporter.Format(sent.length)}" };
        if (send.BytesPerSecond > 0)
        {
            parts.Add($"{send.BytesPerSecond / 1048576.0:0.0} MB/s");
            long left = (sent.length - sent.read) / Math.Max(send.BytesPerSecond, 1);
            if (left > 0)
                parts.Add(left >= 60 ? $"约剩 {(left + 30) / 60} 分钟" : $"约剩 {left} 秒");
        }
        parts.Add(connected);
        return ($"正在发送 {Path.GetFileName(file)}", p.Text, Fonts.Strong, string.Join("  ·  ", parts));
    }

    /// <summary>A game's own page: everything about it, and what can be done to it.</summary>
    private void OpenDetails(TitleRow row) => OpenPage(new DetailsPage(this, _session, row.Key));

    /// <summary>What can be done to one game, where it was right clicked: what its Details page has, a click sooner.</summary>
    private void GameMenu(TitleRow row, Rect at)
    {
        bool free = !_state.IsBusy || _state.Activity == SessionActivity.Measuring;
        var items = new List<(string, char, Action)> { ("详细信息", Glyph.Info, () => OpenDetails(row)) };
        if (!row.IsFolder)
            items.Add(("查看内部", Glyph.Search, () => LookInside(row)));
        if (free)
        {
            if (!row.IsDone(BatchMode.Convert) && !(Compressing && row.IsCompressed))
                items.Add((Compressing ? "压缩此游戏" : "重打包此游戏", Glyph.Download, () => _session.Compress(row.Key)));
            if (!row.IsFolder)
            {
                items.Add(("解包此游戏", Glyph.Folder, () => _session.Extract(row.Key)));
                items.Add(("检查此游戏是否损坏", Glyph.Check, () => _session.Check(row.Key)));
            }
        }
        if (row.OutputPath is { } output)
            items.Add(("在文件夹中显示", Glyph.FolderOpen, () => Explorer.Show(output)));
        if (free)
            items.Add(("从列表中移除", Glyph.Close, () => _session.Remove(row)));
        _ = new PopupList(this, at, items.ToArray());
    }

    /// <summary>Straight into a game, by way of its Details, which Back then goes to.</summary>
    private void LookInside(TitleRow row)
    {
        var details = new DetailsPage(this, _session, row.Key);
        OpenPage(details);
        details.OpenInsideWhenOpen();
    }

    /// <summary>The arrow on Compress: the other things to do to all the ticked games.</summary>
    private void TickedMenu(Rect button)
    {
        int ticked = _state.Rows.Count(r => r.Selected);
        if (ticked == 0 || _state.IsBusy && _state.Activity != SessionActivity.Measuring)
            return;
        string games = ticked == 1 ? "勾选的游戏" : $"勾选的 {ticked} 个游戏";
        var items = new (string, char, Action)[]
        {
            ($"解包 {games}", Glyph.Folder, () => _session.Extract()),
            ($"检查 {games} 是否损坏", Glyph.Check, () => _session.Check()),
            (ticked == 1 ? "重新分析" : "重新分析", Glyph.Search, () => _session.AnalyzeAgain()),
            (ticked == 1 ? "从列表中移除" : "从列表中移除", Glyph.Close, () => _session.RemoveTicked()),
        };
        _ = new PopupList(this, button with { X = button.Right - S(280), W = S(280) }, items);
    }

    private void AddFiles()
    {
        var files = FileDialogs.OpenFiles(Handle, "添加游戏文件", "游戏文件", GameArchive.DialogFilter, multiple: true);
        if (files.Count > 0)
            _session.AddInputs(files);
    }

    private void AddFolder()
    {
        var folders = FileDialogs.OpenFolders(Handle, "添加游戏文件夹，或要搜索游戏文件的目录", multiple: true);
        if (folders.Count > 0)
            _session.AddInputs(folders);
    }

    private void OpenSearch() => OpenPage(new SearchPage(this, files => _session.AddInputs(files)));

    private void ChooseKeys()
    {
        var files = FileDialogs.OpenFiles(Handle, "选择密钥文件", "密钥文件", "*.keys;*.txt;*.dat;*.ini", multiple: false);
        if (files.Count == 0)
            return;
        _session.Settings.Set("--keyset", files[0]);
        _session.Settings.Save();
        _session.ReloadKeys();
    }

    private void OpenOptions()
    {
        var store = new SettingsStore(_session);
        OpenPage(new FormPage(this, "选项", OptionCatalog.GeneralGroups, store, [new FormAction("已完成", ButtonStyle.Primary, () => null)],
            option =>
            {
                if (option == OptionCatalog.ShowWelcome)
                    OpenPage(new WelcomePage(this, _session));
                else
                    _session.CheckForUpdates();
            },
            option => option == OptionCatalog.CheckForUpdates ? _state.UpdateNote : null));
    }

    /// <summary>The app's own things, as the Switch's Help page has them, and clearing the list. What's done to games is on them, and on the
    /// arrow beside Compress for the ticked ones.</summary>
    private void OpenMore()
    {
        var items = new List<(string, char, Action)>();
        if (_state.Rows.Length + _state.Issues.Length > 0 && !_state.IsBusy)
            items.Add(("清空列表", Glyph.Close, () => _session.RemoveAll()));
        items.Add(("帮助", Glyph.Info, () => OpenPage(new HelpPage(this, _session.ReportText))));
        items.Add(("报告问题", Glyph.Send, () => OpenPage(new ReportPage(this, _session.ReportText))));
        items.Add(("检查新版本", Glyph.Download, () => _session.CheckForUpdates()));
        items.Add(("关于", Glyph.Info, ShowAbout));
        var anchor = _more.Bounds with { X = _more.Bounds.Right - S(230), W = S(230) };
        _ = new PopupList(this, anchor, items.ToArray());
    }

    /// <summary>The ways to add games, under the one button at the top. An empty list has them in its middle instead.</summary>
    private void OpenAddMenu()
    {
        var items = new (string, char, Action)[]
        {
            ("添加文件…", Glyph.Add, AddFiles),
            ("添加文件夹…", Glyph.Folder, AddFolder),
            ("搜索位置…", Glyph.Search, OpenSearch),
        };
        var anchor = _add.Bounds with { X = _add.Bounds.Right - S(210), W = S(210) };
        _ = new PopupList(this, anchor, items);
    }

    /// <summary>Shares every game on the list. The status line along the bottom says how it's going.</summary>
    private void SendToSwitch() => _session.SendToSwitch();

    private void ShowAbout() => OpenPage(new AboutPage(this));

    private void ShowLastOutput()
    {
        if (_state.LastOutput is { } path)
            Explorer.Show(path);
    }

    private void ToggleDetails()
    {
        _showDetails = !_showDetails;
        Relayout();
    }
}
