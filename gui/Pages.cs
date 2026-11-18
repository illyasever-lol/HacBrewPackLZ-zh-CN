namespace HacBrewPackLz;

/// <summary>
/// A screen of the main window in place of the list, as on the Switch: the welcome, the options, checking an NSP, searching a
/// location and what this is. It has the card where the list goes and the bar along the bottom, and its widgets go with it.
/// </summary>
internal abstract class Page(MainWindow window)
{
    private readonly List<Widget> _widgets = [];

    protected MainWindow Window => window;

    /// <summary>What it says at the top, beside the way back.</summary>
    public abstract string Title { get; }

    /// <summary>Whether Back and Esc leave it. The welcome only goes with Start.</summary>
    public virtual bool CanGoBack => true;

    /// <summary>Back and Esc go somewhere other than the list: Look inside goes back to the game's Details. False to go to the list.</summary>
    public virtual bool GoBack() => false;

    /// <summary>Its buttons, by name, for the tests. See <see cref="TestLayout"/>.</summary>
    public virtual (string name, Widget widget)[] Named => [];

    /// <summary>Other parts of it, by name, for the tests.</summary>
    public virtual IEnumerable<(string name, Rect bounds, string text)> TestParts => [];

    protected T Add<T>(T widget) where T : Widget
    {
        _widgets.Add(widget);
        return window.Add(widget);
    }

    protected int S(int value) => window.S(value);

    public abstract void Layout(Rect card, Rect bar);

    /// <summary>The bar's background is drawn already. What goes in the card, and in the bar beside the buttons, is the page's.</summary>
    public abstract void Paint(Gfx g, Rect card, Rect bar);

    public virtual bool OnKey(int key) => false;

    public virtual void OnWheel(int delta) { }

    /// <summary>Something it started on another thread has news.</summary>
    public virtual void OnWorker() { }

    /// <summary>It's being left: whatever it was doing stops, and whatever was typed is kept.</summary>
    public virtual void Leaving() { }

    public void RemoveWidgets()
    {
        foreach (var widget in _widgets)
            window.Remove(widget);
        _widgets.Clear();
    }

    /// <summary>The main button at the right end of the bar, as wide as Compress is, and the next one along to the left of it.</summary>
    protected int PlaceBarButtons(Rect bar, params Button[] fromTheRight)
    {
        int x = bar.Right - S(16);
        int y = bar.Y + (bar.H - S(36)) / 2;
        for (int i = 0; i < fromTheRight.Length; i++)
        {
            var button = fromTheRight[i];
            if (!button.Visible)
                continue;
            int w = i == 0 ? Math.Max(button.PreferredWidth(), S(176)) : button.PreferredWidth();
            x -= w;
            button.Bounds = new Rect(x, y, w, S(36));
            x -= S(8);
        }
        return x + S(8);
    }

    /// <summary>Words in the bar, between its left end and the buttons, in the middle of it if they fit on one line.</summary>
    protected void PaintBarText(Gfx g, Rect bar, int left, int right, string text, Rgb color)
    {
        var area = new Rect(left, bar.Y + S(12), Math.Max(right - S(24) - left, 0), bar.H - S(24));
        if (window.TextWidth(text, Fonts.Body) <= area.W)
            g.Text(text, new Rect(area.X, area.Y + (area.H - S(22)) / 2, area.W, S(22)), Fonts.Body, color, TextFlags.Line);
        else
            g.Text(text, area, Fonts.Small, color, TextFlags.Wrapped);
    }
}

/// <summary>Options as a page: one group, or several with a list of them down the side of the card. Checking an NSP is one too.</summary>
internal sealed class FormPage : Page
{
    private const int SidebarWidth = 200;

    private readonly string _title;
    private readonly OptionGroup[] _groups;
    private readonly IFormStore _store;
    private readonly OptionRows _rows;
    private readonly List<Button> _pages = [];
    private readonly List<Button> _actions = [];
    private readonly Button? _reset;
    private readonly ScrollMath _scroll = new();
    private int _page;
    private string? _actionError;
    private Rect _view;
    private Rect _sidebar;

