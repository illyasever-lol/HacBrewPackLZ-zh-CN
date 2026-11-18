using System.Text;

namespace HacBrewPackLz;

/// <summary>
/// The screen people get by double clicking the program or dropping files on it. It takes over the whole window and
/// draws every row to fit the window's current size, so resizing just draws it again. It stays open until told to quit.
/// </summary>
internal sealed partial class Tui
{
    private const int MinimumWidth = 30;
    private const int MinimumHeight = 9;
    private const int Margin = 2;
    private const int PasteLength = 4;

    private enum View
    {
        /// <summary>The list, or a job's own view while there is one.</summary>
        Titles,

        /// <summary>The log: everything that's been said as the last job went.</summary>
        Job,

        /// <summary>All there is to know about the game the pointer's on, and what can be done to it, as on the Switch (TuiInside.cs).</summary>
        Details,

        /// <summary>What a game's made of, a folder at a time (TuiInside.cs).</summary>
        Inside,

        /// <summary>Report a problem: the report to read, what the person was doing, and sending it.</summary>
        Report,
        Options,
        Help,

        /// <summary>The first time it opens: what it's for and the few options most people want a say in, as on the Switch.</summary>
        Welcome,
    }

    private enum Prompt
    {
        None,
        Path,
        /// <summary>A name to find inside a game.</summary>
        Find,
        Menu,
        ConfirmStop,
        ConfirmQuit,
        NoUpdate,
        EditOption,
        ReportSaid,
        AskAboutProblems,
    }

    private readonly Session _session = new();
    private readonly List<OptionSpec> _allOptions;
    private readonly List<OptionSpec> _welcomeOptions;

    /// <summary>The end of the welcome, as the last thing on it.</summary>
    private static readonly OptionSpec StartSpec = new("start", "开始", OptionKind.Action, 0, OptionCatalog.WelcomeLater) { Verb = "开始" };

    /// <summary>What the options screen goes through: all of them, or the welcome's few.</summary>
    private List<OptionSpec> _options => (_view == View.Welcome ? _welcomeOptions : _allOptions).Where(o => OptionCatalog.Shown(o, _session.Settings.Get)).ToList();
    private View _view;
    private Prompt _prompt;
    private string _input = "";
    private int _promptRow;
    private int _menuSelected;
    private int _menuFirst;
    private string? _message;
    private int _selected;
    /// <summary>The game the pointer was on when the view changed, and the line of the list it was on, to find it again in the new view.</summary>
    private string? _anchorKey;
    private int _anchorLine;
    private int _optionSelected;
    private int _titlesFirst;
    /// <summary>The pointer on a job's own view, once it's over, which starts on the first game, as those that couldn't be done come first.</summary>
    private int _jobSelected;
    private int _jobFirst;
    /// <summary>The game About it is about, by its key, or a file that can't be used, by its path.</summary>
    private string? _aboutKey;
    private int _aboutScroll;
    private string? _title;
    /// <summary>The report as it'll be sent, made when Report a problem opens, what the person wrote with it, and whether it's on its way.</summary>
    private string _reportText = "";
    private string _reportSaid = "";
    private int _reportScroll;
    private volatile bool _sending;
    private int _optionsFirst;
    private int _helpScroll;
    private int _width;
    private int _height;
    private string[] _shown = [];
    private volatile bool _dirty = true;
    private const int SpinnerStep = 120;
    private bool _spinning;
    private long _spinnerFrame;
    private bool _quit;
    private bool _testedProblem;

    private Tui()
    {
        _allOptions = OptionCatalog.GeneralGroups.SelectMany(g => g.Options).ToList();
        _welcomeOptions = [.. OptionCatalog.Welcome.Options, StartSpec];
        _session.Changed += () => _dirty = true;
        // It only comes the once, however it's left: everything on it is under Options too.
        // Only leaving it counts as having seen it: quitting from it brings it back next time, as in the GUI.
        if (!_session.Settings.Welcomed)
            _view = View.Welcome;
        // Whoever had the welcome before sending problems was on it is asked the once.
        else if (_session.Settings.AskAboutProblems)
            _prompt = Prompt.AskAboutProblems;
    }

    public static int Run(IReadOnlyList<string> inputs)
    {
        var tui = new Tui();
        Ui.Sink = tui._session;
        ProblemReports.ReportText = tui._session.ReportText;
        ProblemReports.Opened();
        ProblemReports.WatchForFreezes(ProblemReports.AnsweredLately, tui._session.Doing);
        if (inputs.Count > 0)
            tui._session.AddInputs(inputs);
        tui.Loop();
        return 0;
    }

    private void Loop()
    {
        uint? inputMode = Terminal.DisableSelection();
        Console.TreatControlCAsInput = true;
        Console.Title = AppInfo.Name;
        AppDomain.CurrentDomain.ProcessExit += (_, _) => LeaveScreen(inputMode);
        Console.Out.Write("\x1b[?1049h\x1b[?7l\x1b[?25l\x1b[2J\x1b[H");
        try
        {
            while (!_quit)
            {
                ProblemReports.Alive();
                ReadKeys();
                var size = (Terminal.Width, Terminal.Height);
                // The spinner turns on its own, so it can be seen that reading hasn't stuck.
                if (_spinning && Environment.TickCount64 / SpinnerStep != _spinnerFrame)
                    _dirty = true;
                if (size != (_width, _height) || _dirty)
                    Draw(size.Width, size.Height);
                if (!_testedProblem)
                {
                    _testedProblem = true;
                    ProblemReports.MakeTestProblem();
                }
                Thread.Sleep(30);
            }
        }
        finally
        {
            _session.Dispose();
            LeaveScreen(inputMode);
        }
    }

    private static void LeaveScreen(uint? inputMode)
    {
        Console.Out.Write($"\x1b[?25h\x1b[?7h\x1b[?1049l{Sgr.Reset}");
        Terminal.ShowScrollBars(true);
        Terminal.RestoreInputMode(inputMode);
    }

    private void Draw(int width, int height)
    {
        _dirty = false;
        _spinning = false;
        _spinnerFrame = Environment.TickCount64 / SpinnerStep;
        var snapshot = _session.Snapshot();
        UpdateTitle(snapshot);

        // One cell is left free so nothing can ever touch the right edge while the window is being resized.
        int usable = width - 1;
        List<string> rows = width < MinimumWidth || height < MinimumHeight
            ? [new Line(usable).Add(AppInfo.Name + "：请放大此窗口", Sgr.Dim).ToString()]
            : _view switch
            {
                View.Job => DrawJob(snapshot, usable, height),
                View.Details => DrawDetails(snapshot, usable, height),
                View.Inside => DrawInside(usable, height),
                View.Report => DrawReport(usable, height),
                View.Options or View.Welcome => DrawOptions(usable, height),
                View.Help => DrawHelp(usable, height),
                _ => DrawTitles(snapshot, usable, height),
            };

        var output = new StringBuilder();
        if (width != _width || height != _height)
        {
            Terminal.ShowScrollBars(false);
            output.Append("\x1b[2J");
            _shown = new string[height];
            _width = width;
            _height = height;
        }
        for (int y = 0; y < height; y++)
        {
            string row = y < rows.Count ? rows[y] : "";
            if (row != _shown[y])
            {
                // Written over what was there, and only what's left after it cleared, as clearing the row first shows for a moment: the
                // row of the game being compressed changes several times a second, and flickered.
                output.Append($"\x1b[{y + 1};1H").Append(row).Append("\x1b[K");
                _shown[y] = row;
            }
        }

        bool editing = _prompt is Prompt.Path or Prompt.Find or Prompt.EditOption or Prompt.ReportSaid;
        if (editing)
        {
            int column = Math.Min(Margin + Terminal.DisplayWidth(PromptQuestion()) + 2 + Terminal.DisplayWidth(_input), usable - 1);
            output.Append($"\x1b[{_promptRow + 1};{column + 1}H\x1b[?25h");
        }
        else
        {
            output.Append("\x1b[?25l");
        }
        string frame = output.ToString();
        Console.Out.Write(frame);
        LogFrame(width, height, frame);
    }

    /// <summary>For tests: a file each frame is added to as it's written, so what it does to the screen can be gone over afterwards.</summary>
    private static readonly string? ScreenLog = Environment.GetEnvironmentVariable("HACBREWPACKLZ_SCREEN_LOG") is { Length: > 0 } path ? path : null;

    /// <summary>A line each, after the size it was drawn for. Nothing on the screen has a line break in it.</summary>
    private static void LogFrame(int width, int height, string frame)
    {
        if (ScreenLog is null)
            return;
        try
        {
            File.AppendAllText(ScreenLog, $"{width}x{height}\t{frame}\n", new UTF8Encoding(false));
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
        }
    }

    /// <summary>
    /// The first row to show, moved only as far as it takes to keep the selected row on screen. Moving the selection
    /// then changes two rows rather than all of them, which a console window redraws a lot faster.
    /// </summary>
    private static int KeepInView(int first, int selected, int visible, int count) =>
        Math.Clamp(Math.Clamp(first, selected - visible + 1, selected), 0, Math.Max(count - visible, 0));

    private static string ViewName(GamesShown view) => view switch { GamesShown.Done => "已完成", GamesShown.Everything => "全部", _ => "待处理" };

    /// <summary>Left goes to the view before, right to the one after, as on the Switch, keeping the pointer on the same game if it's there,
    /// and otherwise starting at the top. With compression off nothing is done, so there's no Done to go to.</summary>
    private void StepView(SessionSnapshot s, int direction)
    {
        int index = Array.IndexOf(s.ViewsOffered, s.View) + direction;
        if (index < 0 || index >= s.ViewsOffered.Length)
            return;
        // In a view with nothing in it, the game is still the one from before, so going through that view doesn't lose it. On a file that
        // can't be used there's no game to find again: an empty key finds nothing.
        if (_selected < s.Rows.Length)
            (_anchorKey, _anchorLine) = (s.Rows[_selected].Key, _selected - _titlesFirst);
        else
            _anchorKey = ListCount(s) > 0 ? "" : _anchorKey ?? "";
        _session.SetView(s.ViewsOffered[index]);
    }

    /// <summary>The views, each with how many games it has, the one shown filled in, and the arrows that go through them after them. In a
    /// narrow window it says less, rather than wrapping: first the arrows go, then the other views.</summary>
    private static string ViewsLine(SessionSnapshot s, int width)
    {
        var line = new Line(width).To(Margin);
        var views = s.ViewsOffered;
        // Without colours, brackets say which one is shown.
        string Label(GamesShown view) => view == s.View && Sgr.Plain ? $"[{ViewName(view)} {s.CountOf(view)}]" : $" {ViewName(view)} {s.CountOf(view)} ";
        const string Arrows = "  ◄ ►";
        int room = width - Margin * 2;
        int all = views.Sum(v => Label(v).Length) + views.Length - 1;
        if (all > room)
            return line.Add(Label(s.View), Sgr.Chosen).ToString();
        for (int i = 0; i < views.Length; i++)
            line.Add(i > 0 ? " " : "").Add(Label(views[i]), views[i] == s.View ? Sgr.Chosen : Sgr.Dim);
        if (all + Arrows.Length <= room)
            line.Add(Arrows, Sgr.Faint);
        return line.ToString();
    }

    private static bool IsLongJob(SessionActivity activity) =>
        activity is SessionActivity.Compressing or SessionActivity.Extracting or SessionActivity.Verifying;

    private static int ListCount(SessionSnapshot s) => s.Rows.Length + s.Issues.Length;

    private static string Games(int count) => count == 1 ? "1 game" : $"{count} games";

    // ---------------------------------------------------------------------------------------------------------------
    // Shared parts of the screen

    /// <summary>The name and version, with what's going on along the right, and a blank row below.</summary>
    private static void Header(List<string> rows, int width, string right, string rightStyle)
    {
        rows.Add("");
        rows.Add(new Line(width).To(Margin).Add(AppInfo.Name, Sgr.Title).Add("  ").Add(AppInfo.Version, Sgr.Dim).Right(right, rightStyle).ToString());
        rows.Add("");
    }

    private static string RuleRow(int width) =>
        new Line(width).To(Margin).Add(new string(Terminal.Rule, Math.Max(width - Margin * 2, 0)), Sgr.Faint).ToString();

    private static void Pad(List<string> rows, int upTo)
    {
        while (rows.Count < upTo)
            rows.Add("");
    }

    /// <summary>Text that wraps over as many rows as it takes, rather than being cut short.</summary>
    private static List<string> Wrapped(string text, int width, string style) =>
        Terminal.Wrap(text, width - Margin * 2).Select(row => new Line(width).To(Margin).Add(row, style).ToString()).ToList();

    /// <summary>Key hints. In a narrow window the middle ones go first, and the last one, usually how to leave, always stays.</summary>
    private static string Hints(int width, params (string key, string label)[] hints) => HintRows(width, 1, hints)[0];

    /// <summary>
    /// The keys to press, each as a key cap with what it does after it, over as many rows as it takes up to <paramref name="maxRows"/>.
    /// Only when that isn't enough are any left out, from the end but always keeping the last, which is how to leave.
    /// </summary>
    private static List<string> HintRows(int width, int maxRows, params (string key, string label)[] hints)
    {
        const int Gap = 3;
        static int Cells((string key, string label) hint) => Terminal.DisplayWidth(hint.key) + 3 + Terminal.DisplayWidth(hint.label);

        int room = width - Margin * 2;
        List<List<(string key, string label)>> Pack(List<(string key, string label)> shown)
        {
            var rows = new List<List<(string key, string label)>> { new() };
            int used = 0;
            foreach (var hint in shown)
            {
                if (rows[^1].Count > 0 && used + Gap + Cells(hint) > room)
                {
                    rows.Add([]);
                    used = 0;
                }
                used += (rows[^1].Count > 0 ? Gap : 0) + Cells(hint);
                rows[^1].Add(hint);
            }
            return rows;
        }

        var shown = hints.ToList();
        var packed = Pack(shown);
        while (packed.Count > maxRows && shown.Count > 1)
        {
            shown.RemoveAt(shown.Count - 2);
            packed = Pack(shown);
        }

        // The keys are coloured letters, as on the menu: in boxes, one row above another, the boxes would run together.
        return packed.Select(row =>
        {
            var line = new Line(width).To(Margin);
            for (int i = 0; i < row.Count; i++)
            {
                if (i > 0)
                    line.Add(new string(' ', Gap));
                line.Add($" {row[i].key} ", Sgr.AccentStrong).Add(" " + row[i].label, Sgr.Dim);
            }
            return line.ToString();
        }).ToList();
    }

