using System.Diagnostics;
using System.Security.Cryptography;

namespace HacBrewPackLz;

internal sealed record SizeEstimate(long InputBytes, long OutputBytes, long MarginBytes, long RomFsBytes, long RomFsStoredBytes)
{
    /// <summary>All its game data is already stored the way compressing would store it, so there's nothing to work out.</summary>
    public bool AlreadyCompressed { get; init; }

    /// <summary>For a copy this tool made, what compressing it freed: what the game came to before, which the copy says, less what it is now.</summary>
    public long FreedBytes { get; init; }

    /// <summary>Its makers compressed it, rather than this tool.</summary>
    public bool ByMakers { get; init; }

    /// <summary>For a game file packed tighter than compressing packs, as an NSZ is with Zstandard: the file itself, and what it takes installed
    /// as it is, which every installer unpacks it to. Zero for anything else. The engines say both and leave the input as the file, the same
    /// on the Switch; what's shown and judged goes by <see cref="AsInstalled"/>.</summary>
    public long PackedBytes { get; init; }
    public long InstalledBytes { get; init; }

    /// <summary>What it saves on the console. A packed file is compared with what it takes installed as it is, as the NSP made of it goes on the
    /// console as it is, compressed, while the file itself would be unpacked: an NSP can be bigger than its NSZ and still take less space.</summary>
    public SizeEstimate AsInstalled() => PackedBytes > 0 && InstalledBytes > 0 && InputBytes != InstalledBytes ? this with { InputBytes = InstalledBytes } : this;

    public long SavedBytes => InputBytes - OutputBytes;
    public double SavedFraction => InputBytes > 0 ? (double)SavedBytes / InputBytes : 0;
    public double RomFsFraction => RomFsBytes > 0 ? (double)RomFsStoredBytes / RomFsBytes : 1;

    /// <summary>
    /// What it's sure to save: the estimate less its margin. What a sample can't see, such as data that level packs share, only ever makes
    /// it save more, so this is what's shown, as "at least", and what the minimum saving goes by. One that would grow says by how much, as it is.
    /// </summary>
    public long SureSavedBytes => SavedBytes < 0 ? SavedBytes : Math.Max(0, SavedBytes - MarginBytes);
    public double SureSavedFraction => InputBytes > 0 ? (double)SureSavedBytes / InputBytes : 0;
    /// <summary>What it comes to after, to go with what it's sure to save, so the two always add up.</summary>
    public long MostBytes => InputBytes - SureSavedBytes;
    /// <summary>Whether what it saves is only known to be at least that, rather than worked out to the byte.</summary>
    public bool AtLeast => MarginBytes > 0 && SavedBytes >= 0;

    /// <summary>Whether there's a figure to show for it: what it's sure to save, however little, as the minimum saving is what says whether
    /// that's worth it, or what it would grow by when that's a fact about it (<see cref="SureToGrow"/>). Otherwise it says it saves nothing, at
    /// the size it is, as on the Switch: 0 B, or a little below it from a margin that could go either way, only misled.</summary>
    public bool HasFigure(bool compressing) => SureSavedBytes > 0 || SureToGrow(compressing);

    /// <summary>Growing by more than a sliver as a fact about it, which is shown as growing, rather than a first look's say. Compressing stores
    /// what it can't shrink as it is, and games a first look put over their size came to a few kilobytes less when compressed on a console.
    /// It's a fact once all of it has been read, and when nothing is compressed, as a repack unpacks what's packed. A packed file is compared
    /// here with what it takes installed, so being packed tighter is no fact about it, as it is on the Switch for an NSZ made into a file.</summary>
    public bool SureToGrow(bool compressing) => SavedFraction < -SavingRule.GrowsForSure && (Measured || !compressing);

    /// <summary>Worked out from reading all of the game, rather than from a sample of it, so everything stored once is in it.</summary>
    public bool Measured { get; init; }

    /// <summary>A game with its DLC comes to what they come to together. The margins are independent, so they add up as a sum of squares. DLC with
    /// no game data compressing would touch, such as a few hundred bytes of it, says nothing either way. Any other part that isn't compressed
    /// makes a game that isn't, whatever a first look says it gains, as only reading all of it can say it gains nothing: a first look under half
    /// a percent made a whole game with DLC count as compressed.</summary>
    public static SizeEstimate Together(IReadOnlyList<SizeEstimate> each)
    {
        bool compressed = each.Any(e => e.AlreadyCompressed) && each.All(e => e.AlreadyCompressed || e.RomFsBytes == 0);
        // With a packed part, what's installed as it is: each packed part unpacked, and the rest as it is, as on the Switch.
        bool packed = each.Any(e => e.PackedBytes > 0);
        return new SizeEstimate(each.Sum(e => e.InputBytes), each.Sum(e => e.OutputBytes), (long)Math.Sqrt(each.Sum(e => (double)e.MarginBytes * e.MarginBytes)),
            each.Sum(e => e.RomFsBytes), each.Sum(e => e.RomFsStoredBytes))
        {
            AlreadyCompressed = compressed, Measured = each.Count > 0 && each.All(e => e.Measured), FreedBytes = each.Sum(e => e.FreedBytes),
            ByMakers = each.Any(e => e.ByMakers) && each.All(e => e.ByMakers || e.RomFsBytes == 0),
            PackedBytes = packed ? each.Sum(e => e.InputBytes) : 0,
            InstalledBytes = packed ? each.Sum(e => e.PackedBytes > 0 ? e.InstalledBytes : e.InputBytes) : 0,
        };
    }
}

/// <summary>Whether a title is worth the time it takes to compress, for people after the most space in the least time.</summary>
internal static class SavingRule
{
    /// <summary>Whether the minimum has a say at all. If not, there's no need to analyze anything first.</summary>
    public static bool Applies(Options options) => options.Compress && options.MinSavingPercent > 0;

    /// <summary>A first look sure to save less than this can't vouch for the game, which is measured before it's ticked (<see cref="NotSure"/>),
    /// as on the Switch. Whatever it's then sure to save is shown and goes by the minimum: "nothing to gain" is for nothing at all.</summary>
    public const double NothingToGain = 0.005;

