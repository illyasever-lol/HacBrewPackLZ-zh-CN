using System.Globalization;
using System.Runtime.InteropServices;
using System.Text;

namespace HacBrewPackLz;

/// <summary>Colours and text styles. Setting NO_COLOR in the environment turns them all off.</summary>
internal static class Sgr
{
    private static readonly bool Off = Environment.GetEnvironmentVariable("NO_COLOR") is { Length: > 0 };

    public static readonly string Reset = Make("0");
    public static readonly string Title = Make("1;38;5;80");
    public static readonly string Accent = Make("38;5;80");
    public static readonly string Strong = Make("1");
    public static readonly string Dim = Make("38;5;245");
    public static readonly string Good = Make("38;5;114");
    public static readonly string Warn = Make("38;5;179");
    public static readonly string Bad = Make("38;5;203");

    public static string For(UiLevel level) => level switch
    {
        UiLevel.Heading => Strong,
        UiLevel.Detail => Dim,
        UiLevel.Success => Good,
        UiLevel.Warning => Warn,
        UiLevel.Error => Bad,
        _ => "",
    };

    private static string Make(string code) => Off ? "" : $"\x1b[{code}m";
}

/// <summary>Measures and fits text by the cells it takes up in a console, not by its length in characters.</summary>
internal static class Terminal
{
    public const char BarFull = '█';
    public const char BarEmpty = '░';
    public const char Rule = '─';
    public const char Pointer = '►';
    public const char Ellipsis = '…';

    public static int Width => Measure(() => Console.WindowWidth, 80);
    public static int Height => Measure(() => Console.WindowHeight, 25);

    private static int Measure(Func<int> read, int fallback)
    {
        try
        {
            int value = read();
            return value > 0 ? value : fallback;
        }
        catch (Exception e) when (e is IOException or InvalidOperationException)
        {
            return fallback;
        }
    }

    /// <summary>Lets the console understand colours and cursor movement. Unix terminals always do.</summary>
    public static bool EnableVirtualTerminal()
    {
        if (!OperatingSystem.IsWindows())
            return true;
        const uint Processing = 0x0004;
        const int StdOutput = -11;
        var handle = GetStdHandle(StdOutput);
        return GetConsoleMode(handle, out uint mode) && ((mode & Processing) != 0 || SetConsoleMode(handle, mode | Processing));
    }

    /// <summary>Clicking in a console normally starts a selection that freezes the screen until a key is pressed.</summary>
    public static uint? DisableSelection()
    {
        if (!OperatingSystem.IsWindows())
            return null;
        const uint QuickEdit = 0x0040;
        const uint ExtendedFlags = 0x0080;
        const int StdInput = -10;
        var handle = GetStdHandle(StdInput);
        if (!GetConsoleMode(handle, out uint mode))
            return null;
        SetConsoleMode(handle, (mode & ~QuickEdit) | ExtendedFlags);
        return mode;
    }

    public static void RestoreInputMode(uint? mode)
    {
        const int StdInput = -10;
        if (mode is { } value && OperatingSystem.IsWindows())
            SetConsoleMode(GetStdHandle(StdInput), value);
    }

    /// <summary>A screen that always fits its window has nothing to scroll, so the console's scroll bars are just clutter.</summary>
    public static void ShowScrollBars(bool show)
    {
        const int Both = 3;
        if (OperatingSystem.IsWindows())
            ShowScrollBar(GetConsoleWindow(), Both, show);
    }

    public static int CellWidth(Rune rune)
    {
        int value = rune.Value;
        if (value < 0x300)
            return value is < 0x20 or (>= 0x7F and < 0xA0) ? 0 : 1;
        if (Rune.GetUnicodeCategory(rune) is UnicodeCategory.NonSpacingMark or UnicodeCategory.EnclosingMark or UnicodeCategory.Format)
            return 0;
        bool wide = value is (>= 0x1100 and <= 0x115F) or (>= 0x2E80 and <= 0xA4CF) or (>= 0xAC00 and <= 0xD7A3)
            or (>= 0xF900 and <= 0xFAFF) or (>= 0xFE30 and <= 0xFE6F) or (>= 0xFF00 and <= 0xFF60) or (>= 0xFFE0 and <= 0xFFE6)
            or (>= 0x1F300 and <= 0x1FAFF) or (>= 0x20000 and <= 0x3FFFD);
        return wide ? 2 : 1;
    }

