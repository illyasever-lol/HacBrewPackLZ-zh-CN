using System.Diagnostics;

namespace HacBrewPackLz;

internal sealed class ProgressReporter(string label, long total, bool showOutputSize = false, bool showSizes = true)
{
    private readonly Stopwatch _clock = Stopwatch.StartNew();
    private long _lastReport = -1;

    public void Report(long done, long output)
    {
        Ui.Token.ThrowIfCancellationRequested();

        long now = _clock.ElapsedMilliseconds;
        if (_lastReport >= 0 && now - _lastReport < Ui.Sink.ProgressInterval.TotalMilliseconds)
            return;
        _lastReport = now;
        Ui.Sink.Progress(new ProgressSnapshot(label, done, total, output, showOutputSize, now > 0 ? done / (now / 1000.0) : 0, showSizes));
    }

    public void Done() => Ui.Sink.ProgressDone(label, _clock.Elapsed);

    public static string FormatElapsed(TimeSpan elapsed) => elapsed.TotalSeconds < 90
        ? $"{elapsed.TotalSeconds:0} s"
        : $"{elapsed.TotalMinutes:0.0} min";

    /// <summary>Sizes the way Windows Explorer shows them, so the numbers match what people see there.</summary>
    public static string Format(long bytes)
    {
        const double Ki = 1024;
        string sign = bytes < 0 ? "-" : "";
        long value = Math.Abs(bytes);
        return sign + value switch
        {
            >= 1L << 30 => $"{value / (Ki * Ki * Ki):0.00} GB",
            >= 1L << 20 => $"{value / (Ki * Ki):0.0} MB",
            >= 1L << 10 => $"{value / Ki:0.0} KB",
            _ => $"{value} B",
        };
    }
}
