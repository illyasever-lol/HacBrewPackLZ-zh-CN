using System.Diagnostics;
using System.Security.Cryptography;

namespace HacBrewPackLz;

internal sealed record SizeEstimate(long InputBytes, long OutputBytes, long MarginBytes, long RomFsBytes, long RomFsStoredBytes)
{
    /// <summary>All its game data is already stored the way compressing would store it, so there's nothing to work out.</summary>
    public bool AlreadyCompressed { get; init; }

    public long SavedBytes => InputBytes - OutputBytes;
    public double SavedFraction => InputBytes > 0 ? (double)SavedBytes / InputBytes : 0;
    public double RomFsFraction => RomFsBytes > 0 ? (double)RomFsStoredBytes / RomFsBytes : 1;
}

/// <summary>Whether a title is worth the time it takes to compress, for people after the most space in the least time.</summary>
internal static class SavingRule
{
    /// <summary>Whether the minimum has a say at all. If not, there's no need to analyze anything first.</summary>
    public static bool Applies(Options options) => options.Compress && options.MinSavingPercent > 0;

    /// <summary>Saving less than this is nothing, whatever the minimum.</summary>
    public const double NothingToGain = 0.005;

    public static bool WorthCompressing(SizeEstimate estimate, Options options)
    {
        if (options.Compress && (estimate.AlreadyCompressed || estimate.SavedFraction < NothingToGain))
            return false;
        return !Applies(options) || estimate.SavedFraction * 100 >= options.MinSavingPercent;
    }

    public static string Reason(SizeEstimate estimate, Options options)
    {
        if (estimate.AlreadyCompressed)
            return "已压缩";
        if (estimate.SavedFraction < NothingToGain && estimate.SavedBytes >= 0)
            return "没有压缩空间：已经是最小的了";
        return estimate.SavedBytes >= 0
            ? $"saves only {estimate.SavedFraction * 100:0}%, below the {options.MinSavingPercent}% minimum"
            : $"would grow by {-estimate.SavedFraction * 100:0}%, below the {options.MinSavingPercent}% minimum";
    }
}

/// <summary>Refuses before anything gets written, rather than running out of room part way through.</summary>
internal static class RoomCheck
{
    /// <summary>Space on top of the data itself that's kept free while working.</summary>
    private const long RoomToSpare = 64L << 20;

    /// <summary>An NSP can't be split, so this is the end of it on a disk that can't hold 4 GB in one file.</summary>
    private const long Fat32Limit = 0xFFFFFFFFL;

    public static void ForTitle(TitleJob job, Options options, string folder)
    {
        // Compressed, it can't come to much more than what it's made from before any compression, so with room for that there's no need to work it out.
        long free = FreeSpace(folder, out bool fat32);
        long most = job.All.Sum(t => t.Main.Meta.Records.Sum(r => r.Size) + (t.Update?.Meta.Records.Sum(r => r.Size) ?? 0));
        most += most / 8;
        if (options.Compress && free >= most + RoomToSpare && (!fat32 || most < Fat32Limit))
            return;
        var estimate = Analyzer.Estimate(job, options);
        Require(estimate.OutputBytes + estimate.MarginBytes, free, fat32, folder);
    }

    public static void ForBuild(Options options, string folder)
    {
        long free = FreeSpace(folder, out bool fat32);
        var folders = new[] { options.ExefsDir, options.NoRomfs ? null : options.RomfsDir, options.NoLogo ? null : options.LogoDir, options.ControlDir, options.HtmlDocDir, options.LegalInfoDir };
        long most = folders.Where(Directory.Exists).Sum(d => Directory.EnumerateFiles(d!, "*", SearchOption.AllDirectories).Sum(f => new FileInfo(f).Length));
        most += most / 8 + (1L << 20);
        if (free >= most + RoomToSpare && (!fat32 || most < Fat32Limit))
            return;
        var estimate = Analyzer.EstimateBuild(options);
        Require(estimate.OutputBytes + estimate.MarginBytes, free, fat32, folder);
    }