    /// <summary>A key and what it does, for the line above the keys: the one that comes next in a filled box, the only one, so it stands out.
    /// It's kept together.</summary>
    private static (string text, string style)[] Cap(string key, string label, bool next) =>
        [($" {key} ", next ? Sgr.Chosen : Sgr.AccentStrong), (" " + label, next ? Sgr.Strong : "")];

    private static (string text, string style)[] Words(string text, string style = "") => text.Length == 0 ? [] : [(text, style)];

    /// <summary>
    /// Pieces of a line three spaces apart, carried onto the next row where the window's too narrow for them rather than cut short, and words
    /// too long for a row wrapped. <paramref name="reserve"/> keeps room at the end of the first row for a number and a bar.
    /// </summary>
    private static List<Line> Flow(int width, int reserve, params (string text, string style)[][] pieces)
    {
        const int Gap = 3;
        var lines = new List<Line>();
        Line? line = null;
        int Limit() => width - Margin - (lines.Count <= 1 ? reserve : 0);
        foreach (var piece in pieces)
        {
            int cells = piece.Sum(p => Terminal.DisplayWidth(p.text));
            if (cells == 0)
                continue;
            if (line is not null && line.Used + Gap + cells > Limit())
                line = null;
            if (line is null)
            {
                line = new Line(width).To(Margin);
                lines.Add(line);
            }
            else
            {
                line.Add(new string(' ', Gap));
            }
            if (piece.Length > 1 || line.Used + cells <= Limit())
            {
                foreach (var (text, style) in piece)
                    line.Add(text, style);
                continue;
            }
            var rows = Terminal.Wrap(piece[0].text, Limit() - Margin);
            line.Add(rows[0], piece[0].style);
            foreach (string rest in rows.Skip(1))
            {
                line = new Line(width).To(Margin).Add(rest, piece[0].style);
                lines.Add(line);
            }
        }
        if (lines.Count == 0)
            lines.Add(new Line(width));
        return lines;
    }

    /// <summary>The room a number and a bar take at the end of a row, in the columns the games' savings are in.</summary>
    private static int MeasureRoom(Columns c, int width) => c.ShowSaved ? width - Margin - (c.SavedEnd - 6) : 0;

    /// <summary>
    /// Where each column goes for the window's width: the mark, the tick, the name with what there is to say about the game after it, then
    /// its size now, after, and how much smaller, as a number and a thin bar. As the window narrows the bar goes first, then the size now,
    /// then how much smaller.
    /// </summary>
    private readonly record struct Columns(int NowEnd, int AfterEnd, int SavedEnd, int Bar, int BarCells, bool ShowNow, bool ShowSaved)
    {
        public const int Mark = 1;
        public const int Tick = 3;
        public const int Name = 7;

        /// <summary>Where the name and what's said about it stop, a gap short of the sizes.</summary>
        public int NameEnd => (ShowNow ? NowEnd : AfterEnd) - 12;

        public static Columns For(int width)
        {
            int right = width - Margin;
            int barCells = width >= 100 ? 13 : width >= 80 ? 8 : 0;
            bool showSaved = width >= 45;
            int savedEnd = barCells > 0 ? right - barCells - 2 : right;
            // Room for "Nothing more" under "At least", two spaces clear of what's after.
            int afterEnd = showSaved ? savedEnd - 14 : right;
            return new Columns(afterEnd - 13, afterEnd, savedEnd, right - barCells, barCells, width >= 60, showSaved);
        }
    }

    /// <summary>Where what's saved is an estimate, it's what each is sure to save, and the column says so.</summary>
    private static string HeadingRow(Columns c, int width, bool atLeast = false, bool sizeOnly = false, bool done = false)
    {
        var line = new Line(width);
        // On Done: what each was, what it takes now and what was freed, as on the Switch and in the Windows app.
        if (c.ShowNow)
            line.To(c.NowEnd - 3).Add(done ? "Was" : "Now", Sgr.Faint);
        // Extracting and checking change no game's size, so there's nothing to go under After and Saved.
        if (sizeOnly)
            return line.ToString();
        line.To(c.AfterEnd - (done ? 3 : 5)).Add(done ? "Now" : "After", Sgr.Faint);
        if (c.ShowSaved)
            line.To(c.SavedEnd - (done ? 5 : atLeast ? 8 : 5)).Add(done ? "Freed" : atLeast ? "At least" : "Saved", Sgr.Faint);
        return line.ToString();
    }

    /// <summary>What a game's row says, column by column: the mark, the name, what's said about it and then, fainter, what it is, its size now,
    /// after, and how much smaller, with how much of the bar that fills.</summary>
    private sealed record Cells(string Mark, string MarkStyle, string Name, string NameStyle, string Said, string SaidStyle, string Faint,
        string Now, string After, string AfterStyle, string Share, string ShareStyle, double? Fraction, string BarStyle);

    /// <summary>One game's row. What's said about it is cut short only where there's no room, and the line below the list or I says all of it.</summary>
    private static string GameRow(Columns c, int width, Cells cells, string tick, bool band)
    {
        var line = new Line(width, band ? Sgr.Band : "");
        line.To(Columns.Mark).Add(cells.Mark, cells.MarkStyle).To(Columns.Tick);
        if (tick.Length > 0)
            line.Add(tick, Sgr.Accent);
        line.To(Columns.Name);
        int end = c.NameEnd;
        line.Add(Terminal.Fit(cells.Name, end - line.Used), cells.NameStyle);
        foreach (var (text, style) in new[] { (cells.Said, cells.SaidStyle), (cells.Faint, Sgr.Faint) })
        {
            if (text.Length > 0 && line.Used + 5 < end)
                line.Add("  ").Add(Terminal.Fit(text, end - line.Used), style);
        }
        if (c.ShowNow && cells.Now.Length > 0)
            line.To(c.NowEnd - Terminal.DisplayWidth(cells.Now)).Add(cells.Now, Sgr.Dim);
        if (cells.After.Length > 0)
            line.To(c.AfterEnd - Terminal.DisplayWidth(cells.After)).Add(cells.After, cells.AfterStyle);
        if (c.ShowSaved && cells.Share.Length > 0)
            line.To(c.SavedEnd - Terminal.DisplayWidth(cells.Share)).Add(cells.Share, cells.ShareStyle);
        // What it saves, coloured from the left on a quiet track, as on the Switch, or how far a game being worked on has got.
        if (c.BarCells > 0 && cells.Fraction is { } fraction)
            line.To(c.Bar).AddBar(c.BarCells, fraction, cells.BarStyle);
        if (band)
            line.To(width);
        return line.ToString();
    }

    private static double Saving(long before, long after) => before > 0 ? Math.Max((before - after) / (double)before, 0) : 0;

    // ---------------------------------------------------------------------------------------------------------------
    // The list of games

    private List<string> DrawTitles(SessionSnapshot s, int width, int height)
    {
        if (s.ShowingJob)
            return DrawJobView(s, width, height);
        var rows = new List<string>();
        string right = s.KeyProblem is not null ? "keys not found" : s.EverythingCount == 0 ? "" : Games(s.EverythingCount);
        Header(rows, width, right, s.KeyProblem is not null ? Sgr.Warn : Sgr.Dim);

        // Along the bottom: the next thing to do and what it comes to, then the keys. The rest are in the menu, which opens upwards over the
        // end of the list.
        var hints = HintRows(width, 2, TitleHints(s));
        var foot = FooterLines(s, width);
        int footer = height - 3 - foot.Count - hints.Count;
        if (s.Rows.Length == 0 && s.Issues.Length == 0)
            DrawEmpty(rows, s, width, footer);
        else
            DrawTable(rows, s, width, footer);

        Pad(rows, footer);
        if (_prompt == Prompt.Menu)
        {
            var menu = MenuRows(MenuGroups(s), width, footer - 3);
            rows.RemoveRange(rows.Count - menu.Count, menu.Count);
            rows.AddRange(menu);
        }
        rows.Add(RuleRow(width));
        _promptRow = rows.Count;
        rows.AddRange(foot);
        rows.Add("");
        rows.AddRange(hints);
        rows.Add("");
        return rows;
    }

    private (string key, string label)[] TitleHints(SessionSnapshot s)
    {
        if (Blocked(s))
            return [("Esc", "停止"), ("D", "Log")];
        switch (_prompt)
        {
            case Prompt.Path:
                return [("Enter", "添加"), ("Esc", "取消")];
            case Prompt.ConfirmQuit:
                return [("Y", "退出"), ("N", "Stay")];
            case Prompt.AskAboutProblems:
                return [("Y", "Send them"), ("N", "No thanks")];
            case Prompt.NoUpdate:
                return [("U", "取消勾选它们"), ("R", "仍一并重打包"), ("Esc", "取消")];
            case Prompt.Menu:
                return [("Enter", "选择"), ("Esc", "关闭")];
        }

        if (ListCount(s) == 0)
            return [("P", "添加路径"), ("O", "选项"), ("M", "更多"), ("Q", "退出")];
        var hints = new List<(string key, string label)>();
        if (s.Rows.Length > 0)
            hints.Add(("Space", "Tick"));
        // Only while a ticked game can use it.
        if (s.Rows.Any(r => r.Selected && r.NeedsEstimate && !r.IsCompressed))
            hints.Add(("A", "分析"));
        // Measuring goes on behind the list, which Esc stops.
        if (s.Activity == SessionActivity.Measuring)
            hints.Add(("Esc", "Stop measuring"));
        // When it's the next thing to do, it's on the line above.
        if (s.Rows.Length > 0 && !SendNext(s))
            hints.Add(("S", "发送到 Switch"));
        hints.AddRange([("Enter", "详细信息"), ("O", "选项"), ("M", "更多"), ("Q", "退出")]);
        return hints.ToArray();
    }

    /// <summary>The games a first look can't vouch for, which compressing reads in full first.</summary>
    private static int NotSureCount(SessionSnapshot s) => s.Rows.Count(r => r.NotSure && !r.IsDone(BatchMode.Convert) && !r.IsFolder);

    /// <summary>Something's going on that the list waits for. Measuring goes on behind it instead, and gives way to anything asked for.</summary>
    private static bool Blocked(SessionSnapshot s) => s.IsBusy && s.Activity != SessionActivity.Measuring;

    /// <summary>One thing the menu does, with the key that does it straight away from the list, if it has one.</summary>
    private sealed record MenuChoice(string Key, string Label, Action Act);

    /// <summary>The Switch's + menu pages, side by side: the game the pointer's on, the ticked games, the list, and help.</summary>
    private sealed record MenuGroup(string Title, List<MenuChoice> Choices);

    /// <summary>Everything else that can be done right now, under the page it's on, as on the Switch.</summary>
    private List<MenuGroup> MenuGroups(SessionSnapshot s)
    {
        var groups = new List<MenuGroup>();
        int count = ListCount(s);
        if (_selected < count)
        {
            bool onGame = _selected < s.Rows.Length;
            string key = onGame ? s.Rows[_selected].Key : "issue|" + s.Issues[_selected - s.Rows.Length].Path;
            var game = new List<MenuChoice> { new("Enter", "详细信息", () => OpenDetails(key)) };
            if (onGame && !s.Rows[_selected].IsFolder)
            {
                game.Add(new("L", "查看内部", () => LookInside(key)));
                game.Add(new("E", "Extract it", () => DoToOne(ConsoleKey.E, key)));
                game.Add(new("V", "Check it for damage", () => DoToOne(ConsoleKey.V, key)));
            }
            if (onGame && s.Rows[_selected].OutputPath is { } made)
                game.Add(new("F", "在文件夹中显示", () => Explorer.Show(made)));
            game.Add(new("Del", "Remove it from the list", () => HandleTitlesKey(new ConsoleKeyInfo('\0', ConsoleKey.Delete, false, false, false))));
            groups.Add(new MenuGroup("此游戏", game));
        }
        int ticked = s.Rows.Count(r => r.Selected);
        if (s.Rows.Length > 0)
        {
            var some = new List<MenuChoice>();
            if (ticked > 0)
            {
                some.Add(new("", "Extract them", () => StartJob(_session.Extract(), "Those are all extracted already, or are games' folders, which are as extracted as they get.")));
                some.Add(new("", "Check them for damage", () => StartJob(_session.Check(), "Games' folders have nothing packed in them to check.")));
                some.Add(new("", "Analyze them again", () => _session.AnalyzeAgain()));
            }
            some.Add(new("T", ticked == s.Rows.Length ? "Untick all" : "Tick all", () => _session.SelectAll(ticked != s.Rows.Length)));
            if (ticked > 0)
                some.Add(new("", "Remove them from the list", _session.RemoveTicked));
            groups.Add(new MenuGroup(ticked == 1 ? "The ticked game" : ticked > 1 ? $"The {ticked} ticked games" : "Ticked games", some));
        }
        var list = new List<MenuChoice> { new("P", "添加路径", () => { _prompt = Prompt.Path; _input = ""; }) };
        if (s.Rows.Length > 0)
            list.Add(new("S", "发送到 Switch", () => _session.SendToSwitch()));
        if (count > 0)
            list.Add(new("X", "清空列表", _session.RemoveAll));
        list.Add(new("D", "Log", () => _view = View.Job));
        groups.Add(new MenuGroup("列表", list));
        groups.Add(new MenuGroup("帮助", [
            new("H", "帮助", () => OpenHelp(null)),
            new("R", "Report a problem", OpenReport),
            new("", "Check for a new version", () => _session.CheckForUpdates()),
            new("", "关于", () => OpenHelp("关于")),
        ]));
        return groups;
    }

