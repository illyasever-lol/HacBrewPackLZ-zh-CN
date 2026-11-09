namespace HacBrewPackLz;

/// <summary>The window people see: a list, and what can be done with it. Everything else is a click further away.</summary>
internal sealed class MainWindow : AppWindow
{
    private readonly Session _session;
    private readonly TitleList _list;
    private readonly Button _addFiles;
    private readonly Button _addFolder;
    private readonly Button _search;
    private readonly Button _clear;
    private readonly Button _options;
    private readonly Button _more;
    private readonly Button _chooseKeys;
    private readonly Button _analyze;
    private readonly Button _compress;
    private readonly Button _stop;
    private readonly Button _details;
    private readonly Button _showOutput;
    private readonly ProgressLine _progress;
    private readonly LogView _log;
    private int _refreshPending;
    private SessionSnapshot _state;
    private bool _showDetails;
    private bool _closeWhenIdle;
    private FormWindow? _optionsWindow;
    private Timer? _animation;
    /// Keeps the sending snapshot fresh after the Switch stops asking for data.
    private Timer? _sendTick;

    public MainWindow(Session session)
    {
        _session = session;
        _state = session.Snapshot();
        CreateTopLevel(AppInfo.Name, 1024, 768, null);

        _list = Add(new TitleList(row => _session.Toggle(row), all => _session.SelectAll(all), row => _session.Remove(row), issue => _session.Remove(issue), Explorer.Show,
            ShowNotes, ShowCompressed));
        _addFiles = Add(new Button("添加文件", ButtonStyle.Secondary, AddFiles, Glyph.Add));
        _addFolder = Add(new Button("添加文件夹", ButtonStyle.Secondary, AddFolder, Glyph.Folder));
        _search = Add(new Button("搜索位置", ButtonStyle.Secondary, OpenSearch, Glyph.Search));
        _clear = Add(new Button("清空", ButtonStyle.Ghost, () => _session.RemoveAll()));
        _options = Add(new Button("设置", ButtonStyle.Ghost, OpenOptions, Glyph.Settings));
        _more = Add(new Button("", ButtonStyle.Ghost, OpenMore, Glyph.More));
        _chooseKeys = Add(new Button("选择密钥", ButtonStyle.Secondary, ChooseKeys));
        _details = Add(new Button("详细信息", ButtonStyle.Ghost, ToggleDetails));
        _analyze = Add(new Button("分析", ButtonStyle.Secondary, () => _session.Analyze()));
        _compress = Add(new Button("开始压缩", ButtonStyle.Primary, PressCompress) { Menu = OpenCompressMenu });
        _stop = Add(new Button("停止", ButtonStyle.Secondary, () => _session.Stop()));
        _showOutput = Add(new Button("在文件夹中显示", ButtonStyle.Ghost, ShowLastOutput, Glyph.FolderOpen));
        _progress = Add(new ProgressLine());
        _log = Add(new LogView { Visible = false });

        session.Changed += () =>
        {
            if (Interlocked.Exchange(ref _refreshPending, 1) == 0)
                PostFromWorker();
        };
        DropTarget.Register(Handle, hover =>
        {
            _list.DropHover = hover;
            Invalidate();
        }, files => _session.AddInputs(files));

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

    /// A sending session raises no events at all once the Switch stops reading. Without
    /// this slow tick the last snapshot stays on screen forever, still showing a stale
    /// "正在发送 66%" and the name of a file that finished long ago. One refresh a second
    /// is plenty for the 8 second settle window and keeps the details log cheap: SetLines
    /// only appends lines, and does nothing at all when the count hasn't changed.
    private void TickSending(bool on)
    {
        if (on && _sendTick is null)
            _sendTick = new Timer(_ =>
            {
                Interlocked.Exchange(ref _refreshPending, 1);
                PostFromWorker();
            }, null, 1000, 1000);
        else if (!on && _sendTick is not null)
        {
            _sendTick.Dispose();
            _sendTick = null;
        }
    }

    protected override void OnThemeChanged() => _log.ApplyTheme();

    /// <summary>What else can be done with what's on the list, from the arrow at the end of the Compress button.</summary>
    private void OpenCompressMenu(Rect below)
    {
        bool repack = _session.Settings.Get("--nocompress") == "1";
        var items = new List<(string, char, Action)>
        {
            (repack ? "重新打包" : "开始压缩", Glyph.Download, PressCompress),
            ("发送到 Switch", Glyph.FolderOpen, SendToSwitch),
        };
        if (_state.Rows.Any(r => r.Selected && !r.IsFolder && !r.IsDone(BatchMode.Extract)))
            items.Add(("解包到文件夹", Glyph.Folder, () => _session.Extract()));
        int width = S(200);
        _ = new PopupList(this, below with { X = below.Right - width, W = width }, items.ToArray());
    }

    private void PressCompress()
    {
        if (!_state.Rows.Any(r => r.Selected && !r.IsDone(BatchMode.Convert)))
            return;
        var names = _session.TickedWithoutUpdate();
        if (names.Length > 0)
        {
            const int NamesShown = 6;
            string list = string.Join("\n", names.Take(NamesShown)) + (names.Length > NamesShown ? $"\n另有 {names.Length - NamesShown} 个" : "");
            int answer = Win32.MessageBoxW(Handle, $"关闭压缩后，重新打包只能靠把更新包并入本体来缩小体积，而以下游戏没有更新包：\n\n{list}\n\n是否取消勾选它们？\n\n选「否」则仍会一并重新打包。", AppInfo.Name, Win32.MB_YESNOCANCEL | Win32.MB_ICONINFORMATION);
            if (answer == Win32.IDCANCEL)
                return;
            if (answer == Win32.IDYES)
                _session.UntickWithoutUpdate();
        }
        _session.Compress();
    }

    protected override bool OnClosing()
    {
        if (!_state.IsBusy || _state.Activity is SessionActivity.Reading or SessionActivity.Analyzing)
            return true;
        if (Win32.MessageBoxW(Handle, "A job is still running. Stop it and close?\n\nTitles that have finished are kept.", AppInfo.Name, Win32.MB_YESNO | Win32.MB_ICONWARNING) != Win32.IDYES)
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
        if (key == Win32.VK_ESCAPE && _state.IsBusy)
        {
            _session.Stop();
            return true;
        }
        return false;
    }

    private void Refresh()
    {
        var state = _session.Snapshot();
        int ticked = state.Rows.Count(r => r.Selected);
        string compressLabel = (_session.Settings.Get("--nocompress") == "1" ? "重新打包" : "开始压缩") + (ticked < state.Rows.Length && ticked > 0 ? $" {ticked}" : "");
        bool layoutChanged = compressLabel != _compress.Text || state.Rows.Length > 0 != _state.Rows.Length > 0 || state.IsBusy != _state.IsBusy || state.Activity != _state.Activity
            || _list.IsEmpty != (state.Rows.Length == 0 && state.Issues.Length == 0 && state.Notes.Length == 0) || (state.Log.Length > 0) != (_state.Log.Length > 0)
            || (state.KeyProblem is null) != (_state.KeyProblem is null) || (state.Summary is null) != (_state.Summary is null)
            || (state.LastOutput is null) != (_state.LastOutput is null) || (state.Issues.Length == 0) != (_state.Issues.Length == 0);

        _state = state;
        _list.SetData(state);
        // The details drawer is hidden most of the time; feeding the native EDIT while
        // it's hidden only grows the text it has to re-wrap on the next resize.
        if (_log.Visible)
            _log.SetLines(state.Log);
        _compress.Text = compressLabel;
        UpdateEnabled();
        TaskbarProgress.Set(Handle, ProgressFraction(state));
        bool sliding = state.IsBusy && ProgressFraction(state) is null;
        _progress.Sliding = sliding;
        Animate(sliding);
        TickSending(state.Activity == SessionActivity.Sending);
        if (layoutChanged)
            Relayout();
        Invalidate();
        // It shows how checking for updates is going.
        _optionsWindow?.Invalidate();

        if (_closeWhenIdle && !state.IsBusy)
            Win32.DestroyWindow(Handle);
    }

    /// <summary>How far along the job is, for the bar along the bottom and the taskbar. While sending, that's the file the Switch is reading.</summary>
    private static double? ProgressFraction(SessionSnapshot state)
    {
        if (state.Activity == SessionActivity.Sending)
        {
            if (state.Send is { Reading: { } file } send && send.Files.TryGetValue(file, out var sent) && sent.length > 0)
                return (double)sent.read / sent.length;
            return null;
        }
        if (state.Activity is SessionActivity.Compressing or SessionActivity.Extracting or SessionActivity.Verifying or SessionActivity.Analyzing
            && state.Progress is { Total: > 0 } progress)
            return (double)progress.Done / progress.Total;
        return null;
    }

    private void UpdateEnabled()
    {
        bool idle = !_state.IsBusy;
        _addFiles.Enabled = _addFolder.Enabled = _search.Enabled = _options.Enabled = _more.Enabled = _clear.Enabled = idle;
        _analyze.Enabled = idle && _state.Rows.Any(r => r.Selected && r.NeedsEstimate);
        // Sending still works when everything ticked is already compressed.
        _compress.Enabled = idle && _state.Rows.Length > 0;
        _compress.MainEnabled = _state.Rows.Any(r => r.Selected && !r.IsDone(BatchMode.Convert));
    }

    protected override void Layout(int width, int height)
    {
        if (_list is null)
            return;

        int pad = S(20);
        int top = S(62);
        bool busy = _state.IsBusy;

        // Header
        int headerY = S(16);
        int buttonHeight = S(34);
        _more.Bounds = new Rect(width - pad - S(36), headerY, S(36), buttonHeight);
        int optionsWidth = _options.PreferredWidth();
        _options.Bounds = new Rect(_more.Bounds.X - optionsWidth - S(4), headerY, optionsWidth, buttonHeight);

        // Keys banner
        bool keysMissing = _state.KeyProblem is not null;
        _chooseKeys.Visible = keysMissing;
        if (keysMissing)
        {
            int chooseWidth = _chooseKeys.PreferredWidth();
            _chooseKeys.Bounds = new Rect(width - pad - S(12) - chooseWidth, top + S(11), chooseWidth, buttonHeight);
            top += S(56) + S(12);
        }

        // Bottom bar
        int barHeight = S(68);
        int barY = height - barHeight;
        _progress.Visible = busy;
        _progress.Bounds = new Rect(0, barY, width, S(3));

        int x = width - pad;
        void PlaceRight(Button button, bool visible)
        {
            button.Visible = visible;
            if (!visible)
                return;
            int w = button.PreferredWidth();
            x -= w;
            button.Bounds = new Rect(x, barY + (barHeight - S(36)) / 2, w, S(36));
            x -= S(8);
        }

        // Only what can be done next: nothing until there's something listed, and Analyze while something needs it.
        bool listed = _state.Rows.Length > 0;
        PlaceRight(_stop, busy);
        PlaceRight(_compress, !busy && listed);
        PlaceRight(_analyze, !busy && _state.Rows.Any(r => r.NeedsEstimate));
        PlaceRight(_details, busy || _state.Log.Length > 0);
        PlaceRight(_showOutput, !busy && _state.LastOutput is not null);
        _statusRight = x;

        // Details drawer, which is only there while its button is, then the list takes what's left.
        int listBottom = barY - S(8);
        bool showLog = _showDetails && _details.Visible;
        if (showLog)
        {
            int logHeight = Math.Clamp((listBottom - top) / 2, S(100), S(220));
            _log.Bounds = new Rect(pad, listBottom - logHeight, width - pad * 2, logHeight);
            listBottom -= logHeight + S(10);
        }
        _log.Visible = showLog;

        _list.FooterHeight = _list.IsEmpty ? 0 : 56;
        _list.Bounds = new Rect(pad, top, width - pad * 2, listBottom - top);
        var card = _list.Bounds;

        // The buttons that add things sit in the middle of an empty list, or along the bottom of a full one.
        bool empty = _list.IsEmpty;
        // While the first files are read, the list says so instead.
        _addFiles.Visible = _addFolder.Visible = _search.Visible = !(empty && _state.Activity == SessionActivity.Reading);
        int[] widths = [_addFiles.PreferredWidth(), _addFolder.PreferredWidth(), _search.PreferredWidth()];
        if (empty)
        {
            int total = widths.Sum() + S(8) * 2;
            int y = card.Y + card.H / 2 + S(22);
            int left = card.X + (card.W - total) / 2;
            _addFiles.Bounds = new Rect(left, y, widths[0], S(36));
            _addFolder.Bounds = new Rect(_addFiles.Bounds.Right + S(8), y, widths[1], S(36));
            _search.Bounds = new Rect(_addFolder.Bounds.Right + S(8), y, widths[2], S(36));
            _clear.Visible = false;
        }
        else
        {
            int y = card.Bottom - S(56) + (S(56) - S(34)) / 2;
            int left = card.X + S(12);
            _addFiles.Bounds = new Rect(left, y, widths[0], S(34));
            _addFolder.Bounds = new Rect(_addFiles.Bounds.Right + S(8), y, widths[1], S(34));
            _search.Bounds = new Rect(_addFolder.Bounds.Right + S(8), y, widths[2], S(34));
            _clear.Visible = true;
            int clearWidth = _clear.PreferredWidth();
            _clear.Bounds = new Rect(card.Right - S(12) - clearWidth, y, clearWidth, S(34));
        }
    }

    private int _statusRight;

    protected override void PaintWindow(Gfx g)
    {
        var p = Theme.P;
        int pad = S(20);
        int titleWidth = TextWidth(AppInfo.Name, Fonts.Title);
        g.Text(AppInfo.Name, new Rect(pad + S(4), S(16), titleWidth + S(4), S(34)), Fonts.Title, p.Text);
        g.Text(AppInfo.Version, new Rect(pad + S(4) + titleWidth + S(10), S(21), S(60), S(30)), Fonts.Small, p.Faint);

        if (_state.KeyProblem is not null)
        {
            var banner = new Rect(pad, S(62), ClientWidth - pad * 2, S(56));
            g.FillRound(banner, S(8), p.WarnSurface);
            g.Icon(Glyph.Warning, new Rect(banner.X + S(12), banner.Y, S(24), banner.H), 16, p.Warn);
            int textWidth = _chooseKeys.Bounds.X - banner.X - S(56);
            g.Text("需要先提供密钥", new Rect(banner.X + S(46), banner.Y + S(8), textWidth, S(20)), Fonts.Strong, p.Text);
            g.Text("请把 prod.keys 放在本程序旁边，或直接拖到本窗口。", new Rect(banner.X + S(46), banner.Y + S(28), textWidth, S(18)), Fonts.Small, p.SubText);
        }

        PaintStatus(g);
    }

    private void PaintStatus(Gfx g)
    {
        var p = Theme.P;
        int barHeight = S(68);
        int barY = ClientHeight - barHeight;
        var (first, firstColor, firstFont, second) = StatusText();
        var area = new Rect(S(24), barY, Math.Max(_statusRight - S(36), 0), barHeight);
        bool twoLines = second.Length > 0;
        if (first.Length > 0)
            g.Text(first, area with { Y = barY + (twoLines ? S(13) : 0), H = twoLines ? S(22) : barHeight }, firstFont, firstColor);
        if (twoLines)
            g.Text(second, area with { Y = barY + (first.Length > 0 ? S(36) : 0), H = first.Length > 0 ? S(18) : barHeight }, Fonts.Small, p.SubText, TextFlags.Line);
    }

    private (string first, Rgb firstColor, Font firstFont, string second) StatusText()
    {
        var p = Theme.P;
        var s = _state;
        if (s.Activity == SessionActivity.Reading)
            return (s.ReadingText + "…", p.SubText, Fonts.Body, "");

        if (s.Activity == SessionActivity.Sending)
            return SendingText(s);

        if (s.IsBusy)
        {
            string name = s.Activity switch
            {
                SessionActivity.Analyzing => "正在分析",
                SessionActivity.Compressing => _session.Settings.Get("--nocompress") == "1" ? "正在重打包" : "压缩中",
                SessionActivity.Extracting => "正在解包",
                _ => "正在校验",
            };
            string first = s.CurrentCount > 0 ? $"{name}  {s.CurrentIndex} / {s.CurrentCount}：{s.CurrentTitle}" : name + "…";
            string second = "";
            if (s.Progress is { } progress)
            {
                _progress.Fraction = ProgressFraction(s) ?? 1;
                var parts = new List<string> { progress.Label };
                if (progress.ShowSizes)
                    parts.Add($"{ProgressReporter.Format(progress.Done)} of {ProgressReporter.Format(progress.Total)}");
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
            return (s.Summary, color, Fonts.Strong, s.LastOutput ?? output);
        }

        if (s.Rows.Length > 0 && !s.Rows.Any(r => r.Selected))
            return ("未勾选任何游戏", p.Text, Fonts.Strong, $"请勾选要处理的游戏。  {output}。");

        var known = s.Rows.Where(r => r.Selected && r.Estimate is not null).ToList();
        if (known.Count > 0)
        {
            long before = known.Sum(r => r.InputBytes);
            long after = known.Sum(r => r.Estimate!.OutputBytes);
            long margin = (long)Math.Sqrt(known.Sum(r => (double)r.Estimate!.MarginBytes * r.Estimate.MarginBytes));
            string scope = known.Count == s.Rows.Length ? "" : $"（{known.Count}/{s.Rows.Length} 个标题）";
            string change = after <= before ? $"减少 {TitleList.Percent(before, after)}" : $"增加 {TitleList.Percent(before, after).TrimStart('+')}";
            string about = margin > 0 ? "约" : "";
            string estimate = margin > 0 ? $"估算误差约 {ProgressReporter.Format(margin)}，" : "";
            return ($"{ProgressReporter.Format(before)} → {about}{ProgressReporter.Format(after)}，{change}{scope}", p.Text, Fonts.Strong, $"{estimate}未写入任何文件。  {output}。");
        }

        return s.Rows.Length > 0
            ? ($"{s.Rows.Length} 个标题，{ProgressReporter.Format(s.Rows.Sum(r => r.InputBytes))}", p.Text, Fonts.Strong, $"预估压缩后的体积，不会写入任何文件。  {output}。")
            : ("", p.Text, Fonts.Body, output + ".");
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
            string how = $"在 Switch 上打开 hacBrewPackLZ：这些游戏会显示在「游戏文件」中，位于 {send.Machine}。";
            string cable = send.UsbProblem is { } problem ? $"  通过数据线：{problem}" : "Switch 可以位于同一网络，也可以通过 USB 数据线连接。";
            return ($"{sharing}，等待 Switch 连接", p.Text, Fonts.Strong, how + cable);
        }

        string connected = $"Switch 已连接 {string.Join("、", send.Connected)}";
        if (send.Reading is not { } file || !send.Files.TryGetValue(file, out var sent))
        {
            _progress.Fraction = 0;
            var all = send.Files.Values;
            long done = all.Sum(f => f.read);
            return ($"{sharing}. {connected}", p.Text, Fonts.Strong, done > 0
                ? $"已读取 {ProgressReporter.Format(done)}。请在 Switch 上选择要安装的游戏。"
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
        parts.Add(connected.Replace("Switch 已连接", "已连接"));
        return ($"正在发送 {Path.GetFileName(file)}", p.Text, Fonts.Strong, string.Join("  ·  ", parts));
    }

    /// <summary>What there is to know about a title, below its info button.</summary>
    private void ShowNotes(TitleRow row, Rect button)
    {
        _ = new NotePopup(this, button, row.Notes);
    }

    private void ShowCompressed(bool show)
    {
        _session.Settings.Set("--showcompressed", show ? "1" : "0");
        _session.Settings.Save();
        _session.OptionChanged("--showcompressed");
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

    private void OpenSearch() => new SearchWindow(this, files => _session.AddInputs(files)).ShowModalOver(this);

    private void ChooseKeys()
    {
        var files = FileDialogs.OpenFiles(Handle, "选择密钥文件", "密钥", "*.keys;*.txt;*.dat;*.ini", multiple: false);
        if (files.Count == 0)
            return;
        _session.Settings.Set("--keyset", files[0]);
        _session.Settings.Save();
        _session.ReloadKeys();
    }

    private void OpenOptions()
    {
        var store = new SettingsStore(_session);
        _optionsWindow = new FormWindow(this, "设置", OptionCatalog.GeneralGroups, store, [new FormAction("已完成", ButtonStyle.Primary, () => null)],
            _ => _session.CheckForUpdates(), _ => _state.UpdateNote);
        _optionsWindow.ShowModalOver(this);
    }

    private void OpenMore()
    {
        var items = new (string, char, Action)[]
        {
            ("校验 NSP…", Glyph.Check, OpenVerify),
            ("关于", Glyph.Info, ShowAbout),
        };
        var anchor = _more.Bounds with { X = _more.Bounds.Right - S(210), W = S(210) };
        _ = new PopupList(this, anchor, items);
    }

    /// <summary>Shares every game on the list. The status line along the bottom says how it's going.</summary>
    private void SendToSwitch() => _session.SendToSwitch();

    private void OpenVerify()
    {
        var store = new LocalStore();
        var group = new OptionGroup("校验 NSP",
        [
            new OptionSpec("nsp", "NSP 文件", OptionKind.File, OptionUse.Build, "读取校验哈希、块和表是否为原始文件") { Placeholder = "选择文件" },
            new OptionSpec("romfs", "与 RomFS 文件夹比对", OptionKind.Folder, OptionUse.Build, "可选。把这些文件与游戏数据逐字节比对。") { Placeholder = "无" },
            new OptionSpec("exefs", "与 ExeFS 文件夹比对", OptionKind.Folder, OptionUse.Build, "可选。把这些文件与游戏数据逐字节比对。") { Placeholder = "无" },
        ]);
        var actions = new[]
        {
            new FormAction("校验", ButtonStyle.Primary, () =>
            {
                string nsp = store.Get("nsp");
                if (nsp.Length == 0)
                    return "请先选择 NSP 文件。";
                _session.Verify(nsp, EmptyToNull(store.Get("romfs")), EmptyToNull(store.Get("exefs")));
                return null;
            }),
        };
        new FormWindow(this, "校验 NSP", [group], store, actions).ShowModalOver(this);
    }

    private static string? EmptyToNull(string value) => value.Length > 0 ? value : null;

    private void ShowAbout() => new AboutWindow(this).ShowModalOver(this);

    private void ShowLastOutput()
    {
        if (_state.LastOutput is { } path)
            Explorer.Show(path);
    }

    private void ToggleDetails()
    {
        _showDetails = !_showDetails;
        Relayout();
        if (_log.Visible)
            _log.SetLines(_state.Log);
    }
}