    /// <summary>What's extracted is at least as big as what it came from, and can be bigger if it was compressed.</summary>
    public static void ForExtract(TitleJob job, string folder)
    {
        long free = FreeSpace(folder, out _);
        if (free >= 0 && free < job.InputBytes + RoomToSpare)
            throw new ToolException($"It needs at least {ProgressReporter.Format(job.InputBytes + RoomToSpare)} free in '{folder}', and there's {ProgressReporter.Format(free)}");
    }

    private static void Require(long output, long free, bool fat32, string folder)
    {
        if (fat32 && output >= Fat32Limit)
            throw new ToolException($"This comes to {ProgressReporter.Format(output)}, and the disk '{folder}' is on is FAT32, which can't hold a file of 4 GB or more");
        if (free >= 0 && free < output + RoomToSpare)
            throw new ToolException($"It needs about {ProgressReporter.Format(output + RoomToSpare)} free in '{folder}', and there's {ProgressReporter.Format(free)}");
    }

    /// <summary>Negative if there's no telling.</summary>
    private static long FreeSpace(string folder, out bool fat32)
    {
        fat32 = false;
        try
        {
            var drive = new DriveInfo(Path.GetFullPath(folder));
            fat32 = drive.DriveFormat.StartsWith("FAT", StringComparison.OrdinalIgnoreCase) && !drive.DriveFormat.Equals("exFAT", StringComparison.OrdinalIgnoreCase);
            return drive.AvailableFreeSpace;
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or ArgumentException)
        {
            return -1;
        }
    }
}

/// <summary>
/// Predicts a conversion without doing it. Everything but the compressed data is worked out exactly from the headers;
/// for the compressed data, a spread of regions from across the RomFS is really compressed, with the same encoder, level
/// and block size the conversion would use. Hashes aren't checked, so only the sampled regions are read.
/// </summary>
internal static class Analyzer
{
    private const int RegionSize = 0x10000;

    /// <summary>Enough to time reading with, one thread at a time and then all of them at once.</summary>
    private const int TrialRegions = 24;
    private const int BatchRegions = 128;
    private const int MinRegions = 128;

    /// <summary>However small an image is, this much of it gets a look. Anything smaller just gets read.</summary>
    private const int SmallestSample = 32;
    private const int MaxRegions = 8192;
    private const double TimeBudgetSeconds = 1;
    private const double GoodEnoughMargin = 0.02;

    /// <summary>It has to get this much more read before it's worth having every thread read at once.</summary>
    private const double TogetherHasToBeat = 1.2;

    /// <summary>A read that takes this long means a hard drive hunting for it, and asking for more at once would only have it hunt more.</summary>
    private const double SeekingSeconds = 0.004;
    private const double GoldenRatioConjugate = 0.6180339887498949;
    private const double Confidence95 = 1.96;

    private sealed class Tally
    {
        public long RomFs;
        public long Stored;
        public double Variance;

        public SizeEstimate ToEstimate(long input, long output) =>
            new(input, output, (long)Math.Ceiling(Math.Sqrt(Variance)), RomFs, Stored);
    }

    public static SizeEstimate Estimate(TitleJob job, Options options)
    {
        if (IsAlreadyCompressed(job, options))
            return new SizeEstimate(job.InputBytes, job.InputBytes, 0, 0, 0) { AlreadyCompressed = true };
        var tally = new Tally();
        var titles = job.All.ToList();
        long output = NspWriter.HeaderSize(titles.Sum(t => t.Contents.Count), titles.Count);
        try
        {
            foreach (var title in titles)
            {
                foreach (var content in title.Contents)
                {
                    long nca = NcaWriter.HeaderRegionSize;
                    foreach (int i in content.SectionsInFileOrder())
                        nca += NcaWriter.AlignToSector(SectionSize(content, content.SectionAt(i)!.Value, options, tally));
                    output += nca;
                }
            }
        }
        finally
        {
            job.ReleaseMemory();
        }

        foreach (var title in titles)
        {
            var meta = title.Main.Meta;
            output += MetaNcaSize(Cnmt.FileName(meta.TitleId, meta.Type), meta.RebuiltSize(title.Contents.Count));
        }
        return tally.ToEstimate(job.InputBytes, output);
    }