    /// <summary>Starts a job on the ticked games, which shows on the list, or says why it didn't start.</summary>
    private void StartJob(bool started, string why)
    {
        if (started)
            _jobSelected = _jobFirst = 0;
        else
            _message = why;
    }

    private void LookInside(string key)
    {
        OpenDetails(key);
        OpenInside(fromDetails: false);
    }

    /// <summary>Help, from the top or at a heading in it.</summary>
    private void OpenHelp(string? heading)
    {
        _helpScroll = 0;
        if (heading is not null)
        {
            var lines = HomeHelp(Math.Max(_width - 1 - Margin * 2, 20));
            lines.AddRange(HelpText.Lines(Math.Max(_width - 1 - Margin * 2, 20)));
            _helpScroll = Math.Max(lines.FindIndex(l => l.text == heading && l.level == UiLevel.Heading), 0);
        }
        _view = View.Help;
    }

    /// <summary>
    /// The pages side by side, each under its name, as many to a row as fit across the window, the rest below them. In a window too short for
    /// all of it, it scrolls to keep the pointer in view.
    /// </summary>
    private List<string> MenuRows(List<MenuGroup> groups, int width, int room)
    {
        const int Gap = 4;
        const int KeyWidth = 8;
        int total = groups.Sum(g => g.Choices.Count);
        _menuSelected = Math.Clamp(_menuSelected, 0, total - 1);
        int[] widths = groups.Select(g => Math.Max(Terminal.DisplayWidth(g.Title), g.Choices.Max(c => KeyWidth + Terminal.DisplayWidth(c.Label))) + Margin).ToArray();
        int[] starts = new int[groups.Count];
        for (int g = 1; g < groups.Count; g++)
            starts[g] = starts[g - 1] + groups[g - 1].Choices.Count;
        // Pages in rows across the window.
        var bands = new List<List<int>> { new() };
        int used = 0;
        for (int g = 0; g < groups.Count; g++)
        {
            if (bands[^1].Count > 0 && used + Gap + widths[g] > width - Margin)
            {
                bands.Add([]);
                used = 0;
            }
            used += (bands[^1].Count > 0 ? Gap : 0) + widths[g];
            bands[^1].Add(g);
        }
        var lines = new List<string>();
        int pointerLine = 0;
        foreach (var band in bands)
        {
            if (lines.Count > 0)
                lines.Add("");
            int height = band.Max(g => groups[g].Choices.Count) + 1;
            for (int y = 0; y < height; y++)
            {
                var line = new Line(width);
                int x = Margin;
                foreach (int g in band)
                {
                    if (y == 0)
                    {
                        line.To(x).Add(groups[g].Title, Sgr.Faint);
                    }
                    else if (y - 1 < groups[g].Choices.Count)
                    {
                        var choice = groups[g].Choices[y - 1];
                        bool chosen = starts[g] + y - 1 == _menuSelected;
                        if (chosen)
                            pointerLine = lines.Count;
                        line.To(x - Margin).Add(chosen ? Terminal.Pointer.ToString() : " ", Sgr.Accent).To(x);
                        // In a column of their own, which says they're keys: boxed, one under another, they'd run together into one grey bar.
                        if (choice.Key.Length > 0)
                            line.Add($" {choice.Key}", Sgr.AccentStrong);
                        line.To(x + KeyWidth).Add(choice.Label, chosen ? Sgr.Strong : "");
                    }
                    x += widths[g] + Gap;
                }
                lines.Add(line.ToString());
            }
        }
        int visible = Math.Max(room - 1, 1);
        _menuFirst = KeepInView(_menuFirst, pointerLine, visible, lines.Count);
        var rows = new List<string> { RuleRow(width) };
        rows.AddRange(lines.Skip(_menuFirst).Take(visible));
        return rows;
    }

    /// <summary>Everything ticked is compressed already, so what's left is putting it on the Switch.</summary>
    private bool SendNext(SessionSnapshot s) =>
        !s.Rows.Any(r => r.LeftToCompress(Compressing)) && s.Rows.Any(r => r.Selected && (r.IsDone(BatchMode.Convert) || r.IsCompressed));

    private bool Compressing => _session.Settings.Get("--nocompress") != "1";

    private string CompressLabel() => _session.Settings.Get("--nocompress") == "1" ? "Repack" : "Compress";

    /// <summary>What's being asked for, ahead of what's typed.</summary>
    private string PromptQuestion() => _prompt == Prompt.Find ? "Find " : "";

    /// <summary>The job gets a view of its own, with only its games, which the session shows from now on.</summary>
    private void StartCompressing()
    {
        if (_session.Compress())
            _jobSelected = _jobFirst = 0;
        else
            _message = _session.Snapshot().Rows.Any(r => r.Selected) ? "这些都已经压缩过了。" : "尚未勾选任何游戏，请按空格键勾选。";
    }

    private void DrawEmpty(List<string> rows, SessionSnapshot s, int width, int bottom)
    {
        var message = new List<(string text, string style)>();
        if (s.EverythingCount > 0)
        {
            rows.Add(ViewsLine(s, width));
            if (s.View == GamesShown.Done)
            {
                message.Add(("Nothing here is done yet", Sgr.Strong));
                message.Add(("", ""));
                message.Add((Compressing ? "Games show up here once they're compressed, and so do games that already were when they're added. Left goes back to what's to do."
                    : "Games show up here once they're repacked, and so do games with no update to make part of them. Left goes back to what's to do.", Sgr.Dim));
            }
            else
            {
                message.Add(("此处内容已全部处理", Sgr.Strong));
                message.Add(("", ""));
                message.Add((Compressing ? $"{Games(s.DoneCount)} {(s.DoneCount == 1 ? "is" : "are")} compressed. Right shows {(s.DoneCount == 1 ? "it" : "them")} under Done."
                    : $"{Games(s.DoneCount)} {(s.DoneCount == 1 ? "has" : "have")} nothing for a repack to gain. Right shows {(s.DoneCount == 1 ? "it" : "them")} under Done.", Sgr.Dim));
            }
        }
        else if (s.KeyProblem is not null)
        {
            message.Add(("需要先提供密钥", Sgr.Warn));
            message.Add(("", ""));
            message.Add(("请把 prod.keys 放在本程序旁边，或直接拖到本窗口。", ""));
            message.Add(("", ""));
            message.Add(("", ""));
        }
        if (s.EverythingCount == 0)
        {
            message.Add(("将游戏文件或文件夹拖到此窗口", Sgr.Strong));
            message.Add(("", ""));
            message.Add(("To compress, extract or check them: NSP, NSZ, XCI and XCZ. Your originals are never changed.", Sgr.Dim));
        }

        // Each line wraps, centred, rather than being cut short in a narrow window.
        var lines = message.SelectMany(m => m.text.Length == 0 ? [("", "")] : Terminal.Wrap(m.text, width - Margin * 2).Select(row => (row, m.style))).ToList();
        int top = rows.Count + Math.Max((bottom - rows.Count - lines.Count) / 2, 0);
        Pad(rows, top);
        foreach (var (text, style) in lines)
        {
            if (rows.Count >= bottom)
                break;
            rows.Add(new Line(width).To((width - Terminal.DisplayWidth(text)) / 2).Add(text, style).ToString());
        }
    }

    private void DrawTable(List<string> rows, SessionSnapshot s, int width, int bottom)
    {
        var c = Columns.For(width);
        int count = ListCount(s);
        // After the view changes, the pointer goes back to the game it was on if this view has it, on the same line, and otherwise to the
        // top, as on the Switch: where a game this view doesn't have was says nothing about where anything in it is.
        if (_anchorKey is not null)
        {
            int found = Array.FindIndex(s.Rows, r => r.Key == _anchorKey);
            _selected = Math.Max(found, 0);
            _titlesFirst = found < 0 ? 0 : found - _anchorLine;
            _anchorKey = null;
        }
        _selected = Math.Clamp(_selected, 0, Math.Max(count - 1, 0));
        rows.Add(ViewsLine(s, width));
        rows.Add("");
        rows.Add(HeadingRow(c, width, s.Rows.Any(r => r.State != RowState.Done && r.Estimate is { AtLeast: true }), done: s.View == GamesShown.Done && s.Job is null));

        // Below the list, what the row of the game the pointer's on has no room for: why it couldn't be done, or its notes, such as other
        // copies of it left aside. Every game gets the same two lines for it, so the list doesn't jump about while moving through it, and one
        // with a lot to say can't take the window from the rest: what doesn't fit is for I, which says it all. With nothing to say about any
        // game here, the list has all of it. Anything about the list as a whole goes under that.
        var general = s.Notes.SelectMany(n => Terminal.Wrap(n, width - Margin * 2)).ToList();
        bool anySaid = !Blocked(s) && Enumerable.Range(0, count).Any(i => HasMoreToSay(s, i));
        int reserve = (anySaid ? NotesRoom + 1 : 0) + (general.Count > 0 ? general.Count + 1 : 0);
        int visible = Math.Max(bottom - rows.Count - reserve, Math.Min(count, 3));
        int first = _titlesFirst = KeepInView(_titlesFirst, _selected, visible, count);
        for (int i = first; i < Math.Min(first + visible, count) && rows.Count < bottom; i++)
        {
            bool chosen = i == _selected && !Blocked(s);
            if (i < s.Rows.Length)
                rows.Add(GameRow(c, width, ListCells(s.Rows[i], chosen), s.Rows[i].Selected ? "[x]" : "[ ]", chosen));
            else
                rows.Add(GameRow(c, width, IssueCells(s.Issues[i - s.Rows.Length], chosen), "", chosen));
        }

        if (anySaid && rows.Count < bottom - 1)
        {
            rows.Add("");
            rows.AddRange(UpTo(BelowRow(s, _selected, width), Math.Min(NotesRoom, bottom - rows.Count), width, "I"));
        }
        if (general.Count > 0 && rows.Count < bottom - 1)
        {
            rows.Add("");
            foreach (string text in general.TakeWhile(_ => rows.Count < bottom))
                rows.Add(new Line(width).To(Margin).Add(text, Sgr.Dim).ToString());
        }
    }

    /// <summary>As many of the lines as there's room for. When some don't fit, the last one there is the start of it and the key that shows
    /// all of it, so nothing is cut off without a word.</summary>
    private static List<string> UpTo(List<(string text, string style)> lines, int room, int width, string key)
    {
        const string Rest = " shows all of it";
        var rows = new List<string>();
        for (int i = 0; i < Math.Min(lines.Count, room); i++)
        {
            var (text, style) = lines[i];
            var line = new Line(width).To(Margin);
            if (i == room - 1 && lines.Count > room)
            {
                string start = Terminal.Fit(text, Math.Max(width - Margin * 2 - Terminal.DisplayWidth(Rest) - 4, 1));
                if (!start.EndsWith(Terminal.Ellipsis))
                    start += Terminal.Ellipsis;
                line.Add(start, style).Add("  ").Add(key, Sgr.Accent).Add(Rest, Sgr.Dim);
            }
            else
            {
                line.Add(text, style);
            }
            rows.Add(line.ToString());
        }
        return rows;
    }

    /// <summary>How many lines below the list are for the game the pointer's on.</summary>
    private const int NotesRoom = 2;

    /// <summary>Whether a game has anything to say below the list, which <see cref="BelowRow"/> says, without wrapping it to find out.</summary>
    private static bool HasMoreToSay(SessionSnapshot s, int index) =>
        index < s.Rows.Length
            ? (s.Rows[index].State == RowState.Failed && s.Rows[index].Failure is not null) || s.Rows[index].Notes.Length > 0
            : index - s.Rows.Length < s.Issues.Length;

    /// <summary>What the row of the game the pointer's on has no room for: why it couldn't be done, or the file that can't be used, in full,
    /// and its notes, such as other copies of it that were left aside. Below the list, as much as <see cref="NotesRoom"/> has room for.</summary>
    private static List<(string text, string style)> BelowRow(SessionSnapshot s, int index, int width)
    {
        var lines = new List<(string text, string style)>();
        void Say(string text, string style)
        {
            foreach (string row in Terminal.Wrap(text, width - Margin * 2))
                lines.Add((row, style));
        }
        if (index < s.Rows.Length)
        {
            var row = s.Rows[index];
            // In a few plain words first, then the failure as it was said.
            if (row.State == RowState.Failed && row.Failure is { } failure)
            {
                Say(Help.PlainReason(failure, null), Sgr.Bad);
                Say(failure, Sgr.Dim);
            }
            foreach (string note in row.Notes)
                Say(note, Sgr.Dim);
        }
        else if (index - s.Rows.Length < s.Issues.Length)
        {
            Say(s.Issues[index - s.Rows.Length].Message, Sgr.Warn);
        }
        return lines;
    }

