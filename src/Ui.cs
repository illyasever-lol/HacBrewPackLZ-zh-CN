namespace HacBrewPackLz;

internal static class AppInfo
{
    public const string Name = "hacBrewPackLZ";
    public const string Version = "1.4.3";
}

internal enum UiLevel
{
    Info,
    Heading,
    Detail,
    Success,
    Warning,
    Error,
}

/// <param name="ShowSizes">False when Done and Total only measure how far along it is, and aren't sizes worth showing.</param>
internal readonly record struct ProgressSnapshot(string Label, long Done, long Total, long Output, bool ShowOutput, double BytesPerSecond, bool ShowSizes = true);

/// <summary>Where the engine's messages and progress go: plain text, the console screen or the GUI.</summary>
internal interface IUiSink
{
    TimeSpan ProgressInterval { get; }
    void Message(UiLevel level, string text);
    void Progress(ProgressSnapshot snapshot);
    void ProgressDone(string label, TimeSpan elapsed);
}

internal static class Ui
{
    public static IUiSink Sink { get; set; } = new PlainSink();

    /// <summary>Stops long running work. It is checked wherever progress is reported.</summary>
    public static CancellationToken Token { get; set; }

    public static void Info(string text) => Sink.Message(UiLevel.Info, text);
    public static void Heading(string text) => Sink.Message(UiLevel.Heading, text);
    public static void Detail(string text) => Sink.Message(UiLevel.Detail, text);
    public static void Success(string text) => Sink.Message(UiLevel.Success, text);
    public static void Warn(string text) => Sink.Message(UiLevel.Warning, text);
    public static void Error(string text) => Sink.Message(UiLevel.Error, text);
}

/// <summary>Plain lines, for when output is redirected to a file or another program.</summary>
internal sealed class PlainSink : IUiSink
{
    public TimeSpan ProgressInterval => TimeSpan.FromSeconds(10);

    public void Message(UiLevel level, string text)
    {
        if (level == UiLevel.Error)
            Console.Error.WriteLine(text);
        else if (level == UiLevel.Warning)
            Console.WriteLine($"Warning: {text}");
        else
            Console.WriteLine(text);
    }

    public void Progress(ProgressSnapshot s)
    {
        double percent = s.Total > 0 ? s.Done * 100.0 / s.Total : 100;
        string line = $"{s.Label}: {percent,5:0.0}%";
        if (s.ShowSizes)
            line += $"  {ProgressReporter.Format(s.Done)} / {ProgressReporter.Format(s.Total)}  {s.BytesPerSecond / 1048576:0} MB/s";
        if (s.ShowOutput && s.Done > 0)
            line += $"  output {ProgressReporter.Format(s.Output)} ({s.Output * 100.0 / s.Done:0.0}%)";
        Console.WriteLine(line);
    }

    public void ProgressDone(string label, TimeSpan elapsed) =>
        Console.WriteLine($"{label}: 完成时间 {elapsed.TotalSeconds:0.0}s");
}
