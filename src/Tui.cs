using System.Text;

namespace HacBrewPackLz;

/// <summary>
/// The screen people get by double clicking the program or dropping files on it. It takes over the whole window and
/// draws every row to fit the window's current size, so resizing just draws it again. It stays open until told to quit.
/// </summary>
internal sealed class Tui
{
    private const int MinimumWidth = 30;
    private const int MinimumHeight = 8;
    private const int Margin = 2;
    private const int PasteLength = 4;

    private enum View
    {
        Titles,
        Job,
        Options,
        Help,
    }

    private enum Prompt
    {
        None,
        Path,
        Check,
        Menu,
        ConfirmStop,
        ConfirmQuit,
        NoUpdate,
        EditOption,
    }

    private readonly Session _session = new();
    private readonly List<OptionSpec> _options;
    private View _view;
    private Prompt _prompt;
    private string _input = "";
    private int _promptRow;
    private int _menuSelected;
    private int _menuFirst;
    private int _menuColumnRows = 1;
    private readonly List<string?> _checking = [];
    private string? _message;
    private int _selected;
    private int _optionSelected;
    private int _titlesFirst;
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

    private Tui()
    {
        _options = OptionCatalog.GeneralGroups.SelectMany(g => g.Options).ToList();
        _session.Changed += () => _dirty = true;
    }

    public static int Run(IReadOnlyList<string> inputs)
    {
        var tui = new Tui();
        Ui.Sink = tui._session;
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
                ReadKeys();
                var size = (Terminal.Width, Terminal.Height);
                // The spinner turns on its own, so it can be seen that reading hasn't stuck.
                if (_spinning && Environment.TickCount64 / SpinnerStep != _spinnerFrame)
                    _dirty = true;
                if (size != (_width, _height) || _dirty)
                    Draw(size.Width, size.Height);
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
        if (_view == View.Job && !IsLongJob(snapshot.Activity) && snapshot.Summary is null && !snapshot.IsBusy)
            _view = View.Titles;

        // One cell is left free so nothing can ever touch the right edge while the window is being resized.
        int usable = width - 1;
        List<string> rows = width < MinimumWidth || height < MinimumHeight
            ? [new Line(usable).Add(AppInfo.Name + "：请放大此窗口", Sgr.Dim).ToString()]
            : _view switch
            {
                View.Job => DrawJob(snapshot, usable, height),
                View.Options => DrawOptions(usable, height),
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
                output.Append($"\x1b[{y + 1};1H\x1b[K").Append(row);
                _shown[y] = row;
            }
        }