    /// <summary>A game on the list: ticked or not, what's become of it or what it would come to, and the pointer if it's on it.</summary>
    private Cells ListCells(TitleRow row, bool chosen)
    {
        string mark = chosen ? Terminal.Pointer.ToString() : "";
        bool on = row.Selected;
        string nameStyle = chosen ? Sgr.Strong : on ? "" : Sgr.Dim;
        string now = ProgressReporter.Format(row.InputBytes);
        switch (row.State)
        {
            case RowState.Failed:
                return new Cells(mark, Sgr.Accent, row.Name, nameStyle, "未完成", Sgr.Bad, row.Detail, now, "", "", "", "", null, "");
            case RowState.Done when row.DoneAs == BatchMode.Extract:
                return new Cells(mark, Sgr.Accent, row.Name, nameStyle, "extracted", Sgr.Good, row.Detail, now, "", "", "", "", null, "");
            case RowState.Done:
            {
                long saved = row.InputBytes - row.OutputBytes;
                string said = !Compressing ? "repacked" : saved > 0 ? $"compressed, {ProgressReporter.Format(saved)} saved" : "compressed";
                double done = Saving(row.InputBytes, row.OutputBytes);
                return new Cells(mark, Sgr.Accent, row.Name, nameStyle, said, Sgr.Good, row.Detail, now, ProgressReporter.Format(row.OutputBytes), Sgr.Good,
                    Percent(row.InputBytes, row.OutputBytes), Sgr.GoodStrong, done, Sgr.Saving(done));
            }
        }
        // On Done, each game says what's become of it: that it saves nothing worth it goes without saying there, as that's why it's on Done,
        // and what compressing it freed is where its saving would be, as on the Switch.
        if (_session.Snapshot().View == GamesShown.Done && (row.IsCompressed || row.BelowMinimum is not null))
        {
            long freed = row.Freed;
            string by = row.Estimate?.ByMakers == true ? "compressed by its developer" : "";
            // What it was, in words where a copy doesn't know; what one settled here without being compressed freed is nothing.
            string was = freed > 0 ? ProgressReporter.Format(row.InputBytes + freed) : row.IsCompressed ? "Not known" : now;
            string freedSaid = freed > 0 ? ProgressReporter.Format(freed) : row.IsCompressed ? "Not known" : "Nothing";
            return new Cells(mark, Sgr.Accent, row.Name, nameStyle, by, Sgr.Good, row.Detail, was, now, on ? "" : Sgr.Dim,
                freedSaid, freed > 0 ? Sgr.GoodStrong : Sgr.Dim, null, "");
        }
        // Why it's unticked, when the minimum saving left it out, comes before what it is.
        string reason = row.BelowMinimum ?? "";
        // Sure to save nothing: the size it stays at, and that in words, rather than 0% or a hair over its size. Beside what compressing it
        // freed, "Nothing" alone would read as if it had saved nothing. The reason says why.
        Cells Nothing() => new(mark, Sgr.Accent, row.Name, nameStyle, reason, Sgr.Warn, row.Detail, now, now, on ? "" : Sgr.Dim,
            row.IsCompressed ? "Nothing more" : "Nothing", Sgr.Dim, null, "");
        // Not analyzed yet: said in words, as a figure is never left blank.
        if (row.Estimate is not { } estimate)
            return row.IsCompressed ? Nothing() : new Cells(mark, Sgr.Accent, row.Name, nameStyle, reason, Sgr.Warn, row.Detail, now, "", "", "Not yet", Sgr.Dim, null, "");
        // A first look can't vouch for it, so there's no figure to give until compressing has measured it.
        if (row.NotSure)
            return new Cells(mark, Sgr.Accent, row.Name, nameStyle, TitleRow.NotSureShort, Sgr.Dim, row.Detail, now, "", "", "", "", null, "");
        if (!estimate.HasFigure(Compressing))
            return Nothing();
        string after = (estimate.MarginBytes > 0 ? "~" : "") + ProgressReporter.Format(estimate.MostBytes);
        bool grows = estimate.SavedBytes < 0;
        string shareStyle = !on ? Sgr.Faint : grows ? Sgr.WarnStrong : Sgr.GoodStrong;
        double saving = Saving(estimate.InputBytes, estimate.MostBytes);
        return new Cells(mark, Sgr.Accent, row.Name, nameStyle, reason, Sgr.Warn, row.Detail, now, after, on ? "" : Sgr.Dim,
            Percent(estimate.InputBytes, estimate.MostBytes), shareStyle, saving, on ? Sgr.Saving(saving) : Sgr.Faint);
    }

    /// <summary>A file that can't be used, which can be read below the list and in full with I, and taken off the list.</summary>
    private static Cells IssueCells(InputIssue issue, bool chosen) =>
        new(chosen ? Terminal.Pointer.ToString() : "!", chosen ? Sgr.Accent : Sgr.Warn, issue.Message, chosen ? Sgr.Strong : Sgr.Warn, "", "", "", "", "", "", "", "", null, "");

    private static string Percent(long before, long after)
    {
        if (before <= 0)
            return "";
        double saved = (before - after) * 100.0 / before;
        return saved >= 0 ? $"{saved:0}%" : $"+{-saved:0}%";
    }

    /// <summary>
    /// The line above the keys: the next key to press with what it comes to, what's going on, or a question. What has more to it, such as
    /// how to get the Switch to see the games, wraps onto the rows below rather than being cut short.
    /// </summary>
    private List<string> FooterLines(SessionSnapshot s, int width)
    {
        var c = Columns.For(width);
        switch (_prompt)
        {
            case Prompt.Path:
                return [new Line(width).To(Margin).Add(PromptQuestion(), Sgr.Strong).Add("> ", Sgr.Accent).Add(_input).ToString()];
            case Prompt.ConfirmQuit:
                return [new Line(width).To(Margin).Add("退出？", Sgr.Strong).ToString()];
            case Prompt.AskAboutProblems:
                return [.. Wrapped(OptionCatalog.ProblemsQuestion, width, Sgr.Strong), .. Wrapped(OptionCatalog.ProblemsExplained, width, Sgr.Dim)];
            case Prompt.Menu:
                return [""];
            case Prompt.NoUpdate:
            {
                int count = _session.TickedWithoutUpdate().Length;
                return Wrapped(count == 1 ? "This one has no update, so repacking it gains nothing." : $"{count} of these have no update, so repacking them gains nothing.", width, Sgr.Strong);
            }
        }
        if (_message is not null)
        {
            // A second line, when there is one, goes below the first.
            var lines = new List<string>();
            string[] parts = _message.Split('\n');
            for (int i = 0; i < parts.Length; i++)
                lines.AddRange(Wrapped(parts[i], width, i == 0 ? Sgr.Warn : Sgr.Dim));
            return lines;
        }

        var line = new Line(width).To(Margin);
        switch (s.Activity)
        {
            case SessionActivity.Reading:
                _spinning = true;
                return [line.Add("|/-\\"[(int)(_spinnerFrame % 4)] + " " + s.ReadingText + Terminal.Ellipsis, Sgr.Dim).ToString()];
            case SessionActivity.Analyzing or SessionActivity.Measuring or SessionActivity.Verifying:
            {
                string what = ActivityName(s.Activity);
                line.Add(s.CurrentCount > 0 ? $"{what} {s.CurrentIndex} of {s.CurrentCount}: {s.CurrentTitle}" : what + Terminal.Ellipsis, Sgr.Accent);
                if (s.Progress is { Total: > 0 } p)
                    Measure(line, c, (double)p.Done / p.Total, Sgr.AccentStrong, Sgr.Accent);
                return [line.ToString()];
            }
            case SessionActivity.Sending:
            {
                var (first, style, second) = Sending(s);
                var lines = new List<string> { line.Add(first, style).ToString() };
                if (second.Length > 0)
                    lines.AddRange(Wrapped(second, width, Sgr.Dim));
                return lines;
            }
        }

        var foot = NextLines(s, width, c);
        if (s.UpdateNote is { } note)
            foot.AddRange(Wrapped(note, width, Sgr.Accent));
        return foot;
    }

    /// <summary>How much of something as a number and a bar, in the columns the games' savings are in.</summary>
    private static void Measure(Line line, Columns c, double fraction, string numberStyle, string barStyle)
    {
        if (c.ShowSaved)
        {
            string percent = $"{fraction * 100:0}%";
            if (line.Used + 2 <= c.SavedEnd - percent.Length)
                line.To(c.SavedEnd - percent.Length).Add(percent, numberStyle);
        }
        if (c.BarCells > 0 && line.Used + 1 <= c.Bar)
            line.To(c.Bar).AddBar(c.BarCells, fraction, barStyle);
    }

    /// <summary>The next thing to do, with what it comes to: compressing what's ticked, then sending it once it's all compressed.</summary>
    private List<string> NextLines(SessionSnapshot s, int width, Columns c)
    {
        var toDo = s.Rows.Where(r => r.LeftToCompress(Compressing)).ToList();
        List<string> Done(List<Line> lines) => lines.Select(l => l.ToString()).ToList();
        if (toDo.Count > 0)
        {
            var compress = Cap("C", $"{CompressLabel()} {Games(toDo.Count)}", next: true);
            long before = toDo.Sum(r => r.InputBytes);
            if (!toDo.All(r => r.Estimate is not null))
                return Done(Flow(width, 0, compress, Words(ProgressReporter.Format(before)), Words(toDo.Count == 1 ? "A shows what it saves" : "A shows what they save", Sgr.Dim)));
            // Ticked by hand: nothing to add up until they're measured, and they're compressed whatever they save.
            if (toDo.All(r => r.NotSure))
                return Done(Flow(width, 0, compress, Words(ProgressReporter.Format(before)), Words("not sure yet: ticked, they're compressed whatever they save", Sgr.Dim)));
            // What they're sure to save goes under the column that says so.
            long after = toDo.Sum(r => r.SureAfterBytes!.Value);
            var lines = Flow(width, MeasureRoom(c, width), compress, Words($"{ProgressReporter.Format(before)} to {ProgressReporter.Format(after)}"));
            Measure(lines[0], c, Saving(before, after), after <= before ? Sgr.GoodStrong : Sgr.WarnStrong, Sgr.Good);
            return Done(lines);
        }
        if (SendNext(s))
            return Done(Flow(width, 0, Cap("S", "发送到 Switch", next: true), Words("the Switch installs the ticked games from this PC", Sgr.Dim)));
        // Games a first look isn't sure of aren't ticked, and may save more than it seems: a thorough analysis reads them in full, and ticks the
        // ones worth it.
        // On Done with nothing ticked, what compressing them came to, as on the Switch and in the Windows app.
        int compressed = s.View == GamesShown.Done && s.Job is null ? s.Rows.Count(r => r.IsCompressed || r.IsDone(BatchMode.Convert)) : 0;
        if (compressed > 0 && !s.Rows.Any(r => r.Selected))
        {
            long freedAll = s.Rows.Sum(r => r.State == RowState.Done && r.OutputBytes < r.InputBytes ? r.InputBytes - r.OutputBytes : r.Freed);
            return Done(Flow(width, 0, Words($"{Games(compressed)} compressed{(freedAll > 0 ? $", {ProgressReporter.Format(freedAll)} freed in all" : "")}", Sgr.Strong),
                Words("Space ticks a game, to extract or check it", Sgr.Dim)));
        }
        if (s.Rows.Length > 0 && !s.Rows.Any(r => r.Selected))
            return Done(Flow(width, 0, Words("未勾选任何游戏", Sgr.Strong),
                Words(NotSureCount(s) is var notSure && notSure > 0 ? $"{Games(notSure)} not sure yet may save more: tick one, or let Thorough analysis read them" : "Space ticks the game the pointer's on", Sgr.Dim)));
        if (s.Summary is not null)
            return Done(Flow(width, 0, Words(s.Summary, Sgr.For(s.SummaryLevel))));
        if (s.Rows.Length > 0)
            return Done(Flow(width, 0, Words($"{Games(s.Rows.Length)}, {ProgressReporter.Format(s.Rows.Sum(r => r.InputBytes))}", Sgr.Strong)));
        return [""];
    }

    /// <summary>Whether the Switch is there yet, how to get it there, and what it's reading, as the GUI says it.</summary>
    private static (string, string, string) Sending(SessionSnapshot s)
    {
        if (s.Send is not { } send)
            return ("Getting ready to send to the Switch" + Terminal.Ellipsis, Sgr.Accent, "");
        string sharing = $"Sharing {Games(s.Rows.Length)}";
        if (send.Connected.Length == 0)
        {
            string cable = SwitchUsb.Possible ? " or a USB cable" : "";
            return ($"{sharing}. Waiting for the Switch", Sgr.Accent,
                $"On the Switch, open hacBrewPackLZ: these games come up on Game files, as being on {send.Machine}. Same network{cable}.");
        }
        string connected = $"connected {string.Join(" and ", send.Connected)}";
        if (send.Reading is not { } file || !send.Files.TryGetValue(file, out var sent))
            return ($"{sharing}. The Switch is {connected}", Sgr.Accent, "在 Switch 上选择要安装的游戏，请保持本窗口打开直到完成。");
        string speed = send.BytesPerSecond > 0 ? $"  {send.BytesPerSecond / 1048576.0:0.0} MB/s" : "";
        return ($"Sending {Path.GetFileName(file)}", Sgr.Accent, $"{ProgressReporter.Format(sent.read)} of {ProgressReporter.Format(sent.length)}{speed}  {connected}");
    }

    // ---------------------------------------------------------------------------------------------------------------
    // A job's own view: only its games and how each is going, then what came of each, until Enter goes back to the list

    private List<string> DrawJobView(SessionSnapshot s, int width, int height)
    {
        var job = s.Job!;
        var c = Columns.For(width);
        var rows = new List<string>();
        var (state, stateStyle) = JobState(s);
        Header(rows, width, state, stateStyle);
        rows.AddRange(JobHeadline(s, width));
        rows.Add("");
        rows.Add(HeadingRow(c, width, sizeOnly: job.Kind is SessionActivity.Extracting or SessionActivity.Verifying));

        var hints = HintRows(width, 2, JobHints(s));
        var foot = JobFooter(s, width, c);
        int footer = height - 3 - foot.Count - hints.Count;

        // Once it's over, below the games and wrapped rather than cut short: why the one the pointer's on couldn't be done or was left out,
        // then where the new files are and what to know about them.
        _jobSelected = Math.Clamp(_jobSelected, 0, Math.Max(s.Rows.Length - 1, 0));
        var below = job.Running ? [] : JobNotes(s, width);
        int visible = Math.Max(footer - rows.Count - (below.Count > 0 ? below.Count + 1 : 0), Math.Min(s.Rows.Length, 3));
        // While it runs, the game being done stays in view.
        int working = Array.FindIndex(s.Rows, r => r.State == RowState.Working);
        int focus = job.Running ? Math.Max(working, 0) : _jobSelected;
        int first = _jobFirst = KeepInView(_jobFirst, focus, visible, s.Rows.Length);
        // Back on the list once it's over, the pointer goes to the game it's on here, if the list has it, and otherwise to the top.
        if (!job.Running)
            (_anchorKey, _anchorLine) = s.Rows.Length > 0 ? (s.Rows[_jobSelected].Key, _jobSelected - first) : ("", 0);
        for (int i = first; i < Math.Min(first + visible, s.Rows.Length) && rows.Count < footer; i++)
        {
            bool band = job.Running ? i == working : i == _jobSelected;
            rows.Add(GameRow(c, width, JobCells(s, s.Rows[i], band), "", band));
        }
        if (below.Count > 0 && rows.Count < footer - 1)
        {
            rows.Add("");
            rows.AddRange(UpTo(below, footer - rows.Count, width, "D"));
        }

        Pad(rows, footer);
        rows.Add(RuleRow(width));
        _promptRow = rows.Count;
        rows.AddRange(foot);
        rows.Add("");
        rows.AddRange(hints);
        rows.Add("");
        return rows;
    }

