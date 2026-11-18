namespace HacBrewPackLz;

/// <summary>
/// For tests/gui_shots.py: with HACBREWPACKLZ_GUI_LAYOUT set to a file, each layout of the main window writes where its
/// buttons are into it, so the test clicks them wherever the layout puts them. Nothing is written otherwise.
/// </summary>
internal static class TestLayout
{
    private static readonly string? FilePath = Environment.GetEnvironmentVariable("HACBREWPACKLZ_GUI_LAYOUT");

    public static void Write(AppWindow window, params (string name, Widget widget)[] widgets) => Write(window, [], widgets);

    /// <summary>The buttons shown, then other parts of the window by name, such as what the list says along its top.</summary>
    public static void Write(AppWindow window, IEnumerable<(string name, Rect bounds, string text)> parts, params (string name, Widget widget)[] widgets)
    {
        if (FilePath is null)
            return;
        var lines = widgets.Where(w => w.widget.Visible)
            .Select(w => $"{w.name} {w.widget.Bounds.X} {w.widget.Bounds.Y} {w.widget.Bounds.W} {w.widget.Bounds.H} {(w.widget as Button)?.Text}")
            .Concat(parts.Select(p => $"{p.name} {p.bounds.X} {p.bounds.Y} {p.bounds.W} {p.bounds.H} {p.text}"));
        try
        {
            File.WriteAllLines(FilePath, lines);
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
        }
    }
}