    /// <param name="actions">From the right end of the bar, the first the main one. One that returns nothing goes back to the list.</param>
    /// <param name="run">Does what an action among the options is for.</param>
    /// <param name="outcome">How an action went, shown beside its button.</param>
    public FormPage(MainWindow window, string title, OptionGroup[] groups, IFormStore store, FormAction[] actions,
        Action<OptionSpec>? run = null, Func<OptionSpec, string?>? outcome = null) : base(window)
    {
        _title = title;
        _groups = groups;
        _store = store;
        _rows = new OptionRows(window, store, run, outcome, changed: option =>
        {
            if (OptionCatalog.DecidesShown(option))
                ShowPage(_page, keepScroll: true);
        });
        if (groups.Length > 1)
        {
            for (int i = 0; i < groups.Length; i++)
            {
                int page = i;
                _pages.Add(Add(new Button(groups[i].Title, ButtonStyle.Ghost, () => ShowPage(page)) { AlignLeft = true }));
            }
        }
        if (store is SettingsStore)
            _reset = Add(new Button("重置本页", ButtonStyle.Ghost, ResetPage));
        foreach (var action in actions)
        {
            _actions.Add(Add(new Button(action.Text, action.Style, () =>
            {
                _rows.CommitFocused();
                if (_rows.Error is not null)
                    return;
                _actionError = action.Run();
                if (_actionError is null)
                    window.ClosePage();
                else
                    window.Invalidate();
            })));
        }
        ShowPage(0);
    }

    public override string Title => _title;

    public override (string name, Widget widget)[] Named =>
        [.. _actions.Select(a => (a.Text.ToLowerInvariant(), (Widget)a)), .. _reset is null ? [] : new[] { ("reset", (Widget)_reset) },
         .. _pages.Select((p, i) => ($"section_{i}", (Widget)p)), .. _rows.Named];

    private void ShowPage(int page, bool keepScroll = false)
    {
        _page = page;
        _actionError = null;
        _rows.Show(_groups[page].Options.Where(o => OptionCatalog.Shown(o, _store.Get)));
        if (!keepScroll)
        {
            _scroll.Measure(0, 0);
            _scroll.ScrollBy(int.MinValue / 2);
        }
        Window.Relayout();
        Window.Invalidate();
    }

    private void ResetPage()
    {
        foreach (var option in _groups[_page].Options.Where(o => o.Kind != OptionKind.Action))
            _store.Set(option, "");
        ShowPage(_page);
    }

    public override void Leaving()
    {
        _rows.CommitFocused();
        _rows.Clear();
    }

    public override void OnWheel(int delta)
    {
        _scroll.ScrollBy(-delta * S(OptionRows.MinimumRowHeight) / 120);
        Window.Relayout();
        Window.Invalidate();
    }

    public override void Layout(Rect card, Rect bar)
    {
        int left = card.X;
        if (_pages.Count > 0)
        {
            _sidebar = new Rect(card.X, card.Y, S(SidebarWidth), card.H);
            for (int i = 0; i < _pages.Count; i++)
                _pages[i].Bounds = new Rect(card.X + S(10), card.Y + S(12) + i * S(38), S(SidebarWidth) - S(20), S(34));
            left = _sidebar.Right;
        }
        // Below the heading of the section, which stays where it is while the options scroll. With only the one, what it's called is at the top already.
        int heading = _pages.Count > 0 ? S(56) : S(10);
        _view = new Rect(left, card.Y + heading, card.Right - left, card.H - heading - S(2));
        int rowsLeft = _view.X + S(24);
        int rowsRight = Math.Min(_view.Right - S(28), rowsLeft + S(820));
        _scroll.Measure(_rows.Measure(rowsLeft, rowsRight) + S(8), _view.H);
        _rows.Place(_view.Y - _scroll.Offset, _view);

        int x = PlaceBarButtons(bar, [.. _actions]);
        if (_reset is not null)
            _reset.Bounds = new Rect(bar.X + S(16), bar.Y + (bar.H - S(36)) / 2, _reset.PreferredWidth(), S(36));
        _barTextRight = x;
    }

    private int _barTextRight;

    public override void Paint(Gfx g, Rect card, Rect bar)
    {
        var p = Theme.P;
        g.FillRound(card, S(10), p.Surface);
        if (_pages.Count > 0)
        {
            g.Fill(new Rect(_sidebar.Right - 1, card.Y + S(10), 1, card.H - S(20)), p.Border);
            for (int i = 0; i < _pages.Count; i++)
            {
                if (i != _page)
                    continue;
                var b = _pages[i].Bounds;
                g.FillRound(b, S(6), p.Hover);
                g.FillRound(new Rect(b.X, b.Y + S(8), S(3), b.H - S(16)), S(2), p.Accent);
            }
        }
        if (_pages.Count > 0)
        {
            g.Text(_groups[_page].Title, new Rect(_view.X + S(24), card.Y + S(14), _view.W - S(48), S(30)), Fonts.Heading, p.Text);
            g.Fill(new Rect(_view.X + S(16), _view.Y - 1, _view.W - S(32), 1), p.Border);
        }

        g.Clip(_view);
        _rows.Paint(g);
        g.ResetClip();
        _scroll.Paint(g, new Rect(card.Right - S(8), _view.Y + S(4), S(4), _view.H - S(8)), false);

        if ((_rows.Error ?? _actionError) is { } error)
        {
            int left = _reset is not null ? _reset.Bounds.Right + S(16) : bar.X + S(20);
            PaintBarText(g, bar, left, _barTextRight, error, p.Bad);
        }
    }
}