    /// <summary>
    /// What compressing can't shrink is stored as it is, so it never makes game data more than a fraction of a percent bigger. A game that
    /// would grow by more than this is packed with something stronger already, such as an NSZ's Zstandard, and reading all of it wouldn't
    /// change that: it's a fact about the file, not something a sample missed.
    /// </summary>
    public const double GrowsForSure = 0.01;

    /// <summary>Whether it's left alone as already as small as it gets, and the minimum, both go by what it's sure to save.</summary>
    public static bool WorthCompressing(SizeEstimate estimate, Options options)
    {
        if (options.Compress && (estimate.AlreadyCompressed || estimate.SureSavedBytes <= 0))
            return false;
        return !Applies(options) || estimate.SureSavedFraction * 100 >= options.MinSavingPercent;
    }

    /// <summary>
    /// A first look can say a game is worth compressing, but not that it isn't: what a sample can't see, such as data that level packs share,
    /// only makes a game save more. So one it can't vouch for isn't left out on its say. It's measured first, by reading all of it and writing
    /// nothing, and compressed only if what's measured is worth it. Only what's known about the file itself, that it's already compressed,
    /// leaves a game out without that, and growing when that's a fact (<see cref="SizeEstimate.SureToGrow"/>). Over its size on a first look
    /// alone isn't: what compressing can't shrink is stored as it is, and such games, compressed, came to a few kilobytes less.
    /// </summary>
    public static bool NotSure(SizeEstimate estimate, Options options) =>
        options.Compress && !estimate.Measured && !estimate.AlreadyCompressed && !estimate.SureToGrow(options.Compress)
        && (estimate.SureSavedFraction < NothingToGain || !WorthCompressing(estimate, options));

    /// <summary>What the command line says about a game a first look can't vouch for, as a reason a sentence can go on with.</summary>
    public static string NotSureNote(Options options) => Applies(options)
        ? $"尚未确定，可能还能省更多：「完整分析」会完整读取全部数据（不写入任何文件），确认是否至少能省 {options.MinSavingPercent}%"
        : "尚未确定，可能还能省更多：「完整分析」会完整读取全部数据（不写入任何文件），确认是否能省空间";

    /// <summary>Whether it's left out by the minimum alone: compressing it saves something, less than that.</summary>
    public static bool UnderMinimum(SizeEstimate estimate, Options options) =>
        Applies(options) && !estimate.AlreadyCompressed && estimate.SavedBytes >= 0 && estimate.SureSavedBytes > 0
        && estimate.SureSavedFraction * 100 < options.MinSavingPercent;

    public static string Reason(SizeEstimate estimate, Options options)
    {
        // A copy this tool made says how much smaller it is than the game was. One that says neither that nor that its makers compressed
        // it had nothing compressed: it's as small as it gets, as a game with all its data in its program is.
        if (estimate.AlreadyCompressed)
            return estimate.ByMakers ? "发行商已自行压缩"
                : estimate.FreedBytes > 0 ? $"已压缩过，比原来小 {ProgressReporter.Format(estimate.FreedBytes)}" : "没有压缩空间：已经是最小的了";
        // Growing by less than compressing ever makes anything grow is only as small as it gets: "would grow by 0%" says nothing. So is growing
        // more on a first look's say alone (SizeEstimate.SureToGrow), and being sure to save nothing at all.
        if (estimate.SureSavedBytes <= 0 && !estimate.SureToGrow(options.Compress))
            return "没有压缩空间：已经是最小的了";
        if (estimate.SavedBytes < 0)
            return Applies(options) ? $"体积会增大 {-estimate.SavedFraction * 100:0}%，低于设定的 {options.MinSavingPercent}% 下限" : $"体积会增大 {-estimate.SavedFraction * 100:0}%";
        // Rounded, just under the minimum would read as the minimum itself, so close to it, it says to a tenth.
        double percent = estimate.SureSavedFraction * 100;
        string figure = percent >= options.MinSavingPercent - 0.5 ? $"{Math.Floor(percent * 10) / 10:0.0}%" : $"{percent:0}%";
        return estimate.AtLeast && !estimate.Measured
            ? $"初步只能确定可省 {figure}，低于设定的 {options.MinSavingPercent}% 下限"
            : $"只能省 {figure}，低于设定的 {options.MinSavingPercent}% 下限";
    }
}

/// <summary>Refuses before anything gets written, rather than running out of room part way through.</summary>
internal static class RoomCheck
{
    /// <summary>Space on top of the data itself that's kept free while working.</summary>
    private const long RoomToSpare = 64L << 20;

    /// <summary>An NSP can't be split, so this is the end of it on a disk that can't hold 4 GB in one file.</summary>
    private const long Fat32Limit = 0xFFFFFFFFL;

    /// <summary>For the titles that go into one file: a game, and its DLC with it.</summary>
    public static void ForTitles(IReadOnlyList<TitleJob> jobs, Options options, string folder)
    {
        // Compressed, it can't come to much more than what it's made from before any compression, so with room for that there's no need to work it out.
        long free = FreeSpace(folder, out bool fat32);
        long most = jobs.Sum(job => job.Main.Meta.Records.Sum(r => r.Size) + (job.Update?.Meta.Records.Sum(r => r.Size) ?? 0));
        most += most / 8;
        if (options.Compress && free >= most + RoomToSpare && (!fat32 || most < Fat32Limit))
            return;
        var outputs = jobs.Select((job, i) =>
        {
            try
            {
                var estimate = Analyzer.Estimate(job, options);
                return estimate.OutputBytes + estimate.MarginBytes;
            }
            catch (Exception e) when (i > 0 && TitleConverter.IsDlcFailure(e))
            {
                throw TitleConverter.DlcFailed(job, e);
            }
        }).ToList();
        if (Shortfall(outputs.Sum(), free, fat32, folder) is not { } problem)
            return;
        // When the game alone would fit, its DLC is what tips it over, so the message says how to make it smaller.
        if (outputs.Count > 1 && Shortfall(outputs[0], free, fat32, folder) is null)
            problem += "。关闭「整合打包」（--mergedlc）会让它更小。";
        throw new ToolException(problem);
    }