    /// <summary>Whether every part of it compressing would compress is compressed already. An update always makes something new. A game
    /// with nothing compressing would touch, such as one that keeps all its data in its program, counts too: it's as small as it gets, and
    /// otherwise its compressed copy would come back on the list each time it's analyzed.</summary>
    public static bool IsAlreadyCompressed(TitleJob job, Options options)
    {
        if (!options.Compress || job.Update is not null)
            return false;
        int contents = 0;
        foreach (var title in job.All)
        {
            foreach (var content in title.Contents)
            {
                contents++;
                foreach (int i in content.SectionsInFileOrder())
                {
                    var source = content.SectionAt(i)!.Value;
                    if (TitleConverter.Plan(content, source, options) != SectionPlan.CompressRomFs)
                        continue;
                    if (!source.Section.HasCompression)
                        return false;
                }
            }
        }
        return contents > 0;
    }

    /// <summary>The same for building from folders. Nothing here depends on the keys.</summary>
    public static SizeEstimate EstimateBuild(Options options)
    {
        var npdm = Npdm.Load(Path.Combine(options.ExefsDir, "main.npdm"), options.TitleId, null, out ulong titleId);
        var nacp = Nacp.Load(Path.Combine(options.ControlDir, "control.nacp"), options.TitleId, options.TitleName,
            options.TitlePublisher, !options.NoPatchNacpLogo);
        Builder.CheckDirectories(options);

        var tally = new Tally();
        var replacedNpdm = new Dictionary<string, byte[]> { ["main.npdm"] = npdm };
        long program = NcaWriter.HeaderRegionSize
            + NcaWriter.AlignToSector(NcaSections.Pfs0SectionSize(Builder.BuildPfs0(options.ExefsDir, replacedNpdm).Length, NcaSections.ExefsHashBlockSize));
        if (!options.NoLogo)
            program += NcaWriter.AlignToSector(NcaSections.Pfs0SectionSize(Builder.BuildPfs0(options.LogoDir, null).Length, NcaSections.LogoHashBlockSize));
        if (!options.NoRomfs)
        {
            var image = RomFsBuilder.Build(options.RomfsDir);
            program += NcaWriter.AlignToSector(options.Compress ? CompressedRomFsSize(image, options, tally) : PlainRomFsSize(image.Length, tally));
        }

        var folders = new List<string>();
        if (!options.NoRomfs)
            folders.Add(options.RomfsDir);
        folders.Add(options.ExefsDir);
        if (!options.NoLogo)
            folders.Add(options.LogoDir);
        folders.Add(options.ControlDir);

        long output = program + RomFsNcaSize(RomFsBuilder.Build(options.ControlDir, new Dictionary<string, byte[]> { ["control.nacp"] = nacp }).Length);
        int contents = 2;
        foreach (string? folder in new[] { options.HtmlDocDir, options.LegalInfoDir })
        {
            if (folder is null)
                continue;
            output += RomFsNcaSize(RomFsBuilder.Build(folder).Length);
            folders.Add(folder);
            contents++;
        }

        output += NspWriter.HeaderSize(contents) + MetaNcaSize(Cnmt.FileName(titleId), Cnmt.BuiltSize(contents));
        long input = folders.Sum(f => new DirectoryInfo(f).EnumerateFiles("*", SearchOption.AllDirectories).Sum(file => file.Length));
        return tally.ToEstimate(input, output);
    }