/// <summary>The first time it opens: what it's for and the few options most people want a say in, until Start.</summary>
internal sealed class WelcomePage : Page
{
    private readonly OptionRows _rows;
    private readonly Button _start;
    private readonly ScrollMath _scroll = new();
    private readonly Session _session;
    private Rect _view;
    private int _left;
    private int _width;

    public WelcomePage(MainWindow window, Session session) : base(window)
    {
        _session = session;
        _rows = new OptionRows(window, new SettingsStore(session), brief: true);
        _rows.Show(OptionCatalog.Welcome.Options.Where(o => OptionCatalog.Shown(o, flag => session.Settings.Get(flag))));
        _start = Add(new Button("开始", ButtonStyle.Primary, window.ClosePage));
    }

    /// <summary>Said at the top of the window, where its name goes, so the card has room for everything without scrolling.</summary>
    public override string Title => OptionCatalog.Welcome.Title;
    public override bool CanGoBack => false;
    public override (string name, Widget widget)[] Named => [("start", _start)];

    /// <summary>How far it would scroll, which at the window's first size should be nothing.</summary>
    public override IEnumerable<(string name, Rect bounds, string text)> TestParts => [("welcome_overflow", _view, _scroll.Max.ToString())];

    /// <summary>Only Start, or games dropped on it, get here: closing the program on the welcome brings it back next time.</summary>
    public override void Leaving()
    {
        _rows.CommitFocused();
        _rows.Clear();
        _session.Settings.Welcomed = true;
        _session.Settings.Save();
    }

    public override void OnWheel(int delta)
    {
        _scroll.ScrollBy(-delta * S(OptionRows.MinimumRowHeight) / 120);
        Window.Relayout();
        Window.Invalidate();
    }

    /// <summary>What it is, the one thing most people don't know, standing out, and what to watch out for, down to the line under them.</summary>
    private int IntroHeight() =>
        S(20) + Window.TextHeight(OptionCatalog.WelcomeIntro, Fonts.Body, _width) + S(12)
        + Window.TextHeight(OptionCatalog.WelcomeTip, Fonts.Strong, _width) + S(12)
        + Window.TextHeight(OptionCatalog.WelcomeWarning, Fonts.Small, _width) + S(22) + S(8) + S(18);

    public override void Layout(Rect card, Rect bar)
    {
        PlaceBarButtons(bar, _start);
        _view = new Rect(card.X, card.Y + S(2), card.W, card.H - S(4));
        _width = Math.Min(card.W - S(64), S(760));
        _left = card.X + (card.W - _width) / 2;
        int rows = _rows.Measure(_left, _left + _width);
        int intro = IntroHeight();
        _scroll.Measure(intro + rows + S(6), _view.H);
        _rows.Place(card.Y + intro - _scroll.Offset, _view);
    }

    public override void Paint(Gfx g, Rect card, Rect bar)
    {
        var p = Theme.P;
        g.FillRound(card, S(10), p.Surface);
        g.Clip(_view);
        int y = card.Y + S(20) - _scroll.Offset;
        const TextFlags Wrapping = TextFlags.WordBreak | TextFlags.EditControl | TextFlags.NoPrefix;
        int introHeight = Window.TextHeight(OptionCatalog.WelcomeIntro, Fonts.Body, _width);
        g.Text(OptionCatalog.WelcomeIntro, new Rect(_left, y, _width, introHeight), Fonts.Body, p.SubText, Wrapping);
        y += introHeight + S(12);
        // The one thing to take away, in the accent, with a bar beside it.
        int tipHeight = Window.TextHeight(OptionCatalog.WelcomeTip, Fonts.Strong, _width);
        g.FillRound(new Rect(_left - S(12), y, S(3), tipHeight), S(2), p.Accent);
        g.Text(OptionCatalog.WelcomeTip, new Rect(_left, y, _width, tipHeight), Fonts.Strong, p.Text, Wrapping);
        y += tipHeight + S(12);
        int warningHeight = Window.TextHeight(OptionCatalog.WelcomeWarning, Fonts.Small, _width);
        g.Text(OptionCatalog.WelcomeWarning, new Rect(_left, y, _width, warningHeight), Fonts.Small, p.Warn, Wrapping);
        y += warningHeight + S(8);
        g.Text("简体中文汉化版 · 汉化作者：illyasever", new Rect(_left, y, _width, S(22)), Fonts.Small, p.SubText, TextFlags.Line);
        y += S(22) + S(8);
        g.Fill(new Rect(_left, y, _width, 1), p.Border);
        _rows.Paint(g);
        g.ResetClip();
        _scroll.Paint(g, new Rect(card.Right - S(8), card.Y + S(4), S(4), card.H - S(8)), false);

        PaintBarText(g, bar, bar.X + S(20), _start.Bounds.X, _rows.Error ?? OptionCatalog.WelcomeLater, _rows.Error is null ? p.SubText : p.Bad);
    }
}