    public static void ForBuild(Options options, string folder)
    {
        long free = FreeSpace(folder, out bool fat32);
        var folders = new[] { options.ExefsDir, options.NoRomfs ? null : options.RomfsDir, options.NoLogo ? null : options.LogoDir, options.ControlDir, options.HtmlDocDir, options.LegalInfoDir };
        long most = folders.Where(Directory.Exists).Sum(d => RomFsBuilder.FolderBytes(d!));
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
            throw new ToolException($"「{folder}」至少需要 {ProgressReporter.Format(job.InputBytes + RoomToSpare)} 可用空间，目前只有 {ProgressReporter.Format(free)}");
    }

    private static void Require(long output, long free, bool fat32, string folder)
    {
        if (Shortfall(output, free, fat32, folder) is { } problem)
            throw new ToolException(problem);
    }

    /// <summary>Why a file this big can't be written there. Null if it can.</summary>
    private static string? Shortfall(long output, long free, bool fat32, string folder)
    {
        if (fat32 && output >= Fat32Limit)
            return $"预计生成 {ProgressReporter.Format(output)}，但「{folder}」所在磁盘是 FAT32，无法存放 4 GB 及以上的单个文件";
        if (free >= 0 && free < output + RoomToSpare)
            return $"「{folder}」大约需要 {ProgressReporter.Format(output + RoomToSpare)} 可用空间，目前只有 {ProgressReporter.Format(free)}";
        return null;
    }

    /// <summary>Negative if there's no telling.</summary>
    private static long FreeSpace(string folder, out bool fat32)
    {
        fat32 = false;
        // For the tests: a disk with only this much free.
        if (long.TryParse(Environment.GetEnvironmentVariable("HACBREWPACKLZ_TEST_FREE_SPACE"), out long test))
            return test;
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
    /// <summary>A game smaller than this, as the list shows it, is measured exactly: every block compressed, in about twice the time. Above it, the
    /// margin of measuring every 16th block is a small part of what a game saves. The same on the Switch (MeasureExactlyBelow).</summary>
    public const long MeasureExactlyBelow = 1L << 30;

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

    public static SizeEstimate Estimate(TitleJob job, Options options) => Work(job, options, measure: false);

    /// <summary>
    /// What compressing comes to, from reading all of the game once and writing nothing, with its hashes checked as it's read, so a damaged
    /// game fails here rather than part way through being written. Everything compressing stores once is in it, and how well the rest
    /// compresses is measured on a fixed share of it, the same every time. It's for a game a first look can't vouch for.
    /// </summary>
    public static SizeEstimate Measure(TitleJob job, Options options) => Work(job, options, measure: true);

    private static SizeEstimate Work(TitleJob job, Options options, bool measure)
    {
        if (IsAlreadyCompressed(job, options))
        {
            // A copy this tool made says what the game came to before, which is what compressing it freed. One from before that's worked
            // out as it unpacks.
            bool ours = IsMadeByThisTool(job);
            long freed = ours ? Math.Max(0, (CopyMark.In(job)?.OriginalBytes ?? UnpackedBytes(job, options)) - job.InputBytes) : 0;
            return new SizeEstimate(job.InputBytes, job.InputBytes, 0, 0, 0)
                { AlreadyCompressed = true, Measured = measure, FreedBytes = freed, ByMakers = !ours && HasCompressedGameData(job) };
        }
        var tally = new Tally();
        long output = NspWriter.HeaderSize(job.Contents.Count);
        // A solid NSZ read out of order, as a base game under its update is, keeps a copy aside, up to the whole game's size: where converting
        // keeps it, rather than in Windows' temporary folder on C:, which can be the smaller disk.
        NczSource.SpoolFolder = options.TempDir ?? (options.NspDirSet ? options.NspDir : Path.GetDirectoryName(Path.GetFullPath(job.Main.Archive.FilePath)));
        try
        {
            // A game made one with its update has the map of what the update kept in its control data, compressed or not, as the Switch
            // makes a newer update part of either with it, and a compressed copy has its mark there too: the control data is made again
            // with them, as converting makes it.
            var map = UpdateMap.For(job, options.Compress);
            var mark = options.Compress ? CopyMark.For(job, options) : null;
            foreach (var content in job.Contents)
            {
                long nca = NcaWriter.HeaderRegionSize;
                foreach (int i in content.SectionsInFileOrder())
                {
                    var source = content.SectionAt(i)!.Value;
                    nca += NcaWriter.AlignToSector(content.Type == Cnmt.ContentTypeControl && source.Section.FsType == NcaFsSection.FsRomFs && (map is not null || mark is not null)
                        ? ControlSize(source, map, mark) : SectionSize(content, source, options, tally, measure));
                }
                output += nca;
            }
        }
        finally
        {
            job.ReleaseMemory();
        }

        var meta = job.Main.Meta;
        output += MetaNcaSize(Cnmt.FileName(meta.TitleId, meta.Type), meta.RebuiltSize(job.Contents.Count));
        var estimate = tally.ToEstimate(job.InputBytes, output) with { Measured = measure };
        // An NSZ is smaller than what it unpacks to, which is what installing it any other way takes.
        return IsPacked(job) ? estimate with { PackedBytes = job.InputBytes, InstalledBytes = CopyMark.InstalledBytes(job) } : estimate;
    }

    /// <summary>Whether any of it is packed as an NSZ or XCZ packs it, which installing unpacks.</summary>
    public static bool IsPacked(TitleJob job) => job.Contents.Any(c => c.Base?.Packed == true || c.Patch?.Packed == true);

    /// <summary>Whether this tool compressed its game data, rather than its makers: this tool lays a compressed section out its own way.</summary>
    public static bool IsMadeByThisTool(TitleJob job) => job.Contents.Any(c =>
        c.Primary.Sections.Any(section => section is { HasCompression: true } && c.Primary.IsRebuiltCompressed(section)));

    private static bool HasCompressedGameData(TitleJob job) => job.Contents.Any(c => c.Primary.Sections.Any(section => section is { HasCompression: true }));

    /// <summary>What a game's contents come to unpacked, worked out from what they say, without the content meta, as a content meta counts them.</summary>
    public static long UnpackedBytes(TitleJob job, Options options)
    {
        var plain = options.Clone();
        plain.Compress = false;
        long total = 0;
        foreach (var content in job.Contents)
        {
            long nca = NcaWriter.HeaderRegionSize;
            foreach (int i in content.SectionsInFileOrder())
                nca += NcaWriter.AlignToSector(SectionSize(content, content.SectionAt(i)!.Value, plain, new Tally(), measure: false));
            total += nca;
        }
        return total;
    }

    /// <summary>Whether every part of it compressing would compress is compressed already. An update always makes something new. A game
    /// with nothing compressing would touch, such as one that keeps all its data in its program, counts too: it's as small as it gets, and
    /// otherwise its compressed copy would come back on the list each time it's analyzed. One whose file is made again to hold its game's DLC
    /// makes something new too.</summary>
    public static bool IsAlreadyCompressed(TitleJob job, Options options) =>
        options.Compress && job.Update is null && !job.DlcNotInFile && AllCompressed(job.Contents, options);

    /// <summary>The same for a title's contents as they are, with no update: every part compressing would compress is compressed already.</summary>
    public static bool AllCompressed(IReadOnlyList<ContentPair> contents, Options options)
    {
        foreach (var content in contents)
        {
            foreach (int i in content.SectionsInFileOrder())
            {
                var source = content.SectionAt(i)!.Value;
                if (TitleConverter.Plan(content, source, options) != SectionPlan.CompressRomFs)
                    continue;
                if (!source.Section.HasCompression)
                    return false;
            }
        }
        return contents.Count > 0;
    }

    /// <summary>The same for building from folders. Nothing here depends on the keys. Measured, the game data is read in full, as
    /// <see cref="Measure"/> does.</summary>
    public static SizeEstimate EstimateBuild(Options options, bool measure = false)
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
            program += NcaWriter.AlignToSector(!options.Compress ? PlainRomFsSize(image.Length, tally)
                : measure ? MeasuredRomFsSize(image, options, tally, null)
                : CompressedRomFsSize(image, options, tally));
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
        return tally.ToEstimate(input, output) with { Measured = measure };
    }