    public static int DisplayWidth(string text)
    {
        int width = 0;
        foreach (var rune in text.EnumerateRunes())
            width += CellWidth(rune);
        return width;
    }

    /// <summary>Shortens text to fit, ending in an ellipsis if anything had to go.</summary>
    public static string Fit(string text, int width)
    {
        if (width <= 0)
            return "";
        if (DisplayWidth(text) <= width)
            return text;

        var fitted = new StringBuilder();
        int used = 0;
        foreach (var rune in text.EnumerateRunes())
        {
            int cells = CellWidth(rune);
            if (used + cells > width - 1)
                break;
            fitted.Append(rune.ToString());
            used += cells;
        }
        return fitted.Append(Ellipsis).ToString();
    }

    /// <summary>Breaks text at spaces and, for long paths, after separators. Rows after the first start at <paramref name="indent"/>.</summary>
    public static List<string> Wrap(string text, int width, int indent = 0)
    {
        var rows = new List<string>();
        width = Math.Max(width, indent + 8);
        string prefix = new(' ', indent);
        int start = 0;
        while (start < text.Length || rows.Count == 0)
        {
            int room = rows.Count == 0 ? width : width - indent;
            int end = start, used = 0, lastSpace = -1, lastSeparator = -1;
            while (end < text.Length)
            {
                Rune.DecodeFromUtf16(text.AsSpan(end), out var rune, out int consumed);
                int cells = CellWidth(rune);
                if (used + cells > room)
                    break;
                used += cells;
                end += consumed;
                if (rune.Value == ' ')
                    lastSpace = end;
                else if (rune.Value is '\\' or '/')
                    lastSeparator = end;
            }
            if (end < text.Length && lastSpace > start)
                end = lastSpace;
            else if (end < text.Length && lastSeparator > start)
                end = lastSeparator;

            rows.Add((rows.Count == 0 ? "" : prefix) + text[start..end].TrimEnd());
            start = end;
            while (start < text.Length && text[start] == ' ')
                start++;
            if (end == start && end >= text.Length)
                break;
        }
        return rows;
    }

    public static string Bar(int width, double fraction)
    {
        int full = (int)Math.Round(Math.Clamp(fraction, 0, 1) * width);
        return Sgr.Accent + new string(BarFull, full) + Sgr.Dim + new string(BarEmpty, width - full) + Sgr.Reset;
    }

    /// <summary>Roughly how long the rest of a job will take, or empty before there's anything to go on.</summary>
    public static string TimeLeft(ProgressSnapshot s)
    {
        if (s.BytesPerSecond <= 0 || s.Done <= 0 || s.Done >= s.Total)
            return "";
        double seconds = (s.Total - s.Done) / s.BytesPerSecond;
        return seconds switch
        {
            < 60 => "剩余不到一分钟",
            < 90 => "约剩一分钟",
            < 3600 => $"约剩 {Math.Round(seconds / 60)} 分钟",
            _ => $"约剩 {Math.Floor(seconds / 3600)} 小时 {Math.Round(seconds % 3600 / 60)} 分钟",
        };
    }

    [DllImport("kernel32.dll")]
    private static extern IntPtr GetConsoleWindow();

    [DllImport("user32.dll")]
    private static extern bool ShowScrollBar(IntPtr window, int bar, bool show);

    [DllImport("kernel32.dll")]
    private static extern IntPtr GetStdHandle(int handle);

    [DllImport("kernel32.dll")]
    private static extern bool GetConsoleMode(IntPtr handle, out uint mode);

    [DllImport("kernel32.dll")]
    private static extern bool SetConsoleMode(IntPtr handle, uint mode);
}

/// <summary>One row of the screen, kept to the width it was made for no matter what is added to it.</summary>
internal sealed class Line(int width)
{
    private readonly StringBuilder _text = new();

    public int Used { get; private set; }

    public Line Add(string text, string style = "")
    {
        string fitted = Terminal.Fit(text, width - Used);
        if (fitted.Length == 0)
            return this;
        _text.Append(style).Append(fitted);
        if (style.Length > 0)
            _text.Append(Sgr.Reset);
        Used += Terminal.DisplayWidth(fitted);
        return this;
    }

    /// <summary>Adds text that already holds its own styles and whose width is known.</summary>
    public Line AddStyled(string text, int cells)
    {
        if (Used + cells > width)
            return this;
        _text.Append(text);
        Used += cells;
        return this;
    }