/// <summary>Looks through a location for game files, to pick which ones to add.</summary>
internal sealed class SearchPage : Page
{
    private static string? s_lastLocation;

    private readonly Action<List<string>> _add;
    private readonly TextField _location;
    private readonly TextField _filter;
    private readonly Button _browse;
    private readonly Button _go;
    private readonly FoundList _list;
    private readonly Button _selectAll;
    private readonly Button _addButton;
    private readonly object _gate = new();
    private readonly List<FoundFile> _found = [];
    private CancellationTokenSource? _stop;
    private volatile bool _searching;
    private bool _stopped;
    private int _refreshPending;
    private string _status = "";

    public SearchPage(MainWindow window, Action<List<string>> add) : base(window)
    {
        _add = add;
        _location = Add(new TextField("要搜索的文件夹或驱动器"));
        _location.Value = s_lastLocation ?? Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), "Downloads");
        _browse = Add(new Button("浏览", ButtonStyle.Secondary, Browse));
        // Search, or Stop while it's searching, as a whole drive can take a while.
        _go = Add(new Button("搜索", ButtonStyle.Secondary, () =>
        {
            if (_searching)
                StopSearch();
            else
                StartSearch();
        }));
        _filter = Add(new TextField("按名称筛选"));
        _filter.Changed = ApplyFilter;
        _list = Add(new FoundList(UpdateAddButton));
        _selectAll = Add(new Button("全选", ButtonStyle.Ghost, ToggleAll));
        _addButton = Add(new Button("添加", ButtonStyle.Primary, AddChosen));
        StartSearch();
    }

    public override string Title => "搜索位置";
    public override (string name, Widget widget)[] Named => [("location", _location), ("search_go", _go), ("filter", _filter), ("add_chosen", _addButton), ("select_all", _selectAll)];

    public override bool OnKey(int key)
    {
        if (key == Win32.VK_RETURN && Window.Focused == _location)
        {
            StartSearch();
            return true;
        }
        return false;
    }

    public override void Leaving() => _stop?.Cancel();

    public override void OnWorker()
    {
        if (Interlocked.Exchange(ref _refreshPending, 0) == 1)
            ApplyFilter();
    }

    public override void Layout(Rect card, Rect bar)
    {
        int inset = S(20);
        int y = card.Y + inset;
        int right = card.Right - inset;
        int goWidth = _go.PreferredWidth();
        int browseWidth = _browse.PreferredWidth();
        _go.Bounds = new Rect(right - goWidth, y, goWidth, S(34));
        _browse.Bounds = new Rect(_go.Bounds.X - S(8) - browseWidth, y, browseWidth, S(34));
        _location.Bounds = new Rect(card.X + inset, y, _browse.Bounds.X - S(8) - card.X - inset, S(34));
        y += S(42);
        _filter.Bounds = new Rect(card.X + inset, y, card.W - inset * 2, S(34));
        y += S(46);
        _list.Bounds = new Rect(card.X + S(8), y, card.W - S(16), card.Bottom - S(8) - y);

        _barLeft = bar.X + S(16) + _selectAll.PreferredWidth() + S(8);
        _barRight = PlaceBarButtons(bar, _addButton);
        _selectAll.Bounds = new Rect(bar.X + S(16), bar.Y + (bar.H - S(36)) / 2, _selectAll.PreferredWidth(), S(36));
    }

    private int _barLeft;
    private int _barRight;

    public override void Paint(Gfx g, Rect card, Rect bar)
    {
        var p = Theme.P;
        g.FillRound(card, S(10), p.Surface);
        PaintBarText(g, bar, _barLeft + S(8), _barRight, _status, p.SubText);
    }

    private void Browse()
    {
        if (FileDialogs.OpenFolders(Window.Handle, "选择搜索位置", multiple: false).FirstOrDefault() is not { } folder)
            return;
        _location.Value = folder;
        StartSearch();
    }

    private void StartSearch()
    {
        _stop?.Cancel();
        _stopped = false;
        string root = _location.Value.Trim().Trim('"');
        s_lastLocation = root;
        lock (_gate)
            _found.Clear();
        _list.SetFiles([]);
        if (!Directory.Exists(root))
        {
            _searching = false;
            UpdateAddButton();
            _status = root.Length > 0 ? $"'{root}' 不是可用的文件夹。" : "";
            Window.Invalidate();
            return;
        }

        var stop = new CancellationTokenSource();
        _stop = stop;
        _searching = true;
        UpdateAddButton();
        var worker = new Thread(() => Search(root, stop.Token)) { IsBackground = true, Name = "搜索" };
        worker.Start();
    }

    private void StopSearch()
    {
        _stop?.Cancel();
        _searching = false;
        _stopped = true;
        UpdateAddButton();
    }

    private void Search(string root, CancellationToken token)
    {
        var options = new EnumerationOptions
        {
            RecurseSubdirectories = true,
            IgnoreInaccessible = true,
            MatchType = MatchType.Simple,
            MatchCasing = MatchCasing.CaseInsensitive,
            AttributesToSkip = FileAttributes.ReparsePoint | FileAttributes.Offline,
        };
        try
        {
            foreach (string path in Directory.EnumerateFiles(root, "*", options))
            {
                if (token.IsCancellationRequested)
                    return;
                if (!GameArchive.IsGameFile(path))
                    continue;
                long size;
                try
                {
                    size = new FileInfo(path).Length;
                }
                catch (IOException)
                {
                    continue;
                }
                bool compressed = Path.GetFileName(path).Contains("[LZ4]", StringComparison.OrdinalIgnoreCase);
                lock (_gate)
                    _found.Add(new FoundFile(path, size) { Chosen = !compressed });
                PostRefresh();
            }
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
            // Whatever was found until now is still useful.
        }
        if (token.IsCancellationRequested)
            return;
        _searching = false;
        PostRefresh();
    }

    private void PostRefresh()
    {
        if (!Window.IsClosed && Interlocked.Exchange(ref _refreshPending, 1) == 0)
            Window.PostFromWorker();
    }

    private void ApplyFilter()
    {
        string filter = _filter.Value.Trim();
        List<FoundFile> files;
        lock (_gate)
            files = _found.Where(f => filter.Length == 0 || f.Path.Contains(filter, StringComparison.OrdinalIgnoreCase)).ToList();
        _list.SetFiles(files);
        UpdateAddButton();
    }

    private void UpdateAddButton()
    {
        _go.Text = _searching ? "停止" : "搜索";
        var visible = _list.Files;
        int chosen = visible.Count(f => f.Chosen);
        _addButton.Text = chosen == 1 ? "添加 1 个文件" : $"添加 {chosen} 个文件";
        _addButton.Enabled = chosen > 0;
        _selectAll.Text = visible.Count > 0 && chosen == visible.Count ? "全不选" : "全选";
        long bytes = visible.Where(f => f.Chosen).Sum(f => f.Size);
        _status = (_searching ? "正在搜索…  " : _stopped ? "已停止。  " : "") + $"找到 {visible.Count} 个" + (chosen > 0 ? $"，已选择 {ProgressReporter.Format(bytes)}" : "");
        Window.Relayout();
        Window.Invalidate();
    }

    private void ToggleAll()
    {
        var visible = _list.Files;
        bool all = visible.All(f => f.Chosen);
        foreach (var file in visible)
            file.Chosen = !all;
        UpdateAddButton();
    }

    private void AddChosen()
    {
        var chosen = _list.Files.Where(f => f.Chosen).Select(f => f.Path).ToList();
        if (chosen.Count == 0)
            return;
        Window.ClosePage();
        _add(chosen);
    }
}