    /// <summary>Prints an estimate of every title and returns the exit code.</summary>
    public static int Report(TitleSet set, Options options)
    {
        long input = 0, output = 0;
        double variance = 0;
        int failures = 0;
        var ranking = new List<(TitleJob job, SizeEstimate estimate)>();
        foreach (var job in set.Jobs)
        {
            Ui.Info("");
            Ui.Heading(job.Describe());
            try
            {
                var estimate = Estimate(job, options);
                Print(estimate, options);
                if (!SavingRule.WorthCompressing(estimate, options))
                    Ui.Warn($"  Left out:   {SavingRule.Reason(estimate, options)}");
                ranking.Add((job, estimate));
                input += estimate.InputBytes;
                output += estimate.OutputBytes;
                variance += (double)estimate.MarginBytes * estimate.MarginBytes;
            }
            catch (Exception e) when (e is ToolException or IOException or UnauthorizedAccessException or CryptographicException)
            {
                failures++;
                Ui.Error($"FAILED: {e.Message}");
            }
        }

        if (set.Jobs.Count > 1 && failures < set.Jobs.Count)
        {
            Ui.Info("");
            Ui.Heading("全部游戏");
            Print(new SizeEstimate(input, output, (long)Math.Ceiling(Math.Sqrt(variance)), 0, 0), options);
            PrintRanking(ranking, options);
        }
        PrintClosingNote();
        foreach (var problem in set.Problems)
            Ui.Warn($"Not processed: {problem.Message}");
        return failures > 0 || set.Problems.Count > 0 ? 1 : 0;
    }

    public static int ReportBuild(Options options)
    {
        Ui.Heading("从文件夹构建");
        Print(EstimateBuild(options), options);
        PrintClosingNote();
        return 0;
    }

    /// <summary>The most worthwhile first, with what to type to go ahead with only those.</summary>
    private static void PrintRanking(List<(TitleJob job, SizeEstimate estimate)> ranking, Options options)
    {
        Ui.Info("");
        Ui.Heading("节省最多的排前面");
        foreach (var (job, estimate) in ranking.OrderByDescending(r => r.estimate.SavedFraction).ThenByDescending(r => r.estimate.SavedBytes))
        {
            string saved = estimate.SavedBytes >= 0 ? ProgressReporter.Format(estimate.SavedBytes) : "-" + ProgressReporter.Format(-estimate.SavedBytes);
            string percent = estimate.SavedBytes >= 0 ? $"{estimate.SavedFraction * 100:0}%" : $"+{-estimate.SavedFraction * 100:0}%";
            string line = $"  {percent,5}  {saved,10}  {job.DisplayName} [{job.Main.Meta.TitleId:X16}]";
            if (SavingRule.WorthCompressing(estimate, options))
                Ui.Info(line);
            else
                Ui.Detail(line + "，已排除");
        }

        var worth = ranking.Where(r => SavingRule.WorthCompressing(r.estimate, options) && r.estimate.SavedBytes > 0).OrderByDescending(r => r.estimate.SavedFraction).ToList();
        if (worth.Count > 0 && worth.Count < ranking.Count)
        {
            Ui.Info("");
            Ui.Info($"To compress only what's worth it: convert --only {string.Join(',', worth.Select(r => $"{r.job.Main.Meta.TitleId:X16}"))}");
        }
    }

    private static void PrintClosingNote()
    {
        Ui.Info("");
        Ui.Detail("这是根据游戏数据采样得出的估算，未写入任何文件。");
    }

    private static void Print(SizeEstimate e, Options options)
    {
        string margin = e.MarginBytes > 0 ? $"（误差约 {ProgressReporter.Format(e.MarginBytes)}）" : "";
        string prefix = e.MarginBytes > 0 ? "约" : "";
        Ui.Info($"  当前:       {ProgressReporter.Format(e.InputBytes)}");
        Ui.Info($"  压缩后:     {prefix}{ProgressReporter.Format(e.OutputBytes)}{margin}");
        Ui.Detail($"  字节数:     {e.OutputBytes} +/- {e.MarginBytes}");
        Ui.Info(e.SavedBytes >= 0
            ? $"  节省:       {ProgressReporter.Format(e.SavedBytes)} ({e.SavedFraction * 100:0}%)"
            : $"  增大:       {ProgressReporter.Format(-e.SavedBytes)} ({-e.SavedFraction * 100:0}%)");
        if (e.RomFsBytes > 0 && options.Compress)
            Ui.Info($"  Game data:  {ProgressReporter.Format(e.RomFsBytes)} compresses to {ProgressReporter.Format(e.RomFsStoredBytes)} ({e.RomFsFraction * 100:0}%)");
    }