    private static int DoneCount(SessionSnapshot s) => s.Rows.Count(r => r.State == RowState.Done);

    private static (string text, string style) JobState(SessionSnapshot s)
    {
        var job = s.Job!;
        if (job.Running)
            return (s.CurrentCount > 1 ? $"{job.Doing} {s.CurrentIndex} of {s.CurrentCount}" : job.Doing, Sgr.Accent);
        return s.SummaryLevel switch
        {
            UiLevel.Success => ("已完成", Sgr.Good),
            UiLevel.Warning => ("已停止", Sgr.Warn),
            _ => DoneCount(s) > 0 ? ("已完成，但有问题", Sgr.Bad) : ("无法完成", Sgr.Bad),
        };
    }

    /// <summary>What it's doing to how many games and what they come to, or once it's over, what it did and how long it took.</summary>
    private static List<string> JobHeadline(SessionSnapshot s, int width)
    {
        var job = s.Job!;
        var (what, more) = job.Headline("to");
        string style = job.Running ? Sgr.Strong : job.Outcome switch { UiLevel.Success => Sgr.GoodStrong, UiLevel.Error => Sgr.BadStrong, _ => Sgr.WarnStrong };
        return Flow(width, 0, Words(what, style), Words(more, Sgr.Dim)).Select(l => l.ToString()).ToList();
    }

    /// <summary>A game in a job: done, being done with the step it's on, waiting its turn, or once it's over, why it wasn't done.</summary>
    private Cells JobCells(SessionSnapshot s, TitleRow row, bool band)
    {
        var job = s.Job!;
        string now = ProgressReporter.Format(row.InputBytes);
        string nameStyle = band ? Sgr.Strong : "";
        switch (row.State)
        {
            case RowState.Working:
            {
                string step = "starting";
                double? fraction = null;
                if (s.Progress is { } p)
                {
                    string plain = ProgressReporter.Plain(p.Label);
                    step = char.ToLowerInvariant(plain[0]) + plain[1..];
                    if (p.ShowSizes && p.BytesPerSecond > 0)
                        step += $", {p.BytesPerSecond / 1048576:0} MB/s";
                    if (p.Total > 0)
                        fraction = (double)p.Done / p.Total;
                }
                return new Cells(Terminal.Pointer.ToString(), Sgr.Accent, row.Name, Sgr.Strong, step, "", "", now, "", "",
                    fraction is { } f ? $"{f * 100:0}%" : "", Sgr.AccentStrong, fraction, Sgr.Accent);
            }
            case RowState.Done when job.Kind == SessionActivity.Extracting:
                return new Cells("■", Sgr.Good, row.Name, nameStyle, job.Running ? "extracted" : "", Sgr.Good, row.Detail, now, "", "", "", "", null, "");
            case RowState.Done when job.Kind == SessionActivity.Verifying:
                return new Cells("■", Sgr.Good, row.Name, nameStyle, "whole", Sgr.Good, row.Detail, now, "", "", "", "", null, "");
            case RowState.Failed when job.Kind == SessionActivity.Verifying:
                return new Cells("×", Sgr.Bad, row.Name, nameStyle, "damaged", Sgr.Bad, "", now, "", "", "", "", null, "");
            case RowState.Done:
                return new Cells("■", Sgr.Good, row.Name, nameStyle, job.Running ? "done" : "", Sgr.Good, row.Detail, now, ProgressReporter.Format(row.OutputBytes), "",
                    Percent(row.InputBytes, row.OutputBytes), job.Repacking ? Sgr.Dim : Sgr.GoodStrong, Saving(row.InputBytes, row.OutputBytes), Sgr.Good);
            case RowState.Failed:
                return new Cells("×", Sgr.Bad, row.Name, nameStyle, "未完成", Sgr.Bad, "", now, "", "", "", "", null, "");
        }
        if (job.Running)
            return new Cells("·", Sgr.Faint, row.Name, Sgr.Dim, "waiting", Sgr.Faint, "", now, "", "", "", "", null, "");
        string? reason = row.BelowMinimum ?? (row.IsCompressed ? "已压缩" : null);
        return new Cells("·", Sgr.Faint, row.Name, nameStyle, reason is null ? "not done" : "left out", reason is null ? Sgr.Dim : Sgr.Warn, "", now, "", "", "", "", null, "");
    }

    /// <summary>Below a job's games once it's over: why the one the pointer's on wasn't done, then what went wrong with the job as a whole,
    /// if anything did, and then where the new files are and what to know about them.</summary>
    private List<(string text, string style)> JobNotes(SessionSnapshot s, int width)
    {
        var job = s.Job!;
        var lines = new List<(string text, string style)>();
        void Say(string text, string style)
        {
            foreach (string row in Terminal.Wrap(text, width - Margin * 2))
                lines.Add((row, style));
        }
        if (_jobSelected < s.Rows.Length)
        {
            var row = s.Rows[_jobSelected];
            if (row.State == RowState.Failed && row.Failure is { } failure)
            {
                Say($"{row.Name}: {Help.PlainReason(failure, null)}", Sgr.Bad);
                Say(failure, Sgr.Dim);
            }
            else if (row.State == RowState.Ready && (row.BelowMinimum ?? (row.IsCompressed ? "it's already compressed" : null)) is { } reason)
                Say($"{row.Name} was left out: {reason}", Sgr.Warn);
        }
        // What stopped the job as a whole, rather than one of its games.
        if (s.SummaryLevel != UiLevel.Success && !s.Rows.Any(r => r.State == RowState.Failed) && s.Summary is { } summary)
            Say(summary, Sgr.For(s.SummaryLevel));
        if (job.Said.Length > 0)
            Say(string.Join(" ", job.Said), Sgr.Dim);
        return lines;
    }

    /// <summary>All of it together while it runs. Once it's over, what was saved, and the next things to do with what it made.</summary>
    private List<string> JobFooter(SessionSnapshot s, int width, Columns c)
    {
        var job = s.Job!;
        if (_prompt == Prompt.ConfirmStop)
            return Wrapped("Stop this job? The games already done are kept, and nothing is left half written.", width, Sgr.Strong);
        List<string> Done(List<Line> lines) => lines.Select(l => l.ToString()).ToList();
        if (job.Running)
        {
            var lines = Flow(width, MeasureRoom(c, width), Words("合计", Sgr.Strong),
                Words(job.SecondsLeft is { } seconds ? ProgressReporter.TimeLeft(seconds) : "working out how long it takes"),
                Words(job.SavedBytes > 0 && !job.Repacking ? $"{ProgressReporter.Format(job.SavedBytes)} saved so far" : "", Sgr.GoodStrong));
            Measure(lines[0], c, job.Overall, Sgr.AccentStrong, Sgr.Accent);
            return Done(lines);
        }
        // A check says what it found, whatever it found, as nothing about the games changes.
        if (job.Kind == SessionActivity.Verifying)
            return s.Summary is { } found ? Wrapped(found, width, Sgr.For(s.SummaryLevel)) : [""];
        if (DoneCount(s) == 0)
            return Wrapped("Enter goes back to the list, where what wasn't done is still to do." + (job.Failed > 0 ? " D shows the log of what happened." : ""), width, Sgr.Dim);
        var pieces = new List<(string text, string style)[]>();
        if (job.Kind == SessionActivity.Compressing)
        {
            var made = s.Rows.Where(r => r.IsDone(BatchMode.Convert)).ToList();
            if (job.SavedBytes > 0 && !job.Repacking)
                pieces.Add(Words($"{ProgressReporter.Format(job.SavedBytes)} saved", Sgr.GoodStrong));
            pieces.Add(Words($"{ProgressReporter.Format(made.Sum(r => r.InputBytes))} to {ProgressReporter.Format(made.Sum(r => r.OutputBytes))}", Sgr.Dim));
            pieces.Add(Cap("S", "发送到 Switch", next: true));
        }
        if (JobOutput(s) is not null)
            pieces.Add(Cap("F", "在文件夹中显示", next: false));
        return Done(Flow(width, 0, [.. pieces]));
    }

    private (string key, string label)[] JobHints(SessionSnapshot s)
    {
        if (_prompt == Prompt.ConfirmStop)
            return [("Y", "停止"), ("N", "继续")];
        if (s.Job!.Running)
            return [("Esc", "停止"), ("D", "Log")];
        return [("Enter", "Back to the list"), ("I", "详细信息"), ("D", "Log"), ("Q", "退出")];
    }

    /// <summary>What a job made for the game the pointer's on, or else the last thing it made.</summary>
    private string? JobOutput(SessionSnapshot s) => (_jobSelected < s.Rows.Length ? s.Rows[_jobSelected].OutputPath : null) ?? s.LastOutput;

    /// <summary>How far a job has got and how it ended, in the window's title, which the taskbar shows while another window is in use.</summary>
    private void UpdateTitle(SessionSnapshot s)
    {
        string title = AppInfo.Name;
        if (s.Job is { } job)
        {
            string count = job.Running && s.CurrentCount > 1 ? $" {s.CurrentIndex} of {s.CurrentCount}" : "";
            title = job.Running ? $"{job.Overall * 100:0}% · {job.Doing}{count} · {AppInfo.Name}"
                : s.SummaryLevel == UiLevel.Success && job.SavedBytes > 0 && !job.Repacking
                    ? $"Finished, {ProgressReporter.Format(job.SavedBytes)} saved · {AppInfo.Name}"
                    : $"{JobState(s).text} · {AppInfo.Name}";
        }
        else if (s.Activity is SessionActivity.Analyzing or SessionActivity.Measuring or SessionActivity.Sending or SessionActivity.Verifying)
        {
            title = $"{ActivityName(s.Activity)} · {AppInfo.Name}";
        }
        if (title == _title)
            return;
        _title = title;
        try
        {
            Console.Title = title;
        }
        catch (Exception e) when (e is IOException or PlatformNotSupportedException)
        {
        }
    }

    // ---------------------------------------------------------------------------------------------------------------
    // About the game the pointer's on: all there is to say about it, so nothing cut short on the list is out of reach

    private List<string> DrawAbout(SessionSnapshot s, int width, int height)
    {
        var rows = new List<string>();
        Header(rows, width, "About it", Sgr.Dim);
        int footer = height - 4;
        var lines = AboutLines(s, width - Margin * 2);
        int visible = Math.Max(footer - rows.Count, 1);
        _aboutScroll = Math.Clamp(_aboutScroll, 0, Math.Max(lines.Count - visible, 0));
        foreach (var (text, style) in lines.Skip(_aboutScroll).Take(visible))
            rows.Add(new Line(width).To(Margin).Add(text, style).ToString());
        Pad(rows, footer);
        rows.Add(RuleRow(width));
        rows.Add("");
        rows.Add(lines.Count > visible ? Hints(width, ("Up/Down", "滚动"), ("Esc", "返回")) : Hints(width, ("Esc", "返回")));
        rows.Add("");
        return rows;
    }