/// <summary>What people most often need to know, a topic at a time with where to read the rest, and reporting a problem.</summary>
internal sealed class HelpPage : Page
{
    private const int SidebarWidth = 230;

    private readonly List<Button> _topics = [];
    private readonly Button _more;
    private readonly Button _report;
    private readonly Button _done;
    private readonly Func<string> _reportText;
    private int _topic;
    private Rect _sidebar;
    private Rect _view;

    public HelpPage(MainWindow window, Func<string> reportText) : base(window)
    {
        _reportText = reportText;
        for (int i = 0; i < Help.Topics.Length; i++)
        {
            int topic = i;
            _topics.Add(Add(new Button(Help.Topics[i].Title, ButtonStyle.Ghost, () => ShowTopic(topic)) { AlignLeft = true }));
        }
        _more = Add(new Button("在线阅读更多", ButtonStyle.Secondary, () => Explorer.OpenWebPage(Help.Topics[_topic].Url)));
        _report = Add(new Button("报告问题", ButtonStyle.Secondary, Report));
        _done = Add(new Button("已完成", ButtonStyle.Primary, window.ClosePage));
    }

    public override string Title => "帮助";
    public override (string name, Widget widget)[] Named => [("done", _done), ("report", _report), ("more", _more), .. _topics.Select((t, i) => ($"topic_{i}", (Widget)t))];