    public Line To(int column)
    {
        int gap = Math.Min(column, width) - Used;
        if (gap > 0)
        {
            _text.Append(' ', gap);
            Used += gap;
        }
        return this;
    }

    /// <summary>Puts text against the right edge, if it fits beside what is already there.</summary>
    public Line Right(string text, string style = "", int rightMargin = 2)
    {
        int cells = Terminal.DisplayWidth(text);
        int column = width - rightMargin - cells;
        if (column <= Used)
            return this;
        To(column);
        return Add(text, style);
    }

    public override string ToString() => _text.ToString();
}

/// <summary>Colours and a progress bar for a command typed in a console, whose output stays in the scrollback.</summary>
internal sealed class ConsoleSink : IUiSink
{
    private readonly object _gate = new();
    private int _progressCells;

    public TimeSpan ProgressInterval => TimeSpan.FromMilliseconds(200);

    public void Message(UiLevel level, string text)
    {
        lock (_gate)
        {
            ClearProgress();
            var output = level == UiLevel.Error ? Console.Error : Console.Out;
            bool plain = level == UiLevel.Error && Console.IsErrorRedirected;
            string style = plain ? "" : Sgr.For(level);
            output.WriteLine(style.Length > 0 ? style + text + Sgr.Reset : text);
        }
    }

    public void Progress(ProgressSnapshot s)
    {
        lock (_gate)
        {
            int width = Terminal.Width;
            string line = Render(s, width - 1, out int cells);
            MoveToProgressStart(width);
            Console.Out.Write("\r\x1b[J" + line);
            _progressCells = cells;
        }
    }

    public void ProgressDone(string label, TimeSpan elapsed)
    {
        lock (_gate)
        {
            ClearProgress();
            if (elapsed.TotalSeconds >= 1)
                Console.Out.WriteLine($"{Sgr.Dim}{label}: 完成时间 {elapsed.TotalSeconds:0.0}s{Sgr.Reset}");
        }
    }

    private void ClearProgress()
    {
        if (_progressCells == 0)
            return;
        MoveToProgressStart(Terminal.Width);
        Console.Out.Write("\r\x1b[J");
        _progressCells = 0;
    }

    /// <summary>If the window got narrower the console has wrapped the line, and the rows above have to go as well.</summary>
    private void MoveToProgressStart(int width)
    {
        int rows = _progressCells > 0 ? (_progressCells - 1) / width : 0;
        if (rows > 0)
            Console.Out.Write($"\x1b[{rows}A");
    }

    /// <summary>Whatever doesn't fit goes first: the output size, then the speed, the sizes, and last of all the bar.</summary>
    private static string Render(ProgressSnapshot s, int width, out int cells)
    {
        const int BarMax = 28;
        const int BarMin = 8;

        double fraction = s.Total > 0 ? (double)s.Done / s.Total : 1;
        string label = Terminal.Fit(s.Label, Math.Max(width - 6, 0));
        string percent = $"{fraction * 100,4:0}%";
        var extras = new List<string>();
        if (s.ShowSizes)
            extras.Add($"{ProgressReporter.Format(s.Done)} / {ProgressReporter.Format(s.Total)}");
        if (s.ShowSizes && s.BytesPerSecond > 0)
            extras.Add($"{s.BytesPerSecond / 1048576:0} MB/s");
        if (Terminal.TimeLeft(s) is { Length: > 0 } left)
            extras.Add(left);
        if (s.ShowOutput && s.Done > 0)
            extras.Add($"输出 {s.Output * 100.0 / s.Done:0}%");

        int Fixed() => Terminal.DisplayWidth(label) + 1 + percent.Length + extras.Sum(e => e.Length + 2);
        while (extras.Count > 0 && Fixed() + BarMin + 1 > width)
            extras.RemoveAt(extras.Count - 1);

        int bar = Math.Min(width - Fixed() - 1, BarMax);
        var line = new Line(width);
        line.Add(label).Add(" ");
        if (bar >= BarMin)
            line.AddStyled(Terminal.Bar(bar, fraction), bar).Add(" ");
        line.Add(percent);
        foreach (string extra in extras)
            line.Add("  ").Add(extra, Sgr.Dim);
        cells = line.Used;
        return line.ToString();
    }
}