    private List<(string text, string style)> AboutLines(SessionSnapshot s, int width)
    {
        var lines = new List<(string text, string style)>();
        void Say(string text, string style = "")
        {
            foreach (string row in Terminal.Wrap(text, width))
                lines.Add((row, style));
        }
        void Gap() => lines.Add(("", ""));

        var row = s.Rows.FirstOrDefault(r => r.Key == _aboutKey);
        if (row is null)
        {
            var issue = s.Issues.FirstOrDefault(i => "issue|" + i.Path == _aboutKey);
            if (issue is null)
            {
                Say("It isn't on the list any more.", Sgr.Dim);
                return lines;
            }
            Say("Can't be used", Sgr.Warn);
            Gap();
            Say(issue.Message);
            Gap();
            Say(issue.Paths.Length == 1 ? "The file" : "The files", Sgr.Dim);
            foreach (string path in issue.Paths)
                Say(path);
            Gap();
            Say("Del, on the list, takes it off.", Sgr.Dim);
            return lines;
        }

        Say(row.Name, Sgr.Strong);
        Say(row.Detail, Sgr.Dim);
        // Its sizes are what it takes installed, which for an NSZ isn't what the file is.
        if (row.PackedBytes > 0)
            Say($"As files it's {ProgressReporter.Format(row.PackedBytes)}, packed tighter than compressing packs, but installing unpacks it to " +
                $"{ProgressReporter.Format(row.InputBytes)}, which is what's counted here. What's made of it installs as it is.", Sgr.Dim);
        Gap();
        string compress = Compressing ? "compressed" : "repacked";
        switch (row.State)
        {
            case RowState.Working:
                Say("It's being worked on now.", Sgr.Accent);
                break;
            case RowState.Failed:
                Say("无法完成", Sgr.Bad);
                Say(row.Failure ?? "");
                Gap();
                Say("It's still to do, so C tries it again. D has the details of what happened.", Sgr.Dim);
                break;
            case RowState.Done when row.DoneAs == BatchMode.Extract:
                Say("Extracted", Sgr.Good);
                if (row.OutputPath is { } extracted)
                    Say(extracted);
                break;
            case RowState.Done:
                Say($"{char.ToUpperInvariant(compress[0])}{compress[1..]}: {ProgressReporter.Format(row.InputBytes)} to {ProgressReporter.Format(row.OutputBytes)}" +
                    (row.OutputBytes < row.InputBytes ? $", {Percent(row.InputBytes, row.OutputBytes)} smaller" : ""), Sgr.Good);
                if (row.OutputPath is { } output)
                {
                    Gap();
                    Say("Saved as", Sgr.Dim);
                    Say(output);
                }
                break;
            default:
                if (row.IsCompressed)
                {
                    Say("Already compressed, so there's nothing more to gain.", Sgr.Good);
                }
                else if (row.NotSure)
                {
                    Say($"{ProgressReporter.Format(row.InputBytes)} now. Not sure yet what it saves.");
                    Say(TitleRow.NotSureLong, Sgr.Dim);
                }
                else if (row.Estimate is { } nothing && !nothing.HasFigure(Compressing))
                {
                    // Sure to save nothing, or almost nothing, so there's no figure to give: its reason, below, says why.
                    Say(nothing.SureSavedBytes > 0 ? $"{ProgressReporter.Format(row.InputBytes)} now. Once {compress}, it saves almost nothing."
                        : $"{ProgressReporter.Format(row.InputBytes)} now. Once {compress}, it isn't sure to save anything.");
                    Say(nothing.Measured ? "Measured from all of its game data. Nothing has been written yet." : "Nothing has been written yet.", Sgr.Dim);
                }
                else if (row.Estimate is { } estimate)
                {
                    string after = $"{(estimate.MarginBytes > 0 ? "About " : "")}{ProgressReporter.Format(estimate.MostBytes)} once {compress}";
                    string change = estimate.SavedBytes >= 0 ? $"{(estimate.AtLeast ? "at least " : "")}{Percent(estimate.InputBytes, estimate.MostBytes)} smaller"
                        : $"{Percent(estimate.InputBytes, estimate.OutputBytes).TrimStart('+')} bigger";
                    Say($"{ProgressReporter.Format(estimate.InputBytes)} now. {after}, {change}.");
                    Say(estimate.MarginBytes > 0 ? "An estimate from a sample of its game data, and it can save more than this. Nothing has been written yet."
                        : "Worked out from its headers. Nothing has been written yet.", Sgr.Dim);
                }
                else
                {
                    Say($"{ProgressReporter.Format(row.InputBytes)} now. A shows what it comes to once {compress}.");
                }
                if (row.BelowMinimum is { } reason)
                {
                    Gap();
                    Say($"{char.ToUpperInvariant(reason[0])}{reason[1..]}.", Sgr.Warn);
                }
                Gap();
                Say(row.Selected ? "Ticked, so C does it along with the rest." : "Not ticked: Space, on the list, ticks it.", Sgr.Dim);
                break;
        }
        Gap();
        Say(row.Files.Length == 1 ? "Its file" : "Its files", Sgr.Dim);
        foreach (string file in row.Files)
            Say(file);
        if (row.Notes.Length > 0)
        {
            Gap();
            foreach (string note in row.Notes)
                Say(note, Sgr.Dim);
        }
        return lines;
    }

    // ---------------------------------------------------------------------------------------------------------------
    // Details: everything that's been said as the last job went, and checking an NSP, which is a job without games

    private List<string> DrawJob(SessionSnapshot s, int width, int height)
    {
        var rows = new List<string>();
        string state = s.IsBusy
            ? s.CurrentCount > 1 ? $"{ActivityName(s.Activity)} {s.CurrentIndex} of {s.CurrentCount}" : ActivityName(s.Activity)
            : s.Summary is null ? "日志" : s.SummaryLevel switch { UiLevel.Success => "已完成", UiLevel.Warning => "已停止", _ => "问题" };
        Header(rows, width, state, s.IsBusy ? Sgr.Accent : s.Summary is null ? Sgr.Dim : Sgr.For(s.SummaryLevel));
        if (s.IsBusy && s.CurrentTitle is not null)
            rows.Add(new Line(width).To(Margin).Add(s.CurrentTitle, Sgr.Strong).ToString());
        else if (s.Summary is not null)
            AddWrapped(rows, s.Summary, width, Sgr.For(s.SummaryLevel));

        if (s.Progress is { } p)
        {
            double fraction = p.Total > 0 ? (double)p.Done / p.Total : 1;
            rows.Add("");
            rows.Add(new Line(width).To(Margin).Add(ProgressReporter.Plain(p.Label)).Right($"{fraction * 100:0}%", Sgr.AccentStrong).ToString());
            rows.Add(new Line(width).To(Margin).AddBar(Math.Max(width - Margin * 2, 1), fraction, Sgr.Accent).ToString());

            var details = new List<string>();
            if (p.ShowSizes)
                details.Add($"{ProgressReporter.Format(p.Done)} of {ProgressReporter.Format(p.Total)}");
            if (p.ShowSizes && p.BytesPerSecond > 0)
                details.Add($"{p.BytesPerSecond / 1048576:0} MB/s");
            if (Terminal.TimeLeft(p) is { Length: > 0 } left)
                details.Add(left);
            if (p.ShowOutput && p.Done > 0)
                details.Add($"{p.Output * 100.0 / p.Done:0}% of its size so far");
            rows.Add(new Line(width).To(Margin).Add(string.Join("   ", details), Sgr.Dim).ToString());
        }

        rows.Add("");
        var hints = HintRows(width, 1, DetailHints(s));
        int footer = height - 4 - hints.Count;
        var wrapped = new List<string>();
        foreach (var entry in s.Log)
            AddWrapped(wrapped, entry.Text, width, Sgr.For(entry.Level));
        if (wrapped.Count == 0 && !s.IsBusy)
            wrapped.Add(new Line(width).To(Margin).Add("Nothing's been done yet. What's said as games are worked on comes up here.", Sgr.Dim).ToString());
        int room = Math.Max(footer - rows.Count, 0);
        rows.AddRange(wrapped.Skip(Math.Max(wrapped.Count - room, 0)));

        Pad(rows, footer);
        rows.Add(RuleRow(width));
        _promptRow = rows.Count;
        rows.Add(_prompt == Prompt.ConfirmStop ? new Line(width).To(Margin).Add("Stop this job? The games already done are kept.", Sgr.Strong).ToString() : "");
        rows.Add("");
        rows.AddRange(hints);
        rows.Add("");
        return rows;
    }

    private (string key, string label)[] DetailHints(SessionSnapshot s) =>
        _prompt == Prompt.ConfirmStop ? [("Y", "Stop"), ("N", "Keep going")]
        : s.IsBusy ? [("Esc", "停止"), ("D", "返回")] : [("Esc", "返回"), ("Q", "退出")];

    private static void AddWrapped(List<string> rows, string text, int width, string style)
    {
        foreach (string row in Terminal.Wrap(text, width - Margin * 2, 4))
            rows.Add(new Line(width).To(Margin).Add(row, style).ToString());
    }

    private string ActivityName(SessionActivity activity) => activity switch
    {
        SessionActivity.Compressing => _session.Settings.Get("--nocompress") == "1" ? "Repacking" : "Compressing",
        SessionActivity.Extracting => "正在解包",
        SessionActivity.Verifying => "正在校验",
        SessionActivity.Sending => "正在发送到 Switch",
        SessionActivity.Analyzing => "正在分析",
        SessionActivity.Measuring => "Measuring",
        SessionActivity.Reading => "正在读取",
        _ => "",
    };

    // ---------------------------------------------------------------------------------------------------------------
    // Options

    private List<string> DrawOptions(int width, int height)
    {
        var rows = new List<string>();
        bool welcome = _view == View.Welcome;
        Header(rows, width, welcome ? "欢迎" : "选项", Sgr.Dim);
        // What it's for comes first, as long as that leaves room for the options.
        if (welcome && height >= 24)
        {
            void Paragraph(string text, string style)
            {
                foreach (string row in Terminal.Wrap(text, width - Margin * 2))
                    rows.Add(new Line(width).To(Margin).Add(row, style).ToString());
            }

            rows.Add("");
            rows.Add(new Line(width).To(Margin).Add(OptionCatalog.Welcome.Title, Sgr.Title).ToString());
            rows.Add("");
            Paragraph(OptionCatalog.WelcomeIntro, Sgr.Dim);
            rows.Add("");
            Paragraph(OptionCatalog.WelcomeTip, Sgr.Strong);
            rows.Add("");
            Paragraph(OptionCatalog.WelcomeWarning, Sgr.Warn);
            rows.Add("");
        }

        // Turning compression off takes away the options that only go with it.
        _optionSelected = Math.Clamp(_optionSelected, 0, _options.Count - 1);
        var body = new List<(string row, int option)>();
        const int ValueColumn = 30;
        int valueStart = Math.Min(Margin + ValueColumn, width / 2);
        OptionGroup[] groups = welcome ? [OptionCatalog.Welcome with { Title = "To begin with", Options = [.. _welcomeOptions] }] : OptionCatalog.GeneralGroups;
        foreach (var group in groups)
        {
            if (body.Count > 0)
                body.Add(("", -1));
            body.Add((new Line(width).To(Margin).Add(group.Title, Sgr.Dim).ToString(), -1));
            foreach (var option in group.Options.Where(o => OptionCatalog.Shown(o, _session.Settings.Get)))
            {
                int index = _options.IndexOf(option);
                bool chosen = index == _optionSelected;
                var line = new Line(width).Add(chosen ? Terminal.Pointer.ToString() : " ", Sgr.Accent).To(Margin);
                line.Add(Terminal.Fit(option.Label, valueStart - Margin - 1), chosen ? Sgr.Strong : "").To(valueStart);
                string value = OptionValue(option);
                line.Add(value, IsDefault(option) ? Sgr.Dim : Sgr.Accent);
                body.Add((line.ToString(), index));
            }
        }

        // The description of the chosen option wraps, with room for the longest one so the list doesn't jump about
        // while moving through it, as long as at least a few options still show. A blank row keeps it apart from them.
        int textWidth = width - Margin * 2;
        int describeRows = _options.Max(o => Terminal.Wrap(o.Help, textWidth).Count);
        describeRows = Math.Clamp(describeRows, 1, Math.Max(1, height - rows.Count - 8));
        int footer = height - 4 - describeRows;
        int visible = Math.Max(footer - 1 - rows.Count, 1);
        int selectedRow = body.FindIndex(b => b.option == _optionSelected);
        // The group heading above the first option stays in view when going back up to it.
        int first = _optionsFirst = KeepInView(_optionsFirst, _optionSelected == 0 ? 0 : selectedRow, visible, body.Count);
        rows.AddRange(body.Skip(first).Take(visible).Select(b => b.row));
        Pad(rows, footer);

        var description = Terminal.Wrap(_options[_optionSelected].Help, textWidth);
        for (int i = 0; i < describeRows; i++)
        {
            string text = i < describeRows - 1 || description.Count <= describeRows
                ? (i < description.Count ? description[i] : "")
                : Terminal.Fit(string.Join(" ", description.Skip(i)), textWidth);
            rows.Add(new Line(width).To(Margin).Add(text, Sgr.Dim).ToString());
        }
        _promptRow = rows.Count;
        rows.Add(_prompt == Prompt.EditOption
            ? new Line(width).To(Margin).Add("> ", Sgr.Accent).Add(_input).ToString()
            : _message is not null ? new Line(width).To(Margin).Add(_message, Sgr.Warn).ToString() : "");
        rows.Add(RuleRow(width));
        rows.Add(_prompt == Prompt.EditOption
            ? Hints(width, ("Enter", "保存"), ("Esc", "取消"))
            : Hints(width, OptionHints(_options[_optionSelected])));
        rows.Add("");
        return rows;
    }

    /// <summary>Only the keys that do something to the option the pointer's on.</summary>
    private (string key, string label)[] OptionHints(OptionSpec option)
    {
        var hints = new List<(string key, string label)>
        {
            ("Up/Down", "选择"),
            option.Kind switch
            {
                OptionKind.Toggle => ("Enter", "更改"),
                OptionKind.Slider or OptionKind.Choice => ("Left/Right", "更改"),
                OptionKind.Action => ("Enter", option.Verb),
                _ => ("Enter", "编辑"),
            },
        };
        if (!IsDefault(option) && option.Kind != OptionKind.Action)
            hints.Add(("Del", "恢复默认"));
        if (_view == View.Welcome)
        {
            if (option != StartSpec)
                hints.Add(("S", "开始"));
            hints.Add(("Q", "退出"));
        }
        else
        {
            hints.Add(("Esc", "返回"));
        }
        return hints.ToArray();
    }

    private string OptionValue(OptionSpec option)
    {
        string value = _session.Settings.Get(option.Flag);
        return option.Kind switch
        {
            OptionKind.Toggle => option.ToggleOn(value) != option.Inverted ? "On" : "Off",
            OptionKind.Slider or OptionKind.Choice => value.Length > 0 ? value : option.Default,
            // How checking went. Starting has nothing to say.
            OptionKind.Action => option == OptionCatalog.CheckForUpdates ? _session.Snapshot().UpdateNote ?? "" : "",
            _ => value.Length > 0 ? value : option.Placeholder,
        };
    }

    private bool IsDefault(OptionSpec option) => _session.Settings.Get(option.Flag).Length == 0;

    private void ChangeOption(OptionSpec option, string value)
    {
        string old = _session.Settings.Get(option.Flag);
        _session.Settings.Set(option.Flag, value);
        try
        {
            _session.Settings.ToOptions(option.Use);
            _message = null;
        }
        catch (ToolException e)
        {
            _session.Settings.Set(option.Flag, old);
            _message = e.Message;
            return;
        }
        _session.Settings.Save();
        _session.OptionChanged(option.Flag);
        if (option.Flag == "--keyset")
            _session.ReloadKeys();
    }

    private void AdjustOption(int direction)
    {
        var option = _options[_optionSelected];
        switch (option.Kind)
        {
            case OptionKind.Toggle:
                ToggleOption(option);
                break;
            case OptionKind.Slider:
                int level = int.TryParse(OptionValue(option), out int parsed) ? parsed : option.Min;
                level = Math.Clamp(level + direction, option.Min, option.Max);
                ChangeOption(option, level.ToString() == option.Default ? "" : level.ToString());
                break;
            case OptionKind.Choice:
                int index = Math.Max(Array.IndexOf(option.Choices, OptionValue(option)), 0);
                string next = option.Choices[Math.Clamp(index + direction, 0, option.Choices.Length - 1)];
                ChangeOption(option, next == option.Default ? "" : next);
                break;
        }
    }