    private void ShowTopic(int topic)
    {
        _topic = topic;
        Window.Relayout();
        Window.Invalidate();
    }

    /// <summary>A page of its own, with the report to read through before it goes.</summary>
    private void Report() => Window.OpenPage(new ReportPage(Window, _reportText));

    public override void Layout(Rect card, Rect bar)
    {
        _sidebar = new Rect(card.X, card.Y, S(SidebarWidth), card.H);
        for (int i = 0; i < _topics.Count; i++)
            _topics[i].Bounds = new Rect(card.X + S(10), card.Y + S(12) + i * S(38), S(SidebarWidth) - S(20), S(34));
        _view = new Rect(_sidebar.Right, card.Y, card.Right - _sidebar.Right, card.H);
        int width = Math.Min(_view.W - S(48), S(720));
        string text = Help.Topics[_topic].Text;
        int textTop = card.Y + S(14) + S(30) + S(16);
        int height = Window.TextHeight(text, Fonts.Body, width);
        _more.Bounds = new Rect(_view.X + S(24), textTop + height + S(18), _more.PreferredWidth(), S(36));
        PlaceBarButtons(bar, _done, _report);
    }

    public override void Paint(Gfx g, Rect card, Rect bar)
    {
        var p = Theme.P;
        g.FillRound(card, S(10), p.Surface);
        g.Fill(new Rect(_sidebar.Right - 1, card.Y + S(10), 1, card.H - S(20)), p.Border);
        var selected = _topics[_topic].Bounds;
        g.FillRound(selected, S(6), p.Hover);
        g.FillRound(new Rect(selected.X, selected.Y + S(8), S(3), selected.H - S(16)), S(2), p.Accent);

        var topic = Help.Topics[_topic];
        int left = _view.X + S(24);
        int width = Math.Min(_view.W - S(48), S(720));
        g.Text(topic.Title, new Rect(left, card.Y + S(14), width, S(30)), Fonts.Heading, p.Text);
        int textTop = card.Y + S(14) + S(30) + S(16);
        int height = Window.TextHeight(topic.Text, Fonts.Body, width);
        g.Text(topic.Text, new Rect(left, textTop, width, height), Fonts.Body, p.Text, TextFlags.WordBreak | TextFlags.EditControl | TextFlags.NoPrefix);
    }
}

/// <summary>Report a problem: what the person was doing, the report as it'll be sent to read through, Send, and Save instead. It goes straight to
/// the developer, with no keys in it, and when it can't be sent it's saved as a file, as it always was. With nowhere to send it yet, saving it
/// is all there is, so that's the one button. Once it has gone or been saved, what came of it shows, and the button is Done.</summary>
internal sealed class ReportPage : Page
{
    private readonly TextField _said;
    private readonly LogView _text;
    private readonly Button _send;
    private readonly Button _save;
    private readonly string _report;
    private string? _outcome;
    private bool _sending;
    private bool _finished;
    private ReportSender.Outcome? _sent;
    private Rect _intro;
    private Rect _saidLabel;
    private Rect _textLabel;
    private int _barTextRight;

    public ReportPage(MainWindow window, Func<string> reportText) : base(window)
    {
        _report = ReportSender.Prepared(reportText());
        _said = Add(new TextField("当时在做什么，例如哪个游戏、点了什么"));
        _text = Add(new LogView());
        _text.SetLines(_report.Replace("\r", "").Split('\n').Select(line => new LogLine(UiLevel.Info, line)).ToArray());
        _send = Add(new Button(ReportSender.CanSend ? "发送" : "保存", ButtonStyle.Primary, Primary));
        _save = Add(new Button("改为保存", ButtonStyle.Secondary, Save));
        _save.Visible = ReportSender.CanSend;
    }