    private static long SectionSize(ContentPair content, SectionSource source, Options options, Tally tally)
    {
        var (owner, section, original) = source;
        switch (TitleConverter.Plan(content, source, options))
        {
            case SectionPlan.CompressRomFs:
                bool inOrder = owner.ReadsInOrder || original?.ReadsInOrder == true;
                return CompressedRomFsSize(owner.OpenRomFsImage(section, original, verify: false), options, tally, inOrder);
            case SectionPlan.RebuildRomFs:
                return PlainRomFsSize(owner.OpenRomFsImage(section, original, verify: false).Length, tally);
            case SectionPlan.RebuildPfs0:
                var archive = owner.OpenPfs0(section, original, verify: false);
                var files = archive.Files.Select(f => (f.Name, (DataSource)new ZeroSource(f.Size)));
                return NcaSections.Pfs0SectionSize(Pfs0Builder.Build(files).Length, archive.HashBlockSize);
            default:
                return section.Size;
        }
    }

    private static long MetaNcaSize(string cnmtName, long cnmtSize)
    {
        var pfs0 = Pfs0Builder.Build([(cnmtName, new ZeroSource(cnmtSize))]);
        return NcaWriter.HeaderRegionSize + NcaWriter.AlignToSector(NcaSections.Pfs0SectionSize(pfs0.Length, NcaSections.MetaHashBlockSize));
    }

    private static long RomFsNcaSize(long image) =>
        NcaWriter.HeaderRegionSize + NcaWriter.AlignToSector(NcaSections.RomFsSectionSize(image));

    private static long PlainRomFsSize(long image, Tally tally)
    {
        tally.RomFs += image;
        tally.Stored += image;
        return NcaSections.RomFsSectionSize(image);
    }

    private static long CompressedRomFsSize(DataSource image, Options options, Tally tally, bool inOrder = false)
    {
        var sample = Sample(image, options, inOrder);
        tally.RomFs += image.Length;
        tally.Stored += sample.Stored;
        tally.Variance += sample.Margin * sample.Margin;

        // Raw blocks in a row share one table entry, so there can't be more raw entries than gaps between compressed blocks.
        int entries = (int)Math.Min(sample.Lz4Blocks + Math.Min(sample.RawBlocks, sample.Lz4Blocks + 1), int.MaxValue);
        return NcaSections.CompressedRomFsSectionSize(sample.Stored, Math.Max(entries, 1));
    }

    private readonly record struct SampleResult(long Stored, double Margin, long Lz4Blocks, long RawBlocks);

    private readonly record struct RegionResult(long Region, int Length, int Stored, int Lz4Blocks, int RawBlocks);

    private enum ReadingTrial
    {
        OneAtATime,
        Together,
        Decided,
    }

    [ThreadStatic] private static byte[]? t_region;

