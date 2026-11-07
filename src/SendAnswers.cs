using System.Collections.Concurrent;
using System.Globalization;
using System.Security.Cryptography;
using System.Text;

namespace HacBrewPackLz;

/// <summary>What the Switch would otherwise have to work out by reading files across the network: what's in a game file, with its name and icon,
/// and what a game would come to compressed. The PC reads its own disk far faster, so it works these out for it. They can take a while, an NSZ
/// above all, so a question gets "正在处理" straight away and is asked again, which leaves the connection free for reading files meanwhile.
/// Answers are kept for as long as sharing goes on.</summary>
internal sealed class SendAnswers(Func<string, string?> resolve, Func<Keyset> keys)
{
    private const int Workers = 2;

    private readonly ConcurrentDictionary<string, Answer> _answers = new();
    private readonly BlockingCollection<(string key, Func<Answer> work)> _queue = [];
    private Keyset? _keyset;
    private readonly object _keyGate = new();
    private bool _started;

    public sealed record Answer(bool Ready, bool Done, byte[] Data);

    private static readonly Answer Working = new(false, false, []);
    private static readonly Answer Failed = new(true, false, []);

    /// <summary>What's in a game file or an extracted game's folder.</summary>
    public Answer Describe(string inside) => Ask("describe|" + inside, () =>
    {
        string? path = resolve(inside);
        if (path is null)
            return Failed;
        return Directory.Exists(path) ? DescribeFolder(path) : DescribeFile(path);
    });

    /// <summary>What a game comes to compressed. The request is "level, block size, compress, game" on its first line, tab separated, then each
    /// of the game's files on a line of its own, as the Switch has put them together.</summary>
    public Answer Estimate(string request) => Ask("estimate|" + request, () =>
    {
        var lines = request.Split('\n', StringSplitOptions.RemoveEmptyEntries);
        if (lines.Length < 2)
            return Failed;
        var settings = lines[0].Split('\t');
        if (settings.Length < 4 || !int.TryParse(settings[0], out int level) || !int.TryParse(settings[1], out int blockSize)
            || !ulong.TryParse(settings[3], NumberStyles.HexNumber, CultureInfo.InvariantCulture, out ulong application))
            return Failed;
        // The same as the command line takes. A block size of 0 would never get to the end.
        if (level is < Lz4Encoder.MinLevel or > Lz4Encoder.MaxLevel || blockSize is < 0x1000 or > RomFsCompressor.MaxPhysicalBlockSize || (blockSize & (blockSize - 1)) != 0)
            return Failed;
        var options = new Options { Compress = settings[2] == "1", Level = level, BlockSize = blockSize };
        var paths = new List<string>();
        foreach (string inside in lines.Skip(1))
        {
            if (resolve(inside) is not { } path)
                return Failed;
            paths.Add(path);
        }

        SizeEstimate estimate;
        if (paths.Count == 1 && Directory.Exists(paths[0]))
        {
            estimate = Analyzer.EstimateBuild(GameFolder.Open(paths[0]).BuildOptions(options));
        }
        else
        {
            using var set = TitleSet.Load(paths, Keyset());
            var jobs = set.Jobs.Where(j => j.ApplicationId == application).ToList();
            if (jobs.Count == 0)
                return Failed;
            // The same as on the Switch: DLC with no game data to it, or nothing to gain, says nothing either way, and margins add up as a sum of squares.
            var each = jobs.Select(job => Analyzer.Estimate(job, options)).ToList();
            bool compressed = each.Any(e => e.AlreadyCompressed) && each.All(e => e.AlreadyCompressed || e.RomFsBytes == 0 || e.SavedFraction < SavingRule.NothingToGain);
            estimate = new SizeEstimate(each.Sum(e => e.InputBytes), each.Sum(e => e.OutputBytes), (long)Math.Sqrt(each.Sum(e => (double)e.MarginBytes * e.MarginBytes)),
                each.Sum(e => e.RomFsBytes), each.Sum(e => e.RomFsStoredBytes)) { AlreadyCompressed = compressed };
        }
        string text = string.Create(CultureInfo.InvariantCulture,
            $"{estimate.InputBytes} {estimate.OutputBytes} {estimate.MarginBytes} {estimate.RomFsBytes} {estimate.RomFsStoredBytes} {(estimate.AlreadyCompressed ? 1 : 0)}");
        return new Answer(true, true, Encoding.UTF8.GetBytes(text));
    });

    private Answer Ask(string key, Func<Answer> work)
    {
        if (_answers.TryGetValue(key, out var known))
            return known;
        if (_answers.TryAdd(key, Working))
        {
            StartWorkers();
            _queue.Add((key, work));
        }
        return _answers.GetValueOrDefault(key, Working);
    }

    private void StartWorkers()
    {
        lock (_keyGate)
        {
            if (_started)
                return;
            _started = true;
        }
        for (int i = 0; i < Workers; i++)
        {
            new Thread(() =>
            {
                foreach (var (key, work) in _queue.GetConsumingEnumerable())
                {
                    Answer answer;
                    try
                    {
                        answer = work();
                    }
                    catch (Exception e) when (e is ToolException or IOException or UnauthorizedAccessException or CryptographicException or InvalidDataException)
                    {
                        // The Switch then has a go itself, and says what's wrong if it's the same there.
                        answer = Failed;
                    }
                    _answers[key] = answer;
                }
            }) { IsBackground = true, Name = "正在应答" }.Start();
        }
    }

    private Keyset Keyset()
    {
        lock (_keyGate)
            return _keyset ??= keys();
    }

    private Answer DescribeFile(string path)
    {
        using var set = TitleSet.OpenOnly([path], Keyset());
        var text = new StringBuilder();
        var icons = new List<byte[]>();
        if (set.Problems.Count > 0)
            text.Append($"P\t{OneLine(set.Problems[0].Message)}\n");
        foreach (var title in set.Summarize())
        {
            byte[] icon = title.Icon ?? [];
            icons.Add(icon);
            text.Append(CultureInfo.InvariantCulture,
                $"T\t{title.TitleId:x16}\t{title.Type}\t{title.Version}\t{title.LinkedId:x16}\t{title.Bytes}\t{(title.HoldsOnlyPart ? 1 : 0)}\t{icon.Length}\t{OneLine(title.DisplayVersion ?? "")}\t{OneLine(title.Name ?? "")}\n");
        }
        return Together(text, icons);
    }

    private static Answer DescribeFolder(string path)
    {
        var folder = GameFolder.Open(path);
        byte[] icon = folder.Icon ?? [];
        var text = new StringBuilder();
        text.Append(CultureInfo.InvariantCulture,
            $"T\t{folder.TitleId:x16}\t{Cnmt.MetaApplication}\t{folder.Version}\t{0:x16}\t{folder.Bytes}\t0\t{icon.Length}\t\t{OneLine(folder.Name)}\n");
        return Together(text, [icon]);
    }

    /// <summary>The lines, a blank line, and then the icons one after another, as long as the lines say each is.</summary>
    private static Answer Together(StringBuilder text, List<byte[]> icons)
    {
        text.Append('\n');
        var data = new List<byte>(Encoding.UTF8.GetBytes(text.ToString()));
        foreach (var icon in icons)
            data.AddRange(icon);
        return new Answer(true, true, data.ToArray());
    }

    private static string OneLine(string text) => text.Replace('\t', ' ').Replace('\n', ' ').Replace('\r', ' ');
}
