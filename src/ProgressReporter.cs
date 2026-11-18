using System.Diagnostics;
using System.Text.RegularExpressions;

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
        ? $"{elapsed.TotalSeconds:0} 秒"
        : $"{elapsed.TotalMinutes:0.0} 分钟";

    /// <summary>Roughly how long the rest will take.</summary>
    public static string TimeLeft(double seconds)
    {
        if (seconds < 60)
            return "剩余不到一分钟";
        if (seconds < 90)
            return "约剩一分钟";
        long minutes = (long)Math.Round(seconds / 60);
        return minutes < 60 ? $"约剩余 {minutes} 分钟" : $"约剩余 {minutes / 60} 小时 {minutes % 60} 分钟";
    }

    /// <summary>What a step is called on screen: in words anyone knows, rather than the names of the parts of a game file, which the
    /// command line keeps.</summary>
    public static string Plain(string label) => label switch
    {
        "正在采样 RomFS" => "正在计算可节省的空间",
        "正在校验 RomFS" or "正在校验区段" or "正在计算 RomFS 哈希" => "正在校验游戏数据",
        "正在压缩 RomFS" => "正在压缩游戏数据",
        "正在将 RomFS 与源文件比对" => "正在与原始文件比对",
        "正在计算 NCA 哈希" => "正在校验新文件",
        _ when label.StartsWith("Writing ", StringComparison.Ordinal) && label.EndsWith(" NCA", StringComparison.Ordinal) => "正在写入新文件",
        _ when label.StartsWith("Extracting ", StringComparison.Ordinal) => "正在解包",
        _ => label,
    };

    /// <summary>Each number joined to the word after it, so a line that wraps never parts "85" from "MB/s".</summary>
    public static string KeepFiguresWhole(string text) => Regex.Replace(text, @"(\d) ", "$1\u00A0");

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