    private void ToggleOption(OptionSpec option) =>
        ChangeOption(option, option.ToggleValue(!option.ToggleOn(_session.Settings.Get(option.Flag))));

    // ---------------------------------------------------------------------------------------------------------------
    // Help

    /// <summary>How to use this screen comes first. What the command line takes follows, for anyone who wants it.</summary>
    private static List<(string text, UiLevel level)> HomeHelp(int width)
    {
        var lines = new List<(string text, UiLevel level)> { ("使用主界面", UiLevel.Heading) };
        void Paragraph(string text, UiLevel level = UiLevel.Info)
        {
            // A numbered step's later rows line up after its number.
            foreach (string row in Terminal.Wrap(text, width, char.IsDigit(text[0]) ? 3 : 0))
                lines.Add((row, level));
        }

        Paragraph("1. 把游戏文件或文件夹拖到本窗口，或按 P 键输入路径；文件夹会被搜索游戏。");
        Paragraph("2. Press A to see how much each game would save. Only the ones worth it stay ticked, and Space ticks or unticks the one the pointer is on.");
        Paragraph("3. Press C to compress the ticked games. While it works, only those games show, and then what came of each. Enter goes back to the list, where the ones compressed are under Done. The new files go beside the originals, ending in [LZ4].nsp.");
        lines.Add(("", UiLevel.Info));
        Paragraph("Left and right go between To do, Done and Everything. Enter opens the Details of the game the pointer's on, with what can be done to it: L looks inside it, E extracts it, and V checks it for damage. D shows the log of the last job, and O the options. Everything else is under More, M: what to do with the ticked games, sending games to a Switch, and this help. The letter beside each one also works by itself.");
        lines.Add(("", UiLevel.Info));
        foreach (var topic in Help.Topics)
        {
            lines.Add((topic.Title, UiLevel.Heading));
            foreach (string paragraph in topic.Text.Split("\n\n"))
                Paragraph(paragraph);
            Paragraph($"More: {topic.Url}", UiLevel.Detail);
            lines.Add(("", UiLevel.Info));
        }
        lines.Add(("Reporting a problem", UiLevel.Heading));
        Paragraph($"Press R. It saves a report of what happened, with no keys in it, to post with what you were doing as a new issue at {Help.NewIssue}");
        lines.Add(("", UiLevel.Info));
        lines.Add(("通过命令行", UiLevel.Heading));
        Paragraph("以上操作也都能在命令行完成，运行 hacbrewpacklz --help 可查看下列说明。", UiLevel.Detail);
        lines.Add(("", UiLevel.Info));
        return lines;
    }

    private List<string> DrawHelp(int width, int height)
    {
        var rows = new List<string>();
        Header(rows, width, "帮助", Sgr.Dim);
        int footer = height - 3;
        var lines = HomeHelp(width - Margin * 2);
        lines.AddRange(HelpText.Lines(width - Margin * 2));
        int visible = Math.Max(footer - rows.Count, 1);
        _helpScroll = Math.Clamp(_helpScroll, 0, Math.Max(lines.Count - visible, 0));
        foreach (var (text, level) in lines.Skip(_helpScroll).Take(visible))
            rows.Add(new Line(width).To(Margin).Add(text, Sgr.For(level)).ToString());
        Pad(rows, footer);
        rows.Add(RuleRow(width));
        rows.Add(Hints(width, ("Up/Down", "滚动"), ("R", "Report a problem"), ("Esc", "返回")));
        rows.Add("");
        return rows;
    }

    // ---------------------------------------------------------------------------------------------------------------
    // Keys

    private void ReadKeys()
    {
        while (Console.KeyAvailable)
        {
            var burst = new List<ConsoleKeyInfo> { Console.ReadKey(intercept: true) };
            while (Console.KeyAvailable)
                burst.Add(Console.ReadKey(intercept: true));
            if (Runs(burst).Any(run => run.Count > 1 && LooksTyped(run)))
            {
                // Dropped files arrive as a burst of typing. Give a slow console a moment to deliver the rest of it.
                Thread.Sleep(20);
                while (Console.KeyAvailable)
                    burst.Add(Console.ReadKey(intercept: true));
            }

            // A key pressed while a path comes in, or piled up with it while the screen fell behind, such as an arrow or Enter, arrives in the
            // same burst. What's typed is taken as the path it is, and the rest as the keys they are, in the order they came: taken all as keys,
            // the letters of a path would be C, R, O and Q.
            _dirty = true;
            foreach (var run in Runs(burst))
            {
                if (run.Count >= PasteLength && LooksTyped(run))
                    HandlePaste(new string(run.Select(k => k.KeyChar).ToArray()));
                else
                    foreach (var key in run)
                        HandleKey(key);
            }
        }
    }

    /// <summary>A burst of keys in the order they came, split where what types something meets what doesn't, such as an arrow or Enter.</summary>
    private static List<List<ConsoleKeyInfo>> Runs(List<ConsoleKeyInfo> burst)
    {
        var runs = new List<List<ConsoleKeyInfo>>();
        foreach (var key in burst)
        {
            bool typed = key.KeyChar >= ' ';
            if (runs.Count == 0 || typed != (runs[^1][0].KeyChar >= ' '))
                runs.Add([]);
            runs[^1].Add(key);
        }
        return runs;
    }

    /// <summary>
    /// Whether keys that came in together are text rather than one key held down, which repeats faster than the screen
    /// redraws and piles up the same way. Arrows and the like type no character at all.
    /// </summary>
    private static bool LooksTyped(List<ConsoleKeyInfo> burst) =>
        burst.All(k => k.KeyChar >= ' ') && burst.Any(k => k.KeyChar != burst[0].KeyChar);

    private void HandlePaste(string text)
    {
        if (_prompt is Prompt.EditOption or Prompt.Find or Prompt.ReportSaid)
        {
            var paths = PathList.Parse(text);
            _input += paths.Count == 1 ? paths[0] : text;
            return;
        }

        // A drop is a drop whatever's open. A menu or a question would otherwise take the letters of its path as keys.
        if (_prompt != Prompt.Path)
            _prompt = Prompt.None;
        var found = PathList.Parse(_input + text);
        if (found.Count > 0 && found.All(p => File.Exists(p) || Directory.Exists(p)))
        {
            _prompt = Prompt.None;
            _input = "";
            _message = null;
            _session.AddInputs(found);
            // Games dropped on the welcome are as good as starting.
            if (_view == View.Welcome)
                LeaveOptions();
            else if (_view != View.Job)
                _view = View.Titles;
        }
        else if (_prompt == Prompt.Path)
        {
            _input += text;
        }
        else
        {
            _message = $"Couldn't find: {text}";
        }
    }

    private void HandleKey(ConsoleKeyInfo key)
    {
        if (key.Key == ConsoleKey.C && key.Modifiers.HasFlag(ConsoleModifiers.Control))
        {
            HandleCancelKey();
            return;
        }

        switch (_prompt)
        {
            case Prompt.Path or Prompt.Find or Prompt.EditOption or Prompt.ReportSaid:
                HandleTextKey(key);
                return;
            case Prompt.Menu:
                HandleMenuKey(key);
                return;
            case Prompt.ConfirmStop:
                if (key.Key == ConsoleKey.Y)
                    _session.Stop();
                if (key.Key is ConsoleKey.Y or ConsoleKey.N or ConsoleKey.Escape)
                    _prompt = Prompt.None;
                return;
            case Prompt.ConfirmQuit:
                if (key.Key == ConsoleKey.Y)
                    _quit = true;
                if (key.Key is ConsoleKey.Y or ConsoleKey.N or ConsoleKey.Escape)
                    _prompt = Prompt.None;
                return;
            case Prompt.AskAboutProblems:
                if (key.Key is ConsoleKey.Y or ConsoleKey.N or ConsoleKey.Escape)
                {
                    _session.Settings.AnswerAboutProblems(key.Key == ConsoleKey.Y);
                    _prompt = Prompt.None;
                }
                return;
            case Prompt.NoUpdate:
                if (key.Key == ConsoleKey.U)
                    _session.UntickWithoutUpdate();
                if (key.Key is ConsoleKey.U or ConsoleKey.R or ConsoleKey.Escape)
                    _prompt = Prompt.None;
                if (key.Key is ConsoleKey.U or ConsoleKey.R)
                    StartCompressing();
                return;
        }

        switch (_view)
        {
            case View.Job:
                HandleJobKey(key);
                break;
            case View.Details:
                HandleDetailsKey(key);
                break;
            case View.Inside:
                HandleInsideKey(key);
                break;
            case View.Report:
                HandleReportKey(key);
                break;
            case View.Options or View.Welcome:
                HandleOptionsKey(key);
                break;
            case View.Help:
                HandleHelpKey(key);
                break;
            default:
                HandleTitlesKey(key);
                break;
        }
    }

    private void HandleCancelKey()
    {
        var snapshot = _session.Snapshot();
        if (snapshot.IsBusy)
            _prompt = IsLongJob(snapshot.Activity) ? Prompt.ConfirmStop : Prompt.None;
        // Nothing of these is written, so they stop straight away.
        if (snapshot.Activity is SessionActivity.Analyzing or SessionActivity.Measuring or SessionActivity.Reading)
            _session.Stop();
        else if (!snapshot.IsBusy)
            _prompt = Prompt.ConfirmQuit;
    }

    private void HandleTextKey(ConsoleKeyInfo key)
    {
        switch (key.Key)
        {
            case ConsoleKey.Enter:
                if (_prompt == Prompt.ReportSaid)
                {
                    _reportSaid = _input.Trim();
                    _prompt = Prompt.None;
                    _input = "";
                }
                else if (_prompt == Prompt.EditOption)
                {
                    var option = _options[_optionSelected];
                    string value = _input.Trim().Trim('"');
                    _prompt = Prompt.None;
                    _input = "";
                    ChangeOption(option, value);
                }
                else if (_prompt == Prompt.Find)
                {
                    string typed = _input.Trim();
                    _prompt = Prompt.None;
                    _input = "";
                    FindInside(typed);
                }
                else
                {
                    var paths = PathList.Parse(_input);
                    _prompt = Prompt.None;
                    _input = "";
                    if (paths.Count > 0)
                        _session.AddInputs(paths);
                }
                break;
            case ConsoleKey.Escape:
                _prompt = Prompt.None;
                _input = "";
                break;
            case ConsoleKey.Backspace:
                if (_input.Length > 0)
                    _input = _input[..^1];
                break;
            default:
                if (key.KeyChar >= ' ')
                    _input += key.KeyChar;
                break;
        }
    }

    /// <summary>What was written for the title the pointer's on, or else by the last job, as the GUI's folder buttons show.</summary>
    private string? OutputToShow(SessionSnapshot s) =>
        (_selected < s.Rows.Length ? s.Rows[_selected].OutputPath : null) ?? s.LastOutput;

    private void HandleMenuKey(ConsoleKeyInfo key)
    {
        var groups = MenuGroups(_session.Snapshot());
        var flat = groups.SelectMany(g => g.Choices).ToList();
        // Where each page starts, for left and right, which go to the same line of the page beside, or its last.
        var starts = new List<int>();
        for (int g = 0, start = 0; g < groups.Count; start += groups[g].Choices.Count, g++)
            starts.Add(start);
        int group = starts.FindLastIndex(start => start <= _menuSelected);
        int line = _menuSelected - starts[Math.Max(group, 0)];
        MenuChoice? chosen = null;
        switch (key.Key)
        {
            case ConsoleKey.UpArrow:
                _menuSelected = Math.Max(_menuSelected - 1, 0);
                return;
            case ConsoleKey.DownArrow:
                _menuSelected = Math.Min(_menuSelected + 1, flat.Count - 1);
                return;
            case ConsoleKey.LeftArrow when group > 0:
                _menuSelected = starts[group - 1] + Math.Min(line, groups[group - 1].Choices.Count - 1);
                return;
            case ConsoleKey.RightArrow when group < groups.Count - 1:
                _menuSelected = starts[group + 1] + Math.Min(line, groups[group + 1].Choices.Count - 1);
                return;
            case ConsoleKey.Escape or ConsoleKey.Backspace or ConsoleKey.M:
                _prompt = Prompt.None;
                return;
            case ConsoleKey.Enter or ConsoleKey.Spacebar:
                chosen = flat[Math.Clamp(_menuSelected, 0, flat.Count - 1)];
                break;
            default:
                // A choice's own key does it straight away, as it does on the list.
                string pressed = key.Key == ConsoleKey.Delete ? "Del" : key.Key.ToString();
                chosen = flat.FirstOrDefault(c => c.Key == pressed);
                if (chosen is null)
                    return;
                break;
        }
        _prompt = Prompt.None;
        chosen.Act();
    }