    /// <summary>
    /// Walks the image with a golden ratio stride, which stays evenly spread however soon it stops and reaches every
    /// region exactly once. A slow disk gets the minimum sample, and a fast one a far larger sample in about the same time.
    /// A solid NCZ can only be read from front to back, so that takes a single sweep, of however long the whole file takes.
    ///
    /// Every thread keeps going between reading and compressing, so the processor is never waiting for the disk or the
    /// other way round. Reads go in order, so a hard drive sweeps across rather than jumping back and forth, and one at a
    /// time, unless timing them shows that reading several at once gets more read, which it does on what has no seek time
    /// to lose. Going by the time it takes means it doesn't matter what a drive says it is, or what an adapter says on its
    /// behalf.
    ///
    /// What's kept to the minimum on a slow disk is how many times it has to go looking. Reading more at each place doesn't
    /// make up for that, as regions next to each other say much the same: on a hard drive, four in a row each time took as
    /// long as one, and narrowed the margin not at all.
    /// </summary>
    private static SampleResult Sample(DataSource image, Options options, bool inOrder)
    {
        long regions = (image.Length + RegionSize - 1) / RegionSize;
        if (regions == 0)
            return default;

        long stride = (long)(regions * GoldenRatioConjugate);
        while (GreatestCommonDivisor(stride, regions) != 1)
            stride++;

        // A small image takes up less of the answer, and doesn't need as close a look.
        int minimum = (int)Math.Min(MinRegions, Math.Max(regions / 16, SmallestSample));
        double budget = TimeBudgetSeconds * Math.Min(1.0, image.Length / (double)(1L << 30));

        long reportedTotal = minimum * (long)RegionSize;
        var progress = new ProgressReporter("正在采样 RomFS", reportedTotal, showSizes: false);
        var clock = Stopwatch.StartNew();
        var samples = new List<RegionResult>();
        long position = image.Length.GetHashCode() & int.MaxValue;
        var reporting = new object();
        SampleResult result;

        // Done is whichever is further off: reaching the minimum sample, or the end of the time allowed.
        void Report(int regionsRead)
        {
            double done = Math.Min(Math.Min((double)regionsRead / minimum, budget > 0 ? clock.Elapsed.TotalSeconds / budget : 1), 1);
            lock (reporting)
                progress.Report((long)(done * reportedTotal), 0);
        }

        // A go at each way of reading, and then whichever got more done.
        bool canTryTogether = options.Threads > 1 && image.ReadsAtOnce;
        var trial = !inOrder && regions >= TrialRegions * 4 ? ReadingTrial.OneAtATime : ReadingTrial.Decided;
        bool together = false;
        double oneAtATime = 0;

        Report(0);
        if (inOrder)
            NczSource.BeginSinglePass();
        try
        {
            do
            {
                int wanted = inOrder ? minimum : trial != ReadingTrial.Decided ? TrialRegions : Math.Max(BatchRegions, minimum - samples.Count);
                int count = (int)Math.Min(wanted, regions - samples.Count);
                var chosen = new long[count];
                for (int i = 0; i < count; i++)
                {
                    chosen[i] = position % regions;
                    position = chosen[i] + stride;
                }
                Array.Sort(chosen);

                if (inOrder)
                {
                    ReadThenCompress(image, options, chosen, samples, Report);
                }
                else
                {
                    double perRegion = ReadWhileCompressing(image, options, chosen, together || trial == ReadingTrial.Together, samples, Report);
                    if (trial == ReadingTrial.OneAtATime)
                    {
                        oneAtATime = perRegion;
                        trial = canTryTogether && perRegion < SeekingSeconds ? ReadingTrial.Together : ReadingTrial.Decided;
                    }
                    else if (trial == ReadingTrial.Together)
                    {
                        together = options.Threads / perRegion > TogetherHasToBeat / oneAtATime;
                        trial = ReadingTrial.Decided;
                    }
                }
                result = Summarize(samples, image.Length, regions);
            }
            while (!inOrder && samples.Count < Math.Min(regions, MaxRegions)
                && (samples.Count < minimum || (clock.Elapsed.TotalSeconds < budget && result.Margin > result.Stored * GoodEnoughMargin)));
        }
        finally
        {
            NczSource.EndSinglePass();
        }
        image.Release();
        progress.Done();
        return result;
    }

    /// <summary>Every thread takes the next region, reads it and compresses it. Returns the time a read took on average.</summary>
    private static double ReadWhileCompressing(DataSource image, Options options, long[] chosen, bool together, List<RegionResult> samples, Action<int> report)
    {
        int count = chosen.Length;
        var results = new RegionResult[count];
        var gate = new object();
        int next = -1;
        int finished = samples.Count;
        long readingTicks = 0;

        int ReadNext(byte[] buffer, out int length)
        {
            int i = Interlocked.Increment(ref next);
            long offset = chosen[i] * RegionSize;
            length = (int)Math.Min(RegionSize, image.Length - offset);
            long begun = Stopwatch.GetTimestamp();
            image.Read(offset, buffer, 0, length);
            Interlocked.Add(ref readingTicks, Stopwatch.GetTimestamp() - begun);
            return i;
        }

        var parallel = new ParallelOptions { MaxDegreeOfParallelism = options.Threads, CancellationToken = Ui.Token };
        try
        {
            Parallel.For(0, count, parallel, _ =>
            {
                var buffer = t_region ??= new byte[RegionSize];
                int i, length;
                if (together)
                {
                    i = ReadNext(buffer, out length);
                }
                else
                {
                    lock (gate)
                        i = ReadNext(buffer, out length);
                }
                results[i] = CompressRegion(buffer, 0, length, chosen[i], options);
                report(Interlocked.Increment(ref finished));
            });
        }
        catch (AggregateException e)
        {
            foreach (var inner in e.Flatten().InnerExceptions)
            {
                if (inner is not OperationCanceledException)
                    System.Runtime.ExceptionServices.ExceptionDispatchInfo.Capture(inner).Throw();
            }
            throw new OperationCanceledException();
        }
        samples.AddRange(results);
        return Math.Max(readingTicks / (double)Stopwatch.Frequency / count, 1e-6);
    }