    public override string Title => "报告问题";

    /// <summary>Not while it's on its way, so what came of it is seen.</summary>
    public override bool CanGoBack => !_sending;

    public override (string name, Widget widget)[] Named => _save.Visible ? [("said", _said), ("send", _send), ("save", _save)] : [("said", _said), ("send", _send)];

    private static string Intro => ReportSender.CanSend
        ? $"将直接发送给开发者：{OptionCatalog.ReportHolds}。写明当时在做什么最有帮助。"
        : $"会保存为文件供你发给开发者：{OptionCatalog.ReportHolds}。写明当时在做什么最有帮助。";

    private void Primary()
    {
        if (_finished)
            Window.ClosePage();
        else if (ReportSender.CanSend)
            Send();
        else
            Save();
    }

    private void Send()
    {
        if (_sending)
            return;
        _sending = true;
        _send.Enabled = _save.Enabled = false;
        _outcome = "正在发送…";
        string said = _said.Value.Trim();
        string title = said.Length > 0 ? said[..Math.Min(said.Length, 80)] : "来自窗口的报告";
        new Thread(() =>
        {
            _sent = ReportSender.Send("manual", "", title, said, _report);
            Window.PostFromWorker();
        }) { IsBackground = true, Name = "Sending a report" }.Start();
        Window.Relayout();
        Window.Invalidate();
    }

    private void Save()
    {
        try
        {
            string said = _said.Value.Trim();
            string path = Help.SaveReport(said.Length > 0 ? $"当时在做什么：{said}{Environment.NewLine}{Environment.NewLine}{_report}" : _report);
            Help.ShowReport(path);
            Finish($"已保存为 {path}，其中不含密钥。");
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
            _outcome = $"报告保存失败：{e.Message}";
            Window.Invalidate();
        }
    }

    private void Finish(string outcome)
    {
        _outcome = outcome;
        _finished = true;
        _send.Text = "已完成";
        _send.Enabled = true;
        _save.Visible = false;
        Window.Relayout();
        Window.Invalidate();
    }

    public override void OnWorker()
    {
        if (_sending && _sent is { } outcome)
        {
            _sending = false;
            if (outcome.SavedTo is not null)
                Help.ShowReport(outcome.SavedTo);
            Finish(outcome.Said);
        }
    }

    public override void Layout(Rect card, Rect bar)
    {
        int pad = S(20);
        int width = card.W - pad * 2;
        int introHeight = Window.TextHeight(Intro, Fonts.Body, width);
        _intro = new Rect(card.X + pad, card.Y + pad, width, introHeight);
        _saidLabel = new Rect(card.X + pad, _intro.Bottom + S(16), width, S(22));
        _said.Bounds = new Rect(card.X + pad, _saidLabel.Bottom + S(4), width, S(34));
        _textLabel = new Rect(card.X + pad, _said.Bounds.Bottom + S(16), width, S(22));
        _text.Bounds = new Rect(card.X + pad, _textLabel.Bottom + S(4), width, Math.Max(card.Bottom - pad - _textLabel.Bottom - S(4), S(60)));
        _barTextRight = PlaceBarButtons(bar, _send, _save);
    }

    public override void Paint(Gfx g, Rect card, Rect bar)
    {
        var p = Theme.P;
        g.FillRound(card, S(10), p.Surface);
        g.Text(Intro, _intro, Fonts.Body, p.SubText, TextFlags.Wrapped);
        g.Text("当时在做什么", _saidLabel, Fonts.Strong, p.Text, TextFlags.Line);
        g.Text("发送的内容", _textLabel, Fonts.Strong, p.Text, TextFlags.Line);
        if (_outcome is not null)
            PaintBarText(g, bar, bar.X + S(20), _barTextRight, _outcome, p.Text);
    }
}

/// <summary>Asked the once, of whoever had the welcome before sending problems to the developer was on it: whether to. Either answer is
/// kept, and it's never asked again. Closing the program without answering brings it back next time, as with the welcome.</summary>
internal sealed class ProblemsQuestionPage : Page
{
    private readonly Session _session;
    private readonly Button _send;
    private readonly Button _no;

    public ProblemsQuestionPage(MainWindow window, Session session) : base(window)
    {
        _session = session;
        _send = Add(new Button("发送报告", ButtonStyle.Primary, () => Answer(true)));
        _no = Add(new Button("暂不发送", ButtonStyle.Secondary, () => Answer(false)));
    }