        bool editing = _prompt is Prompt.Path or Prompt.Check or Prompt.EditOption;
        if (editing)
        {
            int column = Math.Min(Margin + Terminal.DisplayWidth(PromptQuestion()) + 2 + Terminal.DisplayWidth(_input), usable - 1);
            output.Append($"\x1b[{_promptRow + 1};{column + 1}H\x1b[?25h");
        }
        else
        {
            output.Append("\x1b[?25l");
        }
        Console.Out.Write(output.ToString());
    }

    /// <summary>
    /// The first row to show, moved only as far as it takes to keep the selected row on screen. Moving the selection
    /// then changes two rows rather than all of them, which a console window redraws a lot faster.
    /// </summary>
    private static int KeepInView(int first, int selected, int visible, int count) =>
        Math.Clamp(Math.Clamp(first, selected - visible + 1, selected), 0, Math.Max(count - visible, 0));

    private static bool IsLongJob(SessionActivity activity) =>
        activity is SessionActivity.Compressing or SessionActivity.Extracting or SessionActivity.Verifying;

    // ---------------------------------------------------------------------------------------------------------------
    // Shared parts of the screen

    private static void Header(List<string> rows, int width, string right, string rightStyle)
    {
        rows.Add("");
        rows.Add(new Line(width).To(Margin).Add(AppInfo.Name, Sgr.Title).Add("  ").Add(AppInfo.Version, Sgr.Dim).Right(right, rightStyle).ToString());
        rows.Add(RuleRow(width));
    }

    private static string RuleRow(int width) =>
        new Line(width).To(Margin).Add(new string(Terminal.Rule, Math.Max(width - Margin * 2, 0)), Sgr.Dim).ToString();

    private static void Pad(List<string> rows, int upTo)
    {
        while (rows.Count < upTo)
            rows.Add("");
    }

    /// <summary>Key hints. In a narrow window the middle ones go first, and the last one, usually how to leave, always stays.</summary>
    private static string Hints(int width, params (string key, string label)[] hints) => HintRows(width, 1, hints)[0];

    /// <summary>
    /// The keys to press, over as many rows as it takes up to <paramref name="maxRows"/>. Only when that isn't enough
    /// are any left out, from the end but always keeping the last, which is how to leave.
    /// </summary>
    private static List<string> HintRows(int width, int maxRows, params (string key, string label)[] hints)
    {
        const int Gap = 3;
        static int Cells((string key, string label) hint) => Terminal.DisplayWidth(hint.key) + 1 + Terminal.DisplayWidth(hint.label);

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

        return packed.Select(row =>
        {
            var line = new Line(width).To(Margin);
            for (int i = 0; i < row.Count; i++)
            {
                if (i > 0)
                    line.Add(new string(' ', Gap));
                line.Add(row[i].key, Sgr.Accent).Add(" " + row[i].label);
            }
            return line.ToString();
        }).ToList();
    }

    // ---------------------------------------------------------------------------------------------------------------
    // The list of titles

    private List<string> DrawTitles(SessionSnapshot s, int width, int height)
    {
        var rows = new List<string>();
        string keyState = s.KeyProblem is not null ? "keys not found" : s.Rows.Length > 0 ? $"{s.Rows.Length} title{(s.Rows.Length == 1 ? "" : "s")}" : "";
        Header(rows, width, keyState, s.KeyProblem is not null ? Sgr.Warn : Sgr.Dim);

        // Only the keys for what comes next are along the bottom. The rest are in the menu, which opens upwards over the
        // end of the list.
        var hints = HintRows(width, 2, TitleHints(s));
        int footer = height - 3 - hints.Count;
        if (s.Rows.Length == 0 && s.Issues.Length == 0)
            DrawEmpty(rows, s, width, footer);
        else
            DrawTable(rows, s, width, footer);

        Pad(rows, footer);
        if (_prompt == Prompt.Menu)
        {
            var menu = MenuRows(MenuItems(s), width, footer - 3);
            rows.RemoveRange(rows.Count - menu.Count, menu.Count);
            rows.AddRange(menu);
        }
        _promptRow = rows.Count;
        rows.Add(StatusRow(s, width));
        rows.Add(RuleRow(width));
        rows.AddRange(hints);
        rows.Add("");
        return rows;
    }

    private (string key, string label)[] TitleHints(SessionSnapshot s)
    {
        if (s.IsBusy)
            return [("Esc", "停止")];
        switch (_prompt)
        {
            case Prompt.Path:
                return [("Enter", "添加"), ("Esc", "取消")];
            case Prompt.Check:
                return [("Enter", _checking.Count < 2 ? "下一步" : "校验"), ("Esc", "取消")];
            case Prompt.ConfirmQuit:
                return [("Y", "退出"), ("N", "留下")];
            case Prompt.NoUpdate:
                return [("U", "取消勾选它们"), ("R", "仍一并重打包"), ("Esc", "取消")];
            case Prompt.Menu:
                return [("Enter", "选择"), ("Esc", "关闭")];
        }

        if (s.Rows.Length == 0)
            return [("P", "输入路径"), ("O", "设置"), ("M", "更多"), ("Q", "退出")];
        var hints = new List<(string key, string label)>();
        if (s.Rows.Any(r => r.Selected && r.NeedsEstimate))
            hints.Add(("A", "分析"));
        if (s.Rows.Any(r => r.Selected && !r.IsDone(BatchMode.Convert)))
            hints.Add(("C", CompressLabel()));
        else if (OutputToShow(s) is not null)
            hints.Add(("F", "在文件夹中显示"));
        hints.AddRange([("Space", "勾选"), ("O", "设置"), ("M", "更多"), ("Q", "退出")]);
        return hints.ToArray();
    }

    /// <summary>Everything else that can be done right now, with the key that does it straight away.</summary>
    private List<(string key, string label)> MenuItems(SessionSnapshot s)
    {
        var items = new List<(string key, string label)>();
        if (s.Rows.Length > 0)
            items.AddRange([("S", "发送到 Switch"), ("E", "解包到文件夹")]);
        if (OutputToShow(s) is not null)
            items.Add(("F", "在文件夹中显示"));
        if (s.Rows.Length > 0)
            items.AddRange([("T", s.Rows.All(r => r.Selected) ? "Untick all" : "Tick all"), ("D", "从列表移除此项")]);
        items.AddRange([("P", "添加路径"), ("V", "校验 NSP")]);
        if (s.Rows.Length > 0 || s.Issues.Length > 0)
            items.Add(("X", "清空列表"));
        items.Add(("H", "帮助"));
        return items;
    }

    /// <summary>
    /// The menu in as many columns as fit across the window, filled from the top down so related items stay together.
    /// Only in a window too short for it does it scroll.
    /// </summary>
    private List<string> MenuRows(List<(string key, string label)> items, int width, int room)
    {
        const int Gap = 3;
        int labelWidth = items.Max(i => Terminal.DisplayWidth(i.label));
        int cellWidth = Margin + 3 + labelWidth + Gap;
        int columns = Math.Clamp((width - Margin - (Margin + 3 + labelWidth)) / cellWidth + 1, 1, items.Count);
        _menuColumnRows = (items.Count + columns - 1) / columns;
        columns = (items.Count + _menuColumnRows - 1) / _menuColumnRows;

        _menuSelected = Math.Clamp(_menuSelected, 0, items.Count - 1);
        int visible = Math.Clamp(room - 1, 1, _menuColumnRows);
        _menuFirst = KeepInView(_menuFirst, _menuSelected % _menuColumnRows, visible, _menuColumnRows);
        var rows = new List<string> { RuleRow(width) };
        for (int row = _menuFirst; row < _menuFirst + visible; row++)
        {
            var line = new Line(width);
            for (int column = 0; column < columns; column++)
            {
                int i = column * _menuColumnRows + row;
                if (i >= items.Count)
                    break;
                bool chosen = i == _menuSelected;
                int x = column * cellWidth;
                line.To(x).Add(chosen ? Terminal.Pointer.ToString() : " ", Sgr.Accent).To(x + Margin)
                    .Add(items[i].key, Sgr.Accent).To(x + Margin + 3).Add(items[i].label, chosen ? Sgr.Strong : "");
            }
            rows.Add(line.ToString());
        }
        return rows;
    }

    private string CompressLabel() => _session.Settings.Get("--nocompress") == "1" ? "重新打包" : "开始压缩";

    /// <summary>What's being asked for, ahead of what's typed. Checking an NSP asks for it, then for folders to compare it with.</summary>
    private string PromptQuestion() => _prompt != Prompt.Check ? "" : _checking.Count switch
    {
        0 => "待校验的 NSP：",
        1 => "用于比对的 RomFS 文件夹（可选）：",
        _ => "用于比对的 ExeFS 文件夹（可选）：",
    };

    private void StartCompressing()
    {
        if (_session.Compress())
            _view = View.Job;
        else
            _message = _session.Snapshot().Rows.Any(r => r.Selected) ? "这些都已经压缩过了。" : "没有勾选任何项目，按空格键勾选游戏。";
    }

    private static void DrawEmpty(List<string> rows, SessionSnapshot s, int width, int bottom)
    {
        var message = new List<(string text, string style)>();
        if (s.KeyProblem is not null)
        {
            message.Add(("需要先提供密钥", Sgr.Warn));
            message.Add(("", ""));
            message.Add(("请把 prod.keys 放在本程序旁边，或直接拖到本窗口。", ""));
            message.Add(("", ""));
            message.Add(("", ""));
        }
        message.Add(("将游戏文件或文件夹拖到此窗口", Sgr.Strong));
        message.Add(("", ""));
        message.Add(("支持 NSP、NSZ、XCI 和 XCZ，原文件永远不会被修改。", Sgr.Dim));

        int top = rows.Count + Math.Max((bottom - rows.Count - message.Count) / 2, 0);
        Pad(rows, top);
        foreach (var (text, style) in message)
        {
            if (rows.Count >= bottom)
                break;
            string fitted = Terminal.Fit(text, width - Margin * 2);
            rows.Add(new Line(width).To((width - Terminal.DisplayWidth(fitted)) / 2).Add(fitted, style).ToString());
        }
    }

    private void DrawTable(List<string> rows, SessionSnapshot s, int width, int bottom)
    {
        const int SavedColumn = 7;
        const int AfterColumn = 11;
        const int NowColumn = 11;
        bool showNow = width >= 60;
        bool showSaved = width >= 45;
        int right = width - Margin;
        int savedStart = right - (showSaved ? SavedColumn : 0);
        int afterStart = savedStart - AfterColumn;
        int nowStart = afterStart - (showNow ? NowColumn : 0);

        string Row(string pointer, string tick, string name, string nameStyle, string detail, string now, string after, string afterStyle, string saved, string savedStyle)
        {
            var line = new Line(width).Add(pointer, Sgr.Accent).To(Margin);
            if (tick.Length > 0)
                line.Add(tick, Sgr.Accent).Add(" ");
            line.Add(Terminal.Fit(name, nowStart - line.Used - 1), nameStyle);
            if (detail.Length > 0 && line.Used + 3 < nowStart)
                line.Add("  ").Add(Terminal.Fit(detail, nowStart - line.Used - 1), Sgr.Dim);
            if (showNow)
                line.To(afterStart - now.Length).Add(now, Sgr.Dim);
            line.To(savedStart - Terminal.DisplayWidth(after)).Add(after, afterStyle);
            if (showSaved)
                line.To(right - saved.Length).Add(saved, savedStyle);
            return line.ToString();
        }

        _selected = Math.Clamp(_selected, 0, Math.Max(s.Rows.Length - 1, 0));
        rows.Add(Row("", "", "名称", Sgr.Dim, "", "当前", "之后", Sgr.Dim, "节省", Sgr.Dim));

        // Room for the rows, then the total, then whatever needs saying about files that can't be used.
        int reserve = (s.Rows.Length > 1 ? 2 : 0) + Math.Min(s.Issues.Length * 2, 6) + (s.Issues.Length > 0 ? 1 : 0);
        int visible = Math.Max(bottom - rows.Count - reserve, Math.Min(s.Rows.Length, 1));
        int first = _titlesFirst = KeepInView(_titlesFirst, _selected, visible, s.Rows.Length);
        for (int i = first; i < Math.Min(first + visible, s.Rows.Length) && rows.Count < bottom; i++)
        {
            var row = s.Rows[i];
            bool chosen = i == _selected && !s.IsBusy;
            var (after, afterStyle, saved, savedStyle) = RowResult(row);
            string nameStyle = chosen ? Sgr.Strong : row.Selected ? "" : Sgr.Dim;
            rows.Add(Row(chosen ? Terminal.Pointer.ToString() : "", row.Selected ? "[x]" : "[ ]", row.Name, nameStyle, row.DetailWithEstimate, ProgressReporter.Format(row.InputBytes), after, row.Selected ? afterStyle : Sgr.Dim, saved, row.Selected ? savedStyle : Sgr.Dim));
        }

        if (s.Rows.Length > 1 && rows.Count < bottom - 1 && s.Rows.Any(r => r.AfterBytes is not null))
        {
            var known = s.Rows.Where(r => r.Selected && r.AfterBytes is not null).ToList();
            long before = known.Sum(r => r.InputBytes);
            long after = known.Sum(r => r.AfterBytes!.Value);
            rows.Add(RuleRow(width));
            rows.Add(Row("", "", known.Count == s.Rows.Length ? "全部游戏" : $"{known.Count}/{s.Rows.Length} 个标题", "", "",
                ProgressReporter.Format(before), ProgressReporter.Format(after), "", Percent(before, after), Sgr.Good));
        }

        // What there is to know about the title the pointer's on, as the GUI has behind each row's info button, and how many are hidden.
        var notes = new List<string>();
        if (s.Rows.Length > 0 && !s.IsBusy)
            notes.AddRange(s.Rows[_selected].Notes);
        if (s.HiddenCompressed > 0)
            notes.Add($"已隐藏 {s.HiddenCompressed} 个已压缩游戏（可在设置中显示）");
        if (s.HiddenSmall > 0)
            notes.Add($"{s.HiddenSmall} {(s.HiddenSmall == 1 ? "game" : "games")} under 1 MB hidden");
        if (notes.Count > 0 && rows.Count < bottom - 1)
        {
            rows.Add("");
            foreach (string note in notes)
            {
                foreach (string text in Terminal.Wrap(note, width - Margin * 2 - 2, 2))
                {
                    if (rows.Count < bottom)
                        rows.Add(new Line(width).To(Margin).Add(text, Sgr.Dim).ToString());
                }
            }
        }

        if (s.Issues.Length > 0 && rows.Count < bottom - 1)
        {
            rows.Add("");
            foreach (var issue in s.Issues)
            {
                foreach (string text in Terminal.Wrap(issue.Message, width - Margin * 2 - 2, 2))
                {
                    if (rows.Count < bottom)
                        rows.Add(new Line(width).To(Margin).Add(text, Sgr.Warn).ToString());
                }
            }
        }
    }

    private static (string after, string afterStyle, string saved, string savedStyle) RowResult(TitleRow row)
    {
        switch (row.State)
        {
            case RowState.Working:
                return ("处理中", Sgr.Accent, "", "");
            case RowState.Failed:
                return ("失败", Sgr.Bad, "", "");
            case RowState.Done when row.DoneAs == BatchMode.Extract:
                return ("已解包", Sgr.Good, "", "");
            case RowState.Done:
                return (ProgressReporter.Format(row.OutputBytes), Sgr.Good, Percent(row.InputBytes, row.OutputBytes), Sgr.Good);
        }
        if (row.Estimate is not { } estimate)
            return ("", "", "", "");
        string prefix = estimate.MarginBytes > 0 ? "~" : "";
        return (prefix + ProgressReporter.Format(estimate.OutputBytes), "", Percent(estimate.InputBytes, estimate.OutputBytes), estimate.SavedBytes >= 0 ? Sgr.Good : Sgr.Warn);
    }

    private static string Percent(long before, long after)
    {
        if (before <= 0)
            return "";
        double saved = (before - after) * 100.0 / before;
        return saved >= 0 ? $"{saved:0}%" : $"+{-saved:0}%";
    }

    private string StatusRow(SessionSnapshot s, int width)
    {
        var line = new Line(width).To(Margin);
        if (_prompt is Prompt.Path or Prompt.Check)
            return line.Add(PromptQuestion(), Sgr.Strong).Add("> ", Sgr.Accent).Add(_input).ToString();
        if (_prompt == Prompt.ConfirmQuit)
            return line.Add("退出？", Sgr.Strong).ToString();
        // Kept clear while the menu is open, so nothing below it reads as one of its items.
        if (_prompt == Prompt.Menu)
            return "";
        if (_prompt == Prompt.NoUpdate)
        {
            int count = _session.TickedWithoutUpdate().Length;
            return line.Add(count == 1 ? "This one has no update, so repacking it gains nothing." : $"{count} of these have no update, so repacking them gains nothing.", Sgr.Strong).ToString();
        }
        if (_message is not null)
            return line.Add(_message, Sgr.Warn).ToString();
        switch (s.Activity)
        {
            case SessionActivity.Reading:
                _spinning = true;
                line.Add("|/-\\"[(int)(_spinnerFrame % 4)] + " ", Sgr.Accent);
                return line.Add(s.ReadingText + Terminal.Ellipsis, Sgr.Dim).ToString();
            case SessionActivity.Analyzing:
                string title = s.CurrentCount > 0 ? $"正在分析  {s.CurrentIndex} / {s.CurrentCount}：{s.CurrentTitle}" : "正在分析";
                line.Add(title + Terminal.Ellipsis, Sgr.Accent);
                if (s.Progress is { Total: > 0 } p)
                    line.Add($"  {p.Done * 100.0 / p.Total:0}%  {Terminal.TimeLeft(p)}", Sgr.Dim);
                return line.ToString();
        }
        if (s.Rows.Any(r => r.BelowMinimum is not null && !r.Selected))
        {
            int below = s.Rows.Count(r => r.BelowMinimum is not null && !r.Selected);
            return line.Add($"{below} left out for saving too little. Press Space to tick any you want anyway.", Sgr.Dim).ToString();
        }
        var estimated = s.Rows.Where(r => r.Estimate is not null && r.State != RowState.Done).ToList();
        if (estimated.Count > 0)
        {
            long margin = (long)Math.Sqrt(estimated.Sum(r => (double)r.Estimate!.MarginBytes * r.Estimate.MarginBytes));
            return line.Add(margin > 0
                ? $"按文件头采样估算，误差约 {ProgressReporter.Format(margin)}，未写入任何文件。"
                : "根据文件头推算得出，未写入任何文件。", Sgr.Dim).ToString();
        }
        if (s.UpdateNote is not null)
            return line.Add(s.UpdateNote, Sgr.Dim).ToString();
        if (s.Summary is not null)
            return line.Add(s.Summary, Sgr.Dim).ToString();
        if (s.Rows.Length > 0)
            return line.Add("按 A 键预估压缩后的体积，不会写入任何文件。", Sgr.Dim).ToString();
        return "";
    }

    // ---------------------------------------------------------------------------------------------------------------
    // A running or finished job

    private List<string> DrawJob(SessionSnapshot s, int width, int height)
    {
        var rows = new List<string>();
        string state = s.IsBusy
            ? s.CurrentCount > 1 ? $"{ActivityName(s.Activity)}  {s.CurrentIndex} / {s.CurrentCount}" : ActivityName(s.Activity)
            : s.SummaryLevel switch { UiLevel.Success => "已完成", UiLevel.Warning => "已停止", _ => "有问题" };
        Header(rows, width, state, s.IsBusy ? Sgr.Accent : Sgr.For(s.SummaryLevel));
        rows.Add("");
        if (s.CurrentTitle is not null)
            rows.Add(new Line(width).To(Margin).Add(s.CurrentTitle, Sgr.Strong).ToString());
        else if (s.Summary is not null)
            AddWrapped(rows, s.Summary, width, Sgr.For(s.SummaryLevel));

        if (s.Progress is { } p)
        {
            double fraction = p.Total > 0 ? (double)p.Done / p.Total : 1;
            string percent = $"{fraction * 100:0}%";
            rows.Add("");
            rows.Add(new Line(width).To(Margin).Add(p.Label).Right(percent, Sgr.Accent).ToString());
            int barWidth = Math.Max(width - Margin * 2, 1);
            rows.Add(new Line(width).To(Margin).AddStyled(Terminal.Bar(barWidth, fraction), barWidth).ToString());

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
        int footer = height - 4;
        var wrapped = new List<string>();
        foreach (var entry in s.Log)
            AddWrapped(wrapped, entry.Text, width, Sgr.For(entry.Level));
        int room = Math.Max(footer - rows.Count, 0);
        rows.AddRange(wrapped.Skip(Math.Max(wrapped.Count - room, 0)));

        Pad(rows, footer);
        rows.Add(_prompt == Prompt.ConfirmStop ? new Line(width).To(Margin).Add("要停止当前任务吗？已完成的项目会保留。", Sgr.Strong).ToString() : "");
        rows.Add(RuleRow(width));
        rows.Add(_prompt == Prompt.ConfirmStop
            ? Hints(width, ("Y", "停止"), ("N", "继续"))
            : s.IsBusy ? Hints(width, ("Esc", "停止"))
            : s.LastOutput is not null ? Hints(width, ("Enter", "返回"), ("F", "在文件夹中显示"), ("Q", "退出"))
            : Hints(width, ("Enter", "返回"), ("Q", "退出")));
        rows.Add("");
        return rows;
    }

    private static void AddWrapped(List<string> rows, string text, int width, string style)
    {
        foreach (string row in Terminal.Wrap(text, width - Margin * 2, 4))
            rows.Add(new Line(width).To(Margin).Add(row, style).ToString());
    }

    private string ActivityName(SessionActivity activity) => activity switch
    {
        SessionActivity.Compressing => _session.Settings.Get("--nocompress") == "1" ? "正在重打包" : "压缩中",
        SessionActivity.Extracting => "正在解包",
        SessionActivity.Verifying => "正在校验",
        SessionActivity.Sending => "正在发送到 Switch",
        SessionActivity.Analyzing => "正在分析",
        SessionActivity.Reading => "正在读取",
        _ => "",
    };

    // ---------------------------------------------------------------------------------------------------------------
    // Options

    private List<string> DrawOptions(int width, int height)
    {
        var rows = new List<string>();
        Header(rows, width, "设置", Sgr.Dim);

        var body = new List<(string row, int option)>();
        const int ValueColumn = 30;
        int valueStart = Math.Min(Margin + ValueColumn, width / 2);
        foreach (var group in OptionCatalog.GeneralGroups)
        {
            if (body.Count > 0)
                body.Add(("", -1));
            body.Add((new Line(width).To(Margin).Add(group.Title, Sgr.Dim).ToString(), -1));
            foreach (var option in group.Options)
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
        describeRows = Math.Clamp(describeRows, 1, Math.Max(1, Math.Min(4, height - rows.Count - 8)));
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
            ("上/下", "选择"),
            option.Kind switch
            {
                OptionKind.Toggle => ("Enter", "更改"),
                OptionKind.Slider or OptionKind.Choice => ("左/右", "更改"),
                OptionKind.Action => ("Enter", option.Verb),
                _ => ("Enter", "编辑"),
            },
        };
        if (!IsDefault(option))
            hints.Add(("Del", "恢复默认"));
        hints.Add(("Esc", "返回"));
        return hints.ToArray();
    }

    private string OptionValue(OptionSpec option)
    {
        string value = _session.Settings.Get(option.Flag);
        return option.Kind switch
        {
            OptionKind.Toggle => option.ToggleOn(value) != option.Inverted ? "On" : "Off",
            OptionKind.Slider or OptionKind.Choice => value.Length > 0 ? value : option.Default,
            // How checking went, the only action there is.
            OptionKind.Action => _session.Snapshot().UpdateNote ?? "",
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
        Paragraph("3. 按 C 键压缩已勾选的游戏。新文件与原文件同目录，文件名以 [LZ4].nsp 结尾。");
        lines.Add(("", UiLevel.Info));
        Paragraph("O opens the options, where updates are checked for too. Everything else is under More, M: sending games to a Switch, extracting, checking an NSP and this help. The letter beside each one also works by itself.");
        lines.Add(("", UiLevel.Info));
        lines.Add(("安装压缩游戏之前", UiLevel.Heading));
        Paragraph("The console needs firmware 12.0.0 or newer, and sigpatches. Uninstall the game's old update first, and don't install official updates over a compressed game: give the game and its update together instead, and they become one.");
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
        rows.Add(Hints(width, ("上/下", "滚动"), ("Esc", "返回")));
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
            if (burst.Count > 1 && LooksTyped(burst))
            {
                // Dropped files arrive as a burst of typing. Give a slow console a moment to deliver the rest of it.
                Thread.Sleep(20);
                while (Console.KeyAvailable)
                    burst.Add(Console.ReadKey(intercept: true));
            }

            _dirty = true;
            if (burst.Count >= PasteLength && LooksTyped(burst))
                HandlePaste(new string(burst.Where(k => k.KeyChar >= ' ').Select(k => k.KeyChar).ToArray()));
            else
                foreach (var key in burst)
                    HandleKey(key);
        }
    }

    /// <summary>
    /// Whether keys that came in together are text rather than one key held down, which repeats faster than the screen
    /// redraws and piles up the same way. Arrows and the like type no character at all.
    /// </summary>
    private static bool LooksTyped(List<ConsoleKeyInfo> burst) =>
        burst.All(k => k.KeyChar >= ' ') && burst.Any(k => k.KeyChar != burst[0].KeyChar);

    private void HandlePaste(string text)
    {
        if (_prompt is Prompt.EditOption or Prompt.Check)
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
            if (_view != View.Job)
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
            case Prompt.Path or Prompt.Check or Prompt.EditOption:
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
            case View.Options:
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
        if (snapshot.Activity is SessionActivity.Analyzing or SessionActivity.Reading)
            _session.Stop();
        else if (!snapshot.IsBusy)
            _prompt = Prompt.ConfirmQuit;
    }

    private void HandleTextKey(ConsoleKeyInfo key)
    {
        switch (key.Key)
        {
            case ConsoleKey.Enter:
                if (_prompt == Prompt.EditOption)
                {
                    var option = _options[_optionSelected];
                    string value = _input.Trim().Trim('"');
                    _prompt = Prompt.None;
                    _input = "";
                    ChangeOption(option, value);
                }
                else if (_prompt == Prompt.Check)
                {
                    ContinueCheck();
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
                _checking.Clear();
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

    /// <summary>Takes the NSP, then each folder to compare it with, which can be left empty, and checks it once it has all three.</summary>
    private void ContinueCheck()
    {
        string path = _input.Trim().Trim('"');
        _input = "";
        bool found = _checking.Count == 0 ? File.Exists(path) : path.Length == 0 || Directory.Exists(path);
        if (!found)
        {
            _prompt = Prompt.None;
            _checking.Clear();
            _message = path.Length > 0 ? $"Couldn't find: {path}" : null;
            return;
        }

        _checking.Add(path.Length > 0 ? path : null);
        if (_checking.Count < 3)
            return;
        _prompt = Prompt.None;
        if (_session.Verify(_checking[0]!, _checking[1], _checking[2]))
            _view = View.Job;
        _checking.Clear();
    }

    /// <summary>What was written for the title the pointer's on, or else by the last job, as the GUI's folder buttons show.</summary>
    private string? OutputToShow(SessionSnapshot s) =>
        (_selected < s.Rows.Length ? s.Rows[_selected].OutputPath : null) ?? s.LastOutput;

    private void HandleMenuKey(ConsoleKeyInfo key)
    {
        var items = MenuItems(_session.Snapshot());
        string chosen;
        switch (key.Key)
        {
            case ConsoleKey.UpArrow:
                _menuSelected = Math.Max(_menuSelected - 1, 0);
                return;
            case ConsoleKey.DownArrow:
                _menuSelected = Math.Min(_menuSelected + 1, items.Count - 1);
                return;
            case ConsoleKey.LeftArrow when _menuSelected >= _menuColumnRows:
                _menuSelected -= _menuColumnRows;
                return;
            case ConsoleKey.RightArrow when _menuSelected + _menuColumnRows < items.Count:
                _menuSelected += _menuColumnRows;
                return;
            case ConsoleKey.Escape or ConsoleKey.Backspace or ConsoleKey.M:
                _prompt = Prompt.None;
                return;
            case ConsoleKey.Enter or ConsoleKey.Spacebar:
                chosen = items[Math.Clamp(_menuSelected, 0, items.Count - 1)].key;
                break;
            default:
                chosen = key.Key.ToString();
                if (!items.Any(i => i.key == chosen))
                    return;
                break;
        }

        // The same as pressing its key on the list.
        _prompt = Prompt.None;
        var pressed = Enum.Parse<ConsoleKey>(chosen);
        HandleTitlesKey(new ConsoleKeyInfo(char.ToLowerInvariant(chosen[0]), pressed, false, false, false));
    }

    private void HandleTitlesKey(ConsoleKeyInfo key)
    {
        var snapshot = _session.Snapshot();
        _message = null;
        if (snapshot.IsBusy)
        {
            if (key.Key == ConsoleKey.Escape)
                _session.Stop();
            return;
        }

        switch (key.Key)
        {
            case ConsoleKey.UpArrow:
                _selected = Math.Max(_selected - 1, 0);
                break;
            case ConsoleKey.DownArrow:
                _selected = Math.Min(_selected + 1, Math.Max(snapshot.Rows.Length - 1, 0));
                break;
            case ConsoleKey.PageUp:
                _selected = Math.Max(_selected - 10, 0);
                break;
            case ConsoleKey.PageDown:
                _selected = Math.Min(_selected + 10, Math.Max(snapshot.Rows.Length - 1, 0));
                break;
            case ConsoleKey.Home:
                _selected = 0;
                break;
            case ConsoleKey.End:
                _selected = Math.Max(snapshot.Rows.Length - 1, 0);
                break;
            case ConsoleKey.C when snapshot.Rows.Length > 0:
                if (_session.TickedWithoutUpdate().Length > 0)
                    _prompt = Prompt.NoUpdate;
                else
                    StartCompressing();
                break;
            case ConsoleKey.E when snapshot.Rows.Length > 0:
                if (_session.Extract())
                    _view = View.Job;
                else
                    _message = snapshot.Rows.Any(r => r.Selected) ? "这些都已经解包过了。" : "没有勾选任何项目，按空格键勾选游戏。";
                break;
            case ConsoleKey.A when snapshot.Rows.Length > 0:
                if (!_session.Analyze())
                    _message = snapshot.Rows.Any(r => r.Selected) ? "这些都已有分析结果。" : "没有勾选任何项目，按空格键勾选游戏。";
                break;
            case ConsoleKey.Spacebar when _selected < snapshot.Rows.Length:
                _session.Toggle(snapshot.Rows[_selected]);
                break;
            case ConsoleKey.T when snapshot.Rows.Length > 0:
                _session.SelectAll(!snapshot.Rows.All(r => r.Selected));
                break;
            case ConsoleKey.D or ConsoleKey.Delete when _selected < snapshot.Rows.Length:
                _session.Remove(snapshot.Rows[_selected]);
                break;
            case ConsoleKey.D or ConsoleKey.Delete when snapshot.Issues.Length > 0:
                _session.Remove(snapshot.Issues[0]);
                break;
            case ConsoleKey.P:
                _prompt = Prompt.Path;
                _input = "";
                break;
            case ConsoleKey.S when snapshot.Rows.Length > 0:
                _session.SendToSwitch();
                break;
            case ConsoleKey.M:
                _prompt = Prompt.Menu;
                _menuSelected = _menuFirst = 0;
                break;
            case ConsoleKey.V:
                _prompt = Prompt.Check;
                _input = "";
                _checking.Clear();
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
                _helpScroll = 0;
                _view = View.Help;
                break;
            case ConsoleKey.Q:
                _quit = true;
                break;
            case ConsoleKey.Escape when snapshot.Rows.Length > 0:
                _prompt = Prompt.ConfirmQuit;
                break;
            case ConsoleKey.Escape:
                _quit = true;
                break;
        }
    }

    private void HandleJobKey(ConsoleKeyInfo key)
    {
        var snapshot = _session.Snapshot();
        if (snapshot.IsBusy)
        {
            if (key.Key == ConsoleKey.Escape)
                _prompt = Prompt.ConfirmStop;
            return;
        }
        if (key.Key is ConsoleKey.Enter or ConsoleKey.Escape or ConsoleKey.Backspace)
            _view = View.Titles;
        else if (key.Key == ConsoleKey.F && snapshot.LastOutput is not null)
            Explorer.Show(snapshot.LastOutput);
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
                if (option.Kind == OptionKind.Action)
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
                _view = View.Titles;
                break;
            case ConsoleKey.Q:
                _quit = true;
                break;
        }
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
            case ConsoleKey.Q:
                _quit = true;
                break;
        }
    }
}