    private void HandleTitlesKey(ConsoleKeyInfo key)
    {
        var snapshot = _session.Snapshot();
        _message = null;
        if (snapshot.ShowingJob)
        {
            HandleJobViewKey(key, snapshot);
            return;
        }
        if (Blocked(snapshot))
        {
            if (key.Key == ConsoleKey.Escape)
                _session.Stop();
            else if (key.Key == ConsoleKey.D)
                _view = View.Job;
            return;
        }

        // Measuring goes on behind the list, and Esc stops it, as the hint says.
        if (key.Key == ConsoleKey.Escape && snapshot.Activity == SessionActivity.Measuring)
        {
            _session.Stop();
            return;
        }

        int count = ListCount(snapshot);
        switch (key.Key)
        {
            case ConsoleKey.UpArrow:
                _selected = Math.Max(_selected - 1, 0);
                break;
            case ConsoleKey.DownArrow:
                _selected = Math.Min(_selected + 1, Math.Max(count - 1, 0));
                break;
            case ConsoleKey.PageUp:
                _selected = Math.Max(_selected - 10, 0);
                break;
            case ConsoleKey.PageDown:
                _selected = Math.Min(_selected + 10, Math.Max(count - 1, 0));
                break;
            case ConsoleKey.Home:
                _selected = 0;
                break;
            case ConsoleKey.End:
                _selected = Math.Max(count - 1, 0);
                break;
            case ConsoleKey.C when snapshot.Rows.Length > 0:
                if (_session.TickedWithoutUpdate().Length > 0)
                    _prompt = Prompt.NoUpdate;
                else
                    StartCompressing();
                break;
            case ConsoleKey.E or ConsoleKey.V when _selected < snapshot.Rows.Length:
                DoToOne(key.Key, snapshot.Rows[_selected].Key);
                break;
            case ConsoleKey.L when _selected < snapshot.Rows.Length && !snapshot.Rows[_selected].IsFolder:
                LookInside(snapshot.Rows[_selected].Key);
                break;
            case ConsoleKey.A when snapshot.Rows.Length > 0 && !snapshot.IsBusy:
                if (!_session.Analyze())
                    _message = snapshot.Rows.Any(r => r.Selected) ? "这些都已有分析结果。" : "尚未勾选任何游戏，请按空格键勾选。";
                break;
            case ConsoleKey.Spacebar when _selected < snapshot.Rows.Length:
                _session.Toggle(snapshot.Rows[_selected]);
                break;
            case ConsoleKey.T when snapshot.Rows.Length > 0:
                _session.SelectAll(!snapshot.Rows.All(r => r.Selected));
                break;
            case ConsoleKey.Delete when _selected < snapshot.Rows.Length:
                _session.Remove(snapshot.Rows[_selected]);
                break;
            case ConsoleKey.Delete when _selected < count:
                _session.Remove(snapshot.Issues[_selected - snapshot.Rows.Length]);
                break;
            case ConsoleKey.Enter or ConsoleKey.I when _selected < count:
                OpenDetails(_selected < snapshot.Rows.Length ? snapshot.Rows[_selected].Key : "issue|" + snapshot.Issues[_selected - snapshot.Rows.Length].Path);
                break;
            case ConsoleKey.D:
                _view = View.Job;
                break;
            case ConsoleKey.P:
                _prompt = Prompt.Path;
                _input = "";
                break;
            case ConsoleKey.LeftArrow:
                StepView(snapshot, -1);
                break;
            case ConsoleKey.RightArrow:
                StepView(snapshot, 1);
                break;
            case ConsoleKey.S when snapshot.Rows.Length > 0:
                _session.SendToSwitch();
                break;
            case ConsoleKey.M:
                _prompt = Prompt.Menu;
                _menuSelected = _menuFirst = 0;
                break;
            case ConsoleKey.X:
                _session.RemoveAll();
                break;
            case ConsoleKey.F when OutputToShow(snapshot) is { } output:
                Explorer.Show(output);
                break;
            case ConsoleKey.O:
                _view = View.Options;
                break;
            case ConsoleKey.H or ConsoleKey.F1:
                OpenHelp(null);
                break;
            case ConsoleKey.R:
                OpenReport();
                break;
            case ConsoleKey.Q:
                _quit = true;
                break;
            case ConsoleKey.Escape when count > 0:
                _prompt = Prompt.ConfirmQuit;
                break;
            case ConsoleKey.Escape:
                _quit = true;
                break;
        }
    }

    /// <summary>A job's own view: Esc stops it while it runs. Once it's over the pointer goes through its games, and Enter goes back to the list,
    /// where what it compressed is under Done.</summary>
    private void HandleJobViewKey(ConsoleKeyInfo key, SessionSnapshot s)
    {
        var job = s.Job!;
        if (key.Key == ConsoleKey.D)
        {
            _view = View.Job;
            return;
        }
        if (job.Running)
        {
            if (key.Key == ConsoleKey.Escape)
                _prompt = Prompt.ConfirmStop;
            return;
        }
        switch (key.Key)
        {
            case ConsoleKey.UpArrow:
                _jobSelected = Math.Max(_jobSelected - 1, 0);
                break;
            case ConsoleKey.DownArrow:
                _jobSelected = Math.Min(_jobSelected + 1, Math.Max(s.Rows.Length - 1, 0));
                break;
            case ConsoleKey.PageUp:
                _jobSelected = Math.Max(_jobSelected - 10, 0);
                break;
            case ConsoleKey.PageDown:
                _jobSelected = Math.Min(_jobSelected + 10, Math.Max(s.Rows.Length - 1, 0));
                break;
            case ConsoleKey.Home:
                _jobSelected = 0;
                break;
            case ConsoleKey.End:
                _jobSelected = Math.Max(s.Rows.Length - 1, 0);
                break;
            case ConsoleKey.I when _jobSelected < s.Rows.Length:
                OpenDetails(s.Rows[_jobSelected].Key);
                break;
            case ConsoleKey.Enter or ConsoleKey.Escape or ConsoleKey.Backspace:
                _session.BackToList();
                break;
            case ConsoleKey.S when job.Kind == SessionActivity.Compressing && DoneCount(s) > 0:
                _session.SendToSwitch();
                break;
            case ConsoleKey.F when JobOutput(s) is { } output:
                Explorer.Show(output);
                break;
            case ConsoleKey.Q:
                _quit = true;
                break;
        }
    }


    /// <summary>Details: Esc stops a job that's running, and otherwise goes back, as D does.</summary>
    private void HandleJobKey(ConsoleKeyInfo key)
    {
        var snapshot = _session.Snapshot();
        if (snapshot.IsBusy)
        {
            if (key.Key == ConsoleKey.Escape && IsLongJob(snapshot.Activity))
                _prompt = Prompt.ConfirmStop;
            else if (key.Key == ConsoleKey.Escape)
                _session.Stop();
            else if (key.Key == ConsoleKey.D)
                _view = View.Titles;
            return;
        }
        if (key.Key is ConsoleKey.Enter or ConsoleKey.Escape or ConsoleKey.Backspace or ConsoleKey.D)
            _view = View.Titles;
        else if (key.Key == ConsoleKey.Q)
            _quit = true;
    }

    private void HandleOptionsKey(ConsoleKeyInfo key)
    {
        var option = _options[_optionSelected];
        _message = null;
        switch (key.Key)
        {
            case ConsoleKey.UpArrow:
                _optionSelected = Math.Max(_optionSelected - 1, 0);
                break;
            case ConsoleKey.DownArrow:
                _optionSelected = Math.Min(_optionSelected + 1, _options.Count - 1);
                break;
            case ConsoleKey.LeftArrow:
                AdjustOption(-1);
                break;
            case ConsoleKey.RightArrow:
                AdjustOption(1);
                break;
            case ConsoleKey.Delete when option.Kind != OptionKind.Action:
                ChangeOption(option, "");
                break;
            case ConsoleKey.Enter or ConsoleKey.Spacebar:
                if (option == StartSpec)
                {
                    LeaveOptions();
                }
                else if (option == OptionCatalog.ShowWelcome)
                {
                    _view = View.Welcome;
                    _optionSelected = 0;
                    _optionsFirst = 0;
                }
                else if (option.Kind == OptionKind.Action)
                {
                    _session.CheckForUpdates();
                }
                else if (option.Kind == OptionKind.Toggle)
                {
                    ToggleOption(option);
                }
                else if (option.Kind is OptionKind.Slider or OptionKind.Choice)
                {
                    AdjustOption(1);
                }
                else
                {
                    _input = _session.Settings.Get(option.Flag);
                    _prompt = Prompt.EditOption;
                }
                break;
            case ConsoleKey.Escape or ConsoleKey.Backspace or ConsoleKey.O:
                LeaveOptions();
                break;
            case ConsoleKey.S when _view == View.Welcome:
                LeaveOptions();
                break;
            case ConsoleKey.Q:
                _quit = true;
                break;
        }
    }

    /// <summary>Back to the list, from the options or the welcome, whose lists aren't the same length.</summary>
    private void LeaveOptions()
    {
        if (_view == View.Welcome)
        {
            _optionSelected = 0;
            _optionsFirst = 0;
            _session.Settings.Welcomed = true;
            _session.Settings.Save();
        }
        _view = View.Titles;
    }

    /// <summary>Saved where people will find it, shown there, and what to do with it said in the status.</summary>
    private void OpenReport()
    {
        _reportText = ReportSender.Prepared(_session.ReportText());
        _reportSaid = "";
        _reportScroll = 0;
        _prompt = Prompt.None;
        _view = View.Report;
    }

    /// <summary>Saved where people will find it, shown there, and what's in it said.</summary>
    private void SaveReport()
    {
        try
        {
            string text = _reportSaid.Length > 0 ? $"What I was doing: {_reportSaid}{Environment.NewLine}{Environment.NewLine}{_reportText}" : _reportText;
            string path = Help.SaveReport(text);
            Help.ShowReport(path);
            _message = $"Report saved to {path}, with no keys in it.";
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
            _message = $"The report couldn't be saved: {e.Message}";
        }
        _view = View.Titles;
    }

    /// <summary>On its way beside the screen, which keeps answering, and back on the list with what came of it.</summary>
    private void SendReport()
    {
        if (_sending)
            return;
        _sending = true;
        string text = _reportText, said = _reportSaid;
        string title = said.Length > 0 ? said[..Math.Min(said.Length, 80)] : "A report from the home screen";
        new Thread(() =>
        {
            var outcome = ReportSender.Send("manual", "", title, said, text);
            _message = outcome.Said;
            _sending = false;
            _view = View.Titles;
            _dirty = true;
        }) { IsBackground = true, Name = "Sending a report" }.Start();
    }

    private void HandleReportKey(ConsoleKeyInfo key)
    {
        if (_sending)
            return;
        switch (key.Key)
        {
            case ConsoleKey.W:
                _input = _reportSaid;
                _prompt = Prompt.ReportSaid;
                break;
            // With nowhere to send it yet, saving it is all there is.
            case ConsoleKey.S or ConsoleKey.Enter:
                if (ReportSender.CanSend)
                    SendReport();
                else
                    SaveReport();
                break;
            case ConsoleKey.F when ReportSender.CanSend:
                SaveReport();
                break;
            case ConsoleKey.UpArrow:
                _reportScroll--;
                break;
            case ConsoleKey.DownArrow:
                _reportScroll++;
                break;
            case ConsoleKey.PageUp:
                _reportScroll -= 10;
                break;
            case ConsoleKey.PageDown or ConsoleKey.Spacebar:
                _reportScroll += 10;
                break;
            case ConsoleKey.Escape or ConsoleKey.Backspace:
                _view = View.Titles;
                break;
        }
    }

    /// <summary>What was done, as it'll be sent, to read through first, with what the person was doing above it.</summary>
    private List<string> DrawReport(int width, int height)
    {
        var rows = new List<string>();
        Header(rows, width, "Report a problem", Sgr.Dim);
        foreach (string row in Terminal.Wrap(ReportSender.CanSend
            ? $"This goes straight to the developer: {OptionCatalog.ReportHolds}. Saying what you were doing helps most."
            : $"It's saved as a file to send to the developer: {OptionCatalog.ReportHolds}. Saying what you were doing helps most.", width - Margin * 2))
            rows.Add(new Line(width).To(Margin).Add(row, Sgr.Dim).ToString());
        rows.Add("");
        rows.Add(new Line(width).To(Margin).Add("What you were doing", Sgr.Strong).ToString());
        _promptRow = rows.Count;
        if (_prompt == Prompt.ReportSaid)
            rows.Add(new Line(width).To(Margin).Add("> ", Sgr.Accent).Add(_input).ToString());
        else if (_reportSaid.Length > 0)
            foreach (string row in Terminal.Wrap(_reportSaid, width - Margin * 2))
                rows.Add(new Line(width).To(Margin).Add(row).ToString());
        else
            rows.Add(new Line(width).To(Margin).Add("Nothing yet: W writes it", Sgr.Faint).ToString());
        rows.Add("");
        rows.Add(RuleRow(width));
        int footer = height - 4;
        var lines = _reportText.Replace("\r", "").Split('\n').SelectMany(l => l.Length == 0 ? [""] : Terminal.Wrap(l, width - Margin * 2, 4)).ToList();
        int visible = Math.Max(footer - rows.Count, 1);
        _reportScroll = Math.Clamp(_reportScroll, 0, Math.Max(lines.Count - visible, 0));
        foreach (string row in lines.Skip(_reportScroll).Take(visible))
            rows.Add(new Line(width).To(Margin).Add(row, Sgr.Dim).ToString());
        Pad(rows, footer);
        rows.Add(RuleRow(width));
        rows.Add(_sending ? new Line(width).To(Margin).Add("Sending" + Terminal.Ellipsis, Sgr.Accent).ToString() : "");
        rows.Add(_prompt == Prompt.ReportSaid ? Hints(width, ("Enter", "Done"), ("Esc", "Cancel"))
            : ReportSender.CanSend ? Hints(width, ("S", "Send"), ("W", "Write what you were doing"), ("F", "Save instead"), ("Up/Down", "滚动"), ("Esc", "返回"))
            : Hints(width, ("S", "Save it"), ("W", "Write what you were doing"), ("Up/Down", "滚动"), ("Esc", "返回")));
        rows.Add("");
        return rows;
    }

    private void HandleHelpKey(ConsoleKeyInfo key)
    {
        switch (key.Key)
        {
            case ConsoleKey.UpArrow:
                _helpScroll--;
                break;
            case ConsoleKey.DownArrow:
                _helpScroll++;
                break;
            case ConsoleKey.PageUp:
                _helpScroll -= 10;
                break;
            case ConsoleKey.PageDown or ConsoleKey.Spacebar:
                _helpScroll += 10;
                break;
            case ConsoleKey.Home:
                _helpScroll = 0;
                break;
            case ConsoleKey.Escape or ConsoleKey.Backspace or ConsoleKey.H or ConsoleKey.Enter:
                _view = View.Titles;
                break;
            case ConsoleKey.R:
                OpenReport();
                break;
            case ConsoleKey.Q:
                _quit = true;
                break;
        }
    }
}