    public override string Title => OptionCatalog.ProblemsQuestion;
    public override bool CanGoBack => false;
    public override (string name, Widget widget)[] Named => [("send_problems", _send), ("no_thanks", _no)];

    private void Answer(bool send)
    {
        _session.Settings.AnswerAboutProblems(send);
        Window.ClosePage();
    }

    public override void Layout(Rect card, Rect bar) => PlaceBarButtons(bar, _send, _no);

    public override void Paint(Gfx g, Rect card, Rect bar)
    {
        var p = Theme.P;
        g.FillRound(card, S(10), p.Surface);
        int width = Math.Min(card.W - S(64), S(640));
        int left = card.X + (card.W - width) / 2;
        int h = Window.TextHeight(OptionCatalog.ProblemsExplained, Fonts.Body, width);
        // In the middle of the card, as it's all there is to read.
        int y = card.Y + Math.Max(S(28), (card.H - h) / 2);
        g.Text(OptionCatalog.ProblemsExplained, new Rect(left, y, width, h), Fonts.Body, p.Text, TextFlags.WordBreak | TextFlags.EditControl | TextFlags.NoPrefix);
    }
}

/// <summary>v18：一行可点击的文字，用 1.5.0 的 Explorer.OpenWebPage 打开浏览器。</summary>
internal sealed class LinkLabel(string text, Action<string> open) : Widget
{
    public override bool CanFocus => true;

    public override void Paint(Gfx g)
    {
        var p = Theme.P;
        g.Text(text, Bounds, Fonts.Body, IsHovered ? p.AccentHover : p.Accent, TextFlags.Line);
        int width = Math.Min(Window.TextWidth(text, Fonts.Body), Bounds.W);
        g.Fill(new Rect(Bounds.X, Bounds.Bottom - S(3), width, 1), p.Accent);
    }

    public override void MouseUp(int x, int y)
    {
        if (Bounds.Contains(x, y))
            open(text);
    }

    public override bool KeyDown(int key)
    {
        if (key is not (Win32.VK_RETURN or Win32.VK_SPACE))
            return false;
        open(text);
        return true;
    }
}

/// <summary>v18 汉化版关于页：补回汉化作者信息和可点击的 B 站主页链接。</summary>
internal sealed class AboutPage : Page
{
    private const string Description = OptionCatalog.WelcomeIntro;
    private const string License = OptionCatalog.License;
    private const string Author = "汉化作者：illyasever（简体中文版维护）";
    private const string AuthorUrl = "https://space.bilibili.com/36010";

    private readonly Button _done;
    private readonly LinkLabel _link;

    public AboutPage(MainWindow window) : base(window)
    {
        _done = Add(new Button("已完成", ButtonStyle.Primary, window.ClosePage));
        _link = Add(new LinkLabel(AuthorUrl, Explorer.OpenWebPage));
    }

    public override string Title => "关于";
    public override (string name, Widget widget)[] Named => [("done", _done), ("link", _link)];

    public override void Layout(Rect card, Rect bar)
    {
        PlaceBarButtons(bar, _done);
        int width = Math.Min(card.W - S(64), S(760));
        int left = card.X + (card.W - width) / 2;
        int y = card.Y + S(28) + S(34) + S(8) + Window.TextHeight(Description, Fonts.Body, width)
            + S(12) + S(22) + S(14) + S(22) + S(8);
        _link.Bounds = new Rect(left, y, width, S(24));
    }

    public override void Paint(Gfx g, Rect card, Rect bar)
    {
        var p = Theme.P;
        g.FillRound(card, S(10), p.Surface);
        int width = Math.Min(card.W - S(64), S(760));
        int left = card.X + (card.W - width) / 2;
        int y = card.Y + S(28);
        g.Text($"{AppInfo.Name} {AppInfo.Version}", new Rect(left, y, width, S(34)), Fonts.Heading, p.Text);
        y += S(34) + S(8);
        int h = Window.TextHeight(Description, Fonts.Body, width);
        g.Text(Description, new Rect(left, y, width, h), Fonts.Body, p.Text, TextFlags.WordBreak | TextFlags.EditControl | TextFlags.NoPrefix);
        y += h + S(12);
        g.Text(License, new Rect(left, y, width, S(22)), Fonts.Body, p.SubText, TextFlags.Line);
        y += S(22) + S(14);
        g.Text(Author, new Rect(left, y, width, S(22)), Fonts.Body, p.Text, TextFlags.Line);
    }
}