    /// <summary>A solid NCZ keeps track of how far it has got for the thread that's reading it, so all of it is read here first.</summary>
    private static void ReadThenCompress(DataSource image, Options options, long[] chosen, List<RegionResult> samples, Action<int> report)
    {
        int count = chosen.Length;
        var input = new byte[count * RegionSize];
        var lengths = new int[count];
        for (int i = 0; i < count; i++)
        {
            long offset = chosen[i] * RegionSize;
            int length = (int)Math.Min(RegionSize, image.Length - offset);

            // An update can jump back to earlier data in its base game. That region is left out rather than reading it all again.
            try
            {
                image.Read(offset, input, i * RegionSize, length);
                lengths[i] = length;
            }
            catch (NczRewindException)
            {
            }
            report(samples.Count + i + 1);
        }

        var results = new RegionResult[count];
        var parallel = new ParallelOptions { MaxDegreeOfParallelism = options.Threads, CancellationToken = Ui.Token };
        Parallel.For(0, count, parallel, i => results[i] = CompressRegion(input, i * RegionSize, lengths[i], chosen[i], options));
        samples.AddRange(results.Where(r => r.Length > 0));
    }

    private static RegionResult CompressRegion(byte[] input, int start, int regionLength, long region, Options options)
    {
        int stored = 0, lz4 = 0, raw = 0;
        for (int done = 0; done < regionLength; done += options.BlockSize)
        {
            int length = Math.Min(options.BlockSize, regionLength - done);
            int size = RomFsCompressor.CompressedBlockSize(input, start + done, length, options.Level);
            stored += size > 0 ? size : length;
            if (size > 0)
                lz4++;
            else
                raw++;
        }
        return new RegionResult(region, regionLength, stored, lz4, raw);
    }

    private static SampleResult Summarize(List<RegionResult> samples, long imageSize, long regions)
    {
        double sampled = samples.Sum(s => (double)s.Length);
        if (sampled == 0)
            throw new ToolException("无法读取任何游戏数据用于采样");
        double ratio = samples.Sum(s => (double)s.Stored) / sampled;
        double scale = imageSize / sampled;

        // Neighbouring regions tend to hold similar data, so differences between neighbours give the honest spread for
        // an evenly spread sample. An image small enough to read completely has none.
        var ordered = samples.OrderBy(s => s.Region).ToList();
        double squares = 0;
        for (int i = 1; i < ordered.Count; i++)
        {
            double difference = ordered[i].Stored / (double)ordered[i].Length - ordered[i - 1].Stored / (double)ordered[i - 1].Length;
            squares += difference * difference;
        }
        int count = ordered.Count;
        double unsampled = 1 - (double)count / regions;
        double variance = count > 1 ? unsampled / count * squares / (2.0 * (count - 1)) : 0;

        return new SampleResult(
            (long)(ratio * imageSize),
            Confidence95 * Math.Sqrt(variance) * imageSize,
            (long)(samples.Sum(s => (long)s.Lz4Blocks) * scale),
            (long)(samples.Sum(s => (long)s.RawBlocks) * scale));
    }

    private static long GreatestCommonDivisor(long a, long b)
    {
        while (b != 0)
            (a, b) = (b, a % b);
        return a;
    }
}