    /// <summary>Prints an estimate of every title and returns the exit code.</summary>
    public static int Report(TitleSet set, Options options, List<GameFolder>? folders = null)
    {
        folders ??= [];
        long input = 0, output = 0;
        double variance = 0;
        int failures = 0;
        var ranking = new List<(string name, ulong titleId, SizeEstimate estimate)>();
        foreach (var job in set.Jobs)
        {
            Ui.Info("");
            Ui.Heading(job.Describe());
            try
            {
                // What it saves on the console, which for an NSZ isn't what the file comes to.
                var estimate = Estimate(job, options).AsInstalled();
                // Asked to, what a first look can't vouch for is read in full, writing nothing.
                if (options.Measure && SavingRule.NotSure(estimate, options))
                {
                    Ui.Info("  快速预览无法判断节省量，将完整读取全部数据（不写入任何文件）");
                    estimate = Measure(job, options.MeasuringGameOf(job.InputBytes)).AsInstalled();
                    Ui.Info($"  {MeasuredLine(estimate)}");
                }
                Print(estimate, options);
                if (estimate.PackedBytes > 0)
                    Ui.Info($"  文件大小：  {ProgressReporter.Format(estimate.PackedBytes)}，安装时会解包为上面显示的 {ProgressReporter.Format(estimate.InputBytes)}");
                if (SavingRule.NotSure(estimate, options))
                    Ui.Info($"  尚未确定：{SavingRule.NotSureNote(options)["尚未确定，可能还能省更多：".Length..]}");
                else if (!SavingRule.WorthCompressing(estimate, options))
                    Ui.Warn($"  已跳过：{SavingRule.Reason(estimate, options)}");
                ranking.Add((job.DisplayName, job.Main.Meta.TitleId, estimate));
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
        // Game folders, as building them would make them.
        foreach (var folder in folders)
        {
            Ui.Info("");
            Ui.Heading(Batch.FolderLine(folder));
            try
            {
                var estimate = EstimateBuild(Batch.BuildOptionsFor(folder, options));
                if (options.Measure && SavingRule.NotSure(estimate, options))
                {
                    Ui.Info("  快速预览无法判断节省量，将完整读取全部数据（不写入任何文件）");
                    estimate = EstimateBuild(Batch.BuildOptionsFor(folder, options).MeasuringGameOf(folder.Bytes), measure: true);
                    Ui.Info($"  {MeasuredLine(estimate)}");
                }
                Print(estimate, options);
                if (SavingRule.NotSure(estimate, options))
                    Ui.Info($"  尚未确定：{SavingRule.NotSureNote(options)["尚未确定，可能还能省更多：".Length..]}");
                else if (!SavingRule.WorthCompressing(estimate, options))
                    Ui.Warn($"  已跳过：{SavingRule.Reason(estimate, options)}");
                ranking.Add((folder.Name, folder.TitleId, estimate));
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

        int titles = set.Jobs.Count + folders.Count;
        if (titles > 1 && failures < titles)
        {
            Ui.Info("");
            Ui.Heading("全部游戏");
            Print(new SizeEstimate(input, output, (long)Math.Ceiling(Math.Sqrt(variance)), 0, 0), options);
            PrintRanking(ranking, options);
        }
        PrintClosingNote();
        foreach (var problem in set.Problems)
            Ui.Warn($"未处理：{problem.Message}");
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
    private static void PrintRanking(List<(string name, ulong titleId, SizeEstimate estimate)> ranking, Options options)
    {
        Ui.Info("");
        Ui.Heading("节省最多的排前面");
        foreach (var (name, titleId, estimate) in ranking.OrderByDescending(r => r.estimate.SureSavedFraction).ThenByDescending(r => r.estimate.SureSavedBytes))
        {
            // Sure to save nothing, it says so, rather than 0 B or a hair over its size. One a first look can't vouch for says that instead.
            bool figure = estimate.HasFigure(options.Compress);
            bool notSure = SavingRule.NotSure(estimate, options);
            string saved = figure ? (estimate.SavedBytes >= 0 ? ProgressReporter.Format(estimate.SureSavedBytes) : "-" + ProgressReporter.Format(-estimate.SavedBytes))
                : notSure ? "not sure" : estimate.AlreadyCompressed ? "nothing more" : "nothing";
            string percent = !figure ? "" : estimate.SavedBytes >= 0 ? $"{estimate.SureSavedFraction * 100:0}%" : $"+{-estimate.SavedFraction * 100:0}%";
            string line = $"  {percent,5}  {saved,10}  {name} [{titleId:X16}]";
            if (SavingRule.WorthCompressing(estimate, options))
                Ui.Info(line);
            else if (notSure)
                Ui.Info(figure ? line + ", not sure yet" : line);
            else
                Ui.Detail(line + "，已排除");
        }

        var worth = ranking.Where(r => SavingRule.WorthCompressing(r.estimate, options) && r.estimate.SureSavedBytes > 0).OrderByDescending(r => r.estimate.SureSavedFraction).ToList();
        if (worth.Count > 0 && worth.Count < ranking.Count)
        {
            Ui.Info("");
            Ui.Info($"只压缩确定值得的游戏：convert --only {string.Join(',', worth.Select(r => $"{r.titleId:X16}"))}");
        }
        // What isn't sure yet is only known by reading all of it, which takes a while, so it's asked for.
        if (ranking.Any(r => SavingRule.NotSure(r.estimate, options)))
            Ui.Info("尚未确定的游戏可能还能省更多。加 --measure 会完整读取它们（不写入任何文件）；convert --measure 会只压缩其中值得的游戏。");
    }

    private static void PrintClosingNote()
    {
        Ui.Info("");
        Ui.Detail("这是基于游戏数据采样的估算值，实际可能省得更多。尚未写入任何文件。");
    }

    /// <summary>What reading all of a game came to, as a line of its own.</summary>
    public static string MeasuredLine(SizeEstimate e) => !e.HasFigure(compressing: true)
        ? e.SureSavedBytes > 0
            ? $"实测：几乎不节省，{ProgressReporter.Format(e.InputBytes)} 中仅省 {ProgressReporter.Format(e.SureSavedBytes)}"
            : $"实测：无法确定能节省，{ProgressReporter.Format(e.InputBytes)} 按原样保留"
        : e.SavedBytes >= 0
        ? $"实测：{(e.AtLeast ? "至少 " : "")}可节省 {ProgressReporter.Format(e.SureSavedBytes)}（{e.SureSavedFraction * 100:0}%），{ProgressReporter.Format(e.InputBytes)} → {ProgressReporter.Format(e.MostBytes)}"
        : $"实测：反而增大 {ProgressReporter.Format(-e.SavedBytes)}（{-e.SavedFraction * 100:0}%）";

    private static void Print(SizeEstimate e, Options options)
    {
        string margin = e.MarginBytes > 0 ? $"（误差约 {ProgressReporter.Format(e.MarginBytes)}）" : "";
        string prefix = e.MarginBytes > 0 ? "约 " : "";
        Ui.Info($"  当前：      {ProgressReporter.Format(e.InputBytes)}");
        Ui.Info($"  处理后：    {prefix}{ProgressReporter.Format(e.OutputBytes)}{margin}");
        Ui.Detail($"  字节数：    {e.OutputBytes} +/- {e.MarginBytes}");
        Ui.Info(!e.HasFigure(options.Compress) ? (e.SureSavedBytes > 0 ? $"  可节省：    几乎为 0，仅 {ProgressReporter.Format(e.SureSavedBytes)}" : "  可节省：    无法确定")
            : e.SavedBytes >= 0
            ? $"  可节省：    {(e.AtLeast ? "至少 " : "")}{ProgressReporter.Format(e.SureSavedBytes)}（{e.SureSavedFraction * 100:0}%）"
            : $"  反而增大：  {ProgressReporter.Format(-e.SavedBytes)}（{-e.SavedFraction * 100:0}%）");
        if (e.RomFsBytes > 0 && options.Compress)
            Ui.Info($"  游戏数据：  {ProgressReporter.Format(e.RomFsBytes)} 压缩为 {ProgressReporter.Format(e.RomFsStoredBytes)}（{e.RomFsFraction * 100:0}%）");
    }

    private static long SectionSize(ContentPair content, SectionSource source, Options options, Tally tally, bool measure)
    {
        var (owner, section, original) = source;
        switch (TitleConverter.Plan(content, source, options))
        {
            case SectionPlan.CompressRomFs when measure:
                return MeasuredRomFsSize(owner.OpenRomFsImage(section, original), options, tally, SharedPieces.KeptBy(owner, section, original));
            case SectionPlan.CompressRomFs:
                bool inOrder = owner.ReadsInOrder || original?.ReadsInOrder == true;
                var image = owner.OpenRomFsImage(section, original, verify: false);
                var shared = SharedPieces.Make(image.Length, options.BlockSize, SharedPieces.KeptBy(owner, section, original));
                return CompressedRomFsSize(image, options, tally, inOrder, shared);
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

    /// <summary>The control data made again with the map and the mark, as the copy has it.</summary>
    private static long ControlSize(SectionSource source, UpdateMap? map, CopyMark? mark)
    {
        var (owner, section, original) = source;
        var image = owner.OpenRomFsImage(section, original, verify: false);
        var archive = RomFsArchive.Open(image, $"{owner.Name} section {section.Index}");
        return NcaSections.RomFsSectionSize(RomFsBuilder.BuildFrom(TitleConverter.ControlFiles(archive, null, map, mark), archive.Directories).Length);
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

    private static long CompressedRomFsSize(DataSource image, Options options, Tally tally, bool inOrder = false, SharedPieces.Plan? shared = null)
    {
        // What's held again is stored where it's held first, so the sample leaves it out wherever it's held again, and how well the rest
        // compresses goes for all of the rest alone, as what's held again can compress better or worse. Each place it's held again takes a
        // table entry of its own. That's what the update's map has held again, and the copies of files the file table shows: of those, only
        // what isn't held before is stored, and how well it compresses goes by the rest. A solid NCZ can only be read in order, so it has no
        // look for copies.
        var clock = Stopwatch.StartNew();
        long[] repeats = shared?.Blocks.Where(b => b.Piece >= 0 && !b.First).Select(b => b.Offset).ToArray() ?? [];
        double seconds = FileCopies.TimeShare * TimeBudgetSeconds * Math.Min(1.0, image.Length / (double)(1L << 30));
        List<FileCopies.Copy> copies = inOrder ? [] : FileCopies.Find(image, repeats, options.BlockSize, seconds);
        long held = copies.Sum(c => c.Held);
        long counted = Math.Max(0, image.Length - (long)repeats.Length * options.BlockSize - held);
        var sample = Sample(image, options, inOrder, LeftOut(repeats, options.BlockSize, copies), counted, clock.Elapsed.TotalSeconds);
        tally.RomFs += image.Length;
        tally.Stored += sample.Stored;
        // How much of a near copy is held before is taken from what others came to, so it's give or take a little of it.
        double nearMargin = 0.05 * copies.Where(c => c.Near).Sum(c => c.Held) * (counted > 0 ? (double)sample.Stored / counted : 1);
        tally.Variance += sample.Margin * sample.Margin + nearMargin * nearMargin;

        // Raw blocks in a row share one table entry, so there can't be more raw entries than gaps between compressed blocks.
        long entries = Math.Min(sample.Lz4Blocks + Math.Min(sample.RawBlocks, sample.Lz4Blocks + 1) + repeats.Length + held / options.BlockSize, int.MaxValue);
        return NcaSections.CompressedRomFsSectionSize(sample.Stored, (int)Math.Max(entries, 1));
    }

    /// <summary>All of the image read once, as compressing reads it, and what it comes to counted rather than written.</summary>
    private static long MeasuredRomFsSize(DataSource image, Options options, Tally tally, IReadOnlyList<UpdateMap.Range>? kept)
    {
        var clock = Stopwatch.StartNew();
        var progress = new ProgressReporter("正在测量游戏数据", image.Length);
        var measured = RomFsCompressor.Measure(image, options.BlockSize, options.Level, options.Threads, progress, kept, Math.Max(options.MeasureEvery, 1));
        progress.Done();
        tally.RomFs += image.Length;
        tally.Stored += measured.Stored;
        tally.Variance += measured.Margin * measured.Margin;
        Ui.Detail($"已测量 {ProgressReporter.Format(image.Length)} 游戏数据，用时 {clock.Elapsed.TotalSeconds:0.0} 秒：其中 {ProgressReporter.Format(measured.HeldBytes)} 为重复数据，"
            + $"其余合计 {ProgressReporter.Format(measured.Stored)}，误差约 {ProgressReporter.Format((long)Math.Ceiling(measured.Margin))}");

        // Raw blocks in a row share one table entry, so there can't be more raw entries than gaps between compressed blocks.
        long entries = Math.Min(measured.Lz4Blocks + Math.Min(measured.RawBlocks, measured.Lz4Blocks + 1) + measured.Held, int.MaxValue);
        return NcaSections.CompressedRomFsSectionSize(measured.Stored, (int)Math.Max(entries, 1));
    }

    /// <summary>A stretch of the image the sample leaves out, as it's stored somewhere else.</summary>
    private readonly record struct Skip(long Start, long End);

    /// <summary>The blocks the update's map has held again and the later copies of files, in order, any that touch made one.</summary>
    private static Skip[] LeftOut(long[] repeats, int blockSize, List<FileCopies.Copy> copies)
    {
        var all = repeats.Select(r => new Skip(r, r + blockSize)).Concat(copies.Select(c => new Skip(c.Offset, c.Offset + c.Length)))
            .OrderBy(s => s.Start).ToList();
        var merged = new List<Skip>(all.Count);
        foreach (var skip in all)
        {
            if (merged.Count > 0 && skip.Start <= merged[^1].End)
                merged[^1] = merged[^1] with { End = Math.Max(merged[^1].End, skip.End) };
            else
                merged.Add(skip);
        }
        return merged.ToArray();
    }

    private readonly record struct SampleResult(long Stored, double Margin, long Lz4Blocks, long RawBlocks);

    /// <summary>Counted is what of the region was compressed, which is all of it but any piece that's held again.</summary>
    private readonly record struct RegionResult(long Region, int Length, int Stored, int Lz4Blocks, int RawBlocks, int Counted);

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
    ///
    /// <paramref name="skips"/> are what's stored somewhere else, in order: where a piece of the original game is held again, and later
    /// copies of files. A region counts nothing for what it has of one, however that would compress. <paramref name="counted"/> is how
    /// much of the image is stored where it is, and <paramref name="spent"/> the time already taken looking for copies, out of the time allowed.
    /// </summary>
    private static SampleResult Sample(DataSource image, Options options, bool inOrder, Skip[] skips, long counted, double spent)
    {
        long regions = (image.Length + RegionSize - 1) / RegionSize;
        if (regions == 0)
            return default;

        long stride = (long)(regions * GoldenRatioConjugate);
        while (GreatestCommonDivisor(stride, regions) != 1)
            stride++;

        // A small image takes up less of the answer, and doesn't need as close a look.
        int minimum = (int)Math.Min(MinRegions, Math.Max(regions / 16, SmallestSample));
        double budget = Math.Max(0, TimeBudgetSeconds * Math.Min(1.0, image.Length / (double)(1L << 30)) - spent);

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
                    ReadThenCompress(image, options, chosen, skips, samples, Report);
                }
                else
                {
                    double perRegion = ReadWhileCompressing(image, options, chosen, skips, together || trial == ReadingTrial.Together, samples, Report);
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
                result = Summarize(samples, counted);
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
    private static double ReadWhileCompressing(DataSource image, Options options, long[] chosen, Skip[] skips, bool together, List<RegionResult> samples, Action<int> report)
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
                results[i] = CompressRegion(buffer, 0, length, chosen[i], options, skips);
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
    private static void ReadThenCompress(DataSource image, Options options, long[] chosen, Skip[] skips, List<RegionResult> samples, Action<int> report)
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
        Parallel.For(0, count, parallel, i => results[i] = CompressRegion(input, i * RegionSize, lengths[i], chosen[i], options, skips));
        samples.AddRange(results.Where(r => r.Length > 0));
    }

    /// <summary>What's around a piece held again is compressed in blocks from where it starts, as the compressor cuts them.</summary>
    private static RegionResult CompressRegion(byte[] input, int start, int regionLength, long region, Options options, Skip[] skips)
    {
        int stored = 0, lz4 = 0, raw = 0, counted = 0;
        long offset = region * RegionSize;
        int skip = FirstEndingAfter(skips, offset);
        for (int done = 0; done < regionLength;)
        {
            if (skip < skips.Length && skips[skip].Start <= offset + done)
            {
                done = (int)Math.Min(regionLength, skips[skip].End - offset);
                skip++;
                continue;
            }
            int until = skip < skips.Length ? (int)Math.Min(regionLength, skips[skip].Start - offset) : regionLength;
            int length = Math.Min(options.BlockSize, until - done);
            int size = RomFsCompressor.CompressedBlockSize(input, start + done, length, options.Level);
            stored += size > 0 ? size : length;
            if (size > 0)
                lz4++;
            else
                raw++;
            counted += length;
            done += length;
        }
        return new RegionResult(region, regionLength, stored, lz4, raw, counted);
    }

    /// <summary>The first of the stretches left out that ends after the offset.</summary>
    private static int FirstEndingAfter(Skip[] skips, long offset)
    {
        int low = 0, high = skips.Length;
        while (low < high)
        {
            int middle = (low + high) / 2;
            if (skips[middle].End <= offset)
                low = middle + 1;
            else
                high = middle;
        }
        return low;
    }

    /// <summary>
    /// <paramref name="counted"/> is how much of the image is stored where it is: all of it but what's held again. How well that compresses
    /// comes only from what of it the sample has, as what's held again can compress better or worse than the rest.
    /// </summary>
    private static SampleResult Summarize(List<RegionResult> samples, long counted)
    {
        if (samples.Sum(s => (double)s.Length) == 0)
            throw new ToolException("无法读取任何游戏数据用于采样");
        double sampled = samples.Sum(s => (double)s.Counted);
        // All it came to was pieces held again: what's left is taken as not compressing at all, give or take all of it.
        if (sampled == 0)
            return new SampleResult(counted, counted, 0, 0);
        double ratio = samples.Sum(s => (double)s.Stored) / sampled;
        double scale = counted / sampled;

        // Neighbouring regions tend to hold similar data, so differences between neighbours give the honest spread for
        // an evenly spread sample. An image small enough to read completely has none.
        var ordered = samples.Where(s => s.Counted > 0).OrderBy(s => s.Region).ToList();
        double squares = 0;
        for (int i = 1; i < ordered.Count; i++)
        {
            double difference = ordered[i].Stored / (double)ordered[i].Counted - ordered[i - 1].Stored / (double)ordered[i - 1].Counted;
            squares += difference * difference;
        }
        int count = ordered.Count;
        double unsampled = Math.Max(0, 1 - sampled / counted);
        double variance = count > 1 ? unsampled / count * squares / (2.0 * (count - 1)) : 0;

        return new SampleResult(
            (long)(ratio * counted),
            Confidence95 * Math.Sqrt(variance) * counted,
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

/// <summary>
/// What a game's file table shows it holds again: later copies of its files, whole or nearly so, which compressing finds and stores once.
/// The table has every file's name, size and place, so a few small reads tell a copy. The same size and the same 4 KB from the middle, the
/// start and the end make a copy of the whole file. The same name, about the same size, and an end or a start much the same as the earlier
/// one's make a near copy, as when a game keeps a pack for each area with what the areas share, or a DLC folder holds the game's sound banks
/// again. What else compressing finds held again, such as what level packs have in common, can only be seen by reading all of it.
/// </summary>
internal static class FileCopies
{
    /// <summary>A later copy, where it is, and how much of it compressing finds held before.</summary>
    public readonly record struct Copy(long Offset, long Length, long Held, bool Near);

    /// <summary>Smaller files have too little to gain to be worth a look: from 256 KB, Metroid Dread's packs had a smaller one with the same
    /// name, which took them for versions of one file, and reading took three times as long.</summary>
    private const long Smallest = 1 << 20;
    private const int Piece = 0x1000;
    /// <summary>Read from the end, then the start, of a later file, and of the earlier one, which leaves room for what's there to sit a little further along.</summary>
    private const int LaterEnd = 0x10000;
    private const int EarlierEnd = 0x40000;
    /// <summary>A later file's end or start has to have this much of what it holds in the earlier one's, from at least a few spots.</summary>
    private const double EndsAlike = 0.8;
    private const int FewestSpots = 4;
    /// <summary>Files with the same name that differ in size by more than this aren't taken as copies.</summary>
    private const double CloseInSize = 0.2;
    /// <summary>
    /// Three or more with the same name whose sizes spread further than this are versions of one file, such as one for each language, not
    /// copies: Dying Light's menu packs for five languages end the same, and share a third.
    /// </summary>
    private const double VersionsSpread = 1.2;
    /// <summary>How much of a near copy is held before, of the smaller of the two: 87-99% in Metroid Dread's area packs and Outlast's sound banks.</summary>
    public const double NearHeld = 0.95;
    /// <summary>
    /// The most reads one game's data takes, so even one with many files the same size is quick: Metroid Dread, Dying Light and Splatoon 3
    /// needed up to 137, and Outlast, with a sound bank again for each of many in its DLC, found 87% of its copies in 128.
    /// </summary>
    public const int MostReads = 128;
    /// <summary>
    /// Reads that are always made, and then only for this share of the time the sample has, so a hard drive, with a seek for every read,
    /// stops after the copies that could hold the most. A small game, with few to make, comes to the same each time.
    /// </summary>
    private const int FirstReads = 16;
    public const double TimeShare = 0.25;

    /// <summary>A group of files the same size, or a later file with the same name as an earlier one, and how much it could find held again.</summary>
    private sealed record Check(long Potential, long Offset, List<RomFsArchive.Entry> Group, RomFsArchive.Entry? Later, RomFsArchive.Entry? Earlier);

    public static List<Copy> Find(DataSource image, long[] repeats, int blockSize, double seconds, int mostReads = MostReads)
    {
        var clock = Stopwatch.StartNew();
        var copies = new List<Copy>();
        List<RomFsArchive.Entry> files;
        try
        {
            // Two files in the same place are the same bytes, stored once already.
            files = RomFsArchive.Open(image, "The game data").Files.Where(f => f.Size >= Smallest).OrderBy(f => f.Offset).DistinctBy(f => f.Offset).ToList();
        }
        catch (ToolException)
        {
            // Compressing will say what's wrong with it.
            return copies;
        }

        // Every check there is to make, the one that could find the most first, so a game with many files the same size still gets to its
        // biggest copies within the reads allowed.
        var checks = new List<Check>();
        foreach (var group in files.GroupBy(f => f.Size).Select(g => g.ToList()).Where(g => g.Count > 1))
            checks.Add(new Check(group[0].Size * (group.Count - 1), group[0].Offset, group, null, null));
        foreach (var group in files.GroupBy(f => NameOf(f.Path), StringComparer.Ordinal).Select(g => g.ToList()).Where(g => g.Count > 1))
        {
            if (group.Count >= 3 && group.Max(f => f.Size) > VersionsSpread * group.Min(f => f.Size))
                continue;
            for (int i = 1; i < group.Count; i++)
            {
                var later = group[i];
                var earlier = group.Take(i).MinBy(f => Math.Abs(f.Size - later.Size))!;
                if (Math.Abs(later.Size - earlier.Size) <= CloseInSize * Math.Max(later.Size, earlier.Size))
                    checks.Add(new Check(Math.Min(later.Size, earlier.Size), later.Offset, group, later, earlier));
            }
        }

        int reads = 0;
        var buffer = new byte[EarlierEnd];
        ulong PieceAt(long at, ulong seed)
        {
            image.Read(at, buffer, 0, Piece);
            reads++;
            return SharedPieces.Hash(buffer.AsSpan(0, Piece), seed);
        }

        // An end is read once, however many files it's compared with.
        var ends = new Dictionary<(long, int), HashSet<ulong>>();
        HashSet<ulong> SpotsAt(long at, int length)
        {
            if (ends.TryGetValue((at, length), out var spots))
                return spots;
            image.Read(at, buffer, 0, length);
            reads++;
            spots = [];
            SharedPieces.Planner.SpotsIn(buffer.AsSpan(0, length), blockSize, spots);
            ends[(at, length)] = spots;
            return spots;
        }
        bool Alike(RomFsArchive.Entry later, RomFsArchive.Entry earlier, bool end)
        {
            int mine = (int)Math.Min(LaterEnd, later.Size), theirs = (int)Math.Min(EarlierEnd, earlier.Size);
            var spots = SpotsAt(end ? later.Offset + later.Size - mine : later.Offset, mine);
            if (spots.Count < FewestSpots)
                return false;
            var against = SpotsAt(end ? earlier.Offset + earlier.Size - theirs : earlier.Offset, theirs);
            return spots.Count(against.Contains) >= EndsAlike * spots.Count;
        }

        // Before every read, as one group of files the same size can take many: once the time's up, what's left is left out.
        bool OutOfTime() => reads >= FirstReads && clock.Elapsed.TotalSeconds > seconds;

        // Where each later copy is in the list, so a near copy that turns out to be a whole one is counted as that instead.
        var copied = new Dictionary<long, int>();
        foreach (var check in checks.OrderByDescending(c => c.Potential).ThenBy(c => c.Offset))
        {
            if (OutOfTime())
                break;
            if (check.Later is null)
            {
                // The same size, then the same 4 KB from the middle, then from the start and the end: a copy of the whole file.
                if (reads + check.Group.Count > mostReads)
                    continue;
                var middles = new List<(ulong Hash, RomFsArchive.Entry File)>();
                foreach (var file in check.Group)
                {
                    if (OutOfTime())
                        return copies;
                    middles.Add((PieceAt(file.Offset + file.Size / 2 / Piece * Piece, 0), file));
                }
                foreach (var alike in middles.GroupBy(m => m.Hash).Select(g => g.Select(m => m.File).ToList()).Where(g => g.Count > 1).ToList())
                {
                    if (reads + 2 * alike.Count > mostReads)
                        break;
                    var both = new List<(ulong Hash, RomFsArchive.Entry File)>();
                    foreach (var file in alike)
                    {
                        if (OutOfTime())
                            return copies;
                        both.Add((PieceAt(file.Offset, 0) ^ PieceAt(file.Offset + file.Size - Piece, 1), file));
                    }
                    foreach (var same in both.GroupBy(m => m.Hash).Select(g => g.Select(m => m.File).ToList()).Where(g => g.Count > 1).ToList())
                    {
                        foreach (var later in same.Skip(1))
                        {
                            var copy = new Copy(later.Offset, later.Size, Math.Max(0, later.Size - blockSize - MapBytes(repeats, blockSize, later)), false);
                            if (copied.TryGetValue(later.Offset, out int at))
                            {
                                copies[at] = copy;
                                continue;
                            }
                            copied[later.Offset] = copies.Count;
                            copies.Add(copy);
                        }
                    }
                }
            }
            else
            {
                // The same name, about the same size, and the end or the start much the same as the earlier one's: a near copy.
                var later = check.Later;
                var earlier = check.Earlier!;
                if (copied.ContainsKey(later.Offset) || reads + 4 > mostReads)
                    continue;
                if (!Alike(later, earlier, true) && (OutOfTime() || !Alike(later, earlier, false)))
                    continue;
                long held = (long)(NearHeld * Math.Min(later.Size, earlier.Size)) - blockSize - MapBytes(repeats, blockSize, later);
                copied[later.Offset] = copies.Count;
                copies.Add(new Copy(later.Offset, later.Size, Math.Max(0, held), true));
            }
        }
        return copies;
    }

    private static string NameOf(string path) => path[(path.LastIndexOf('/') + 1)..];

    /// <summary>What of a file the update's map already has held again: those places are stored once without any of this.</summary>
    private static long MapBytes(long[] repeats, int blockSize, RomFsArchive.Entry file)
    {
        long end = file.Offset + file.Size, total = 0;
        int i = Array.BinarySearch(repeats, file.Offset - blockSize + 1);
        for (i = i < 0 ? ~i : i; i < repeats.Length && repeats[i] < end; i++)
            total += Math.Min(repeats[i] + blockSize, end) - Math.Max(repeats[i], file.Offset);
        return total;
    }
}
