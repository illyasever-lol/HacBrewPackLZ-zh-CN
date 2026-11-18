using System.Buffers.Binary;
using System.Security.Cryptography;

namespace HacBrewPackLz;

internal enum SectionPlan
{
    CompressRomFs,
    RebuildRomFs,
    RebuildPfs0,
    PassThrough,
}

/// <summary>A file that can't be used, with the reason.</summary>
internal sealed record InputIssue(string Path, string Message)
{
    /// <summary>The files it's about: just the one, or, for several in a folder with the same problem, all of them, which go together.</summary>
    public string[] Paths { get; init; } = [Path];

    /// <summary>Turned up by going through a folder rather than added by itself, so it's left off the list unless that's asked for.</summary>
    public bool FoundInFolder { get; init; }
}

/// <summary>What's known of a title from the start of its file.</summary>
internal sealed record TitleSummary(ulong TitleId, byte Type, uint Version, ulong LinkedId, long Bytes, bool HoldsOnlyPart, string? Name, string? DisplayVersion, byte[]? Icon)
{
    /// <summary>Its game data is compressed the way this tool compresses it, which its headers say, whatever the file is called.</summary>
    public bool MadeHere { get; init; }

    /// <summary>What the game came to before, as a copy this tool made says. Zero if it doesn't say, as a copy from before 1.5 doesn't.</summary>
    public long OriginalBytes { get; init; }
}

/// <summary>Where one section of a title comes from: an update's section may still read from its base game.</summary>
internal readonly record struct SectionSource(NcaFile Owner, NcaFsSection Section, NcaFile? Original);

internal sealed class TitleItem
{
    public required GameArchive Archive { get; init; }
    public required ContentMeta Meta { get; init; }
    /// <summary>Where the content meta is, in the file.</summary>
    public required string MetaName { get; init; }
    /// <summary>What it's made with. If the content meta had to be worked out, that's the program instead.</summary>
    public required NcaFile MetaNca { get; init; }
}

/// <summary>One content of a title: an update's NCA takes over from the base game's, but may still read from it.</summary>
internal sealed record ContentPair(byte Type, byte IdOffset, NcaFile? Base, NcaFile? Patch)
{
    public NcaFile Primary => Patch ?? Base!;

    /// <summary>
    /// Section numbers in the order the sections lie in the file, which needn't be the order they're numbered in. Going
    /// through them like this means a solid NCZ gets read from front to back only once.
    /// </summary>
    public IEnumerable<int> SectionsInFileOrder() =>
        Enumerable.Range(0, Primary.Sections.Length).Where(i => SectionAt(i) is not null).OrderBy(i => SectionAt(i)!.Value.Section.Offset);

    public void ReleaseMemory()
    {
        Base?.ReleaseMemory();
        Patch?.ReleaseMemory();
    }

    public SectionSource? SectionAt(int index)
    {
        if (Patch?.Sections[index] is { } patched)
            return new SectionSource(Patch, patched, Base);
        if (Base?.Sections[index] is { } own)
            return new SectionSource(Base, own, null);
        return null;
    }
}

internal sealed class TitleJob
{
    /// <summary>It's come up: a game compressed on its own, with its original update installed beside it, doesn't open.</summary>
    public const string UpdatesWontWork = "在已压缩的游戏上直接安装官方更新补丁会导致游戏无法启动。要装新补丁，请把补丁和已压缩的游戏一起交给本程序，它们会重新合成一个标题。";

    public required TitleItem Main { get; init; }
    public TitleItem? Update { get; init; }
    public required List<ContentPair> Contents { get; init; }
    public required long InputBytes { get; init; }
    public string? TitleName { get; init; }

    /// <summary>The picture the Switch shows for it, as the JPEG it's kept as.</summary>
    public byte[]? Icon { get; init; }

    /// <summary>The version the game shows people, such as 1.0.10, from its control data: the update's when it has one.</summary>
    public string? DisplayVersion { get; init; }

    public bool IsBaseGame => Main.Meta.Type == Cnmt.MetaApplication;

    /// <summary>The game it's part of: itself for a base game, and the game a DLC is for.</summary>
    public ulong ApplicationId => IsBaseGame ? Main.Meta.TitleId : Main.Meta.LinkedId;

    /// <summary>Its game data is compressed the way this tool compresses it, which its headers say, whatever the file is called.</summary>
    public bool MadeHere { get; init; }

    /// <summary>With Game and DLC in one file on, DLC of its game are in the files that the file it's in doesn't hold, so that file is made
    /// again with them: a copy this tool made of the game, or DLC without their game that no one file holds together.</summary>
    public bool DlcNotInFile { get; init; }

    /// <summary>Something this tool made, which only gets made once there's nothing newer to make it from, or its DLC to put in its file.</summary>
    public bool IsOwnOutput => Update is null && MadeHere && !DlcNotInFile;

    /// <summary>What there is to know about it that isn't the title itself: other copies of it, and what was left aside.</summary>
    public List<string> Notes { get; } = [];

    public void ReleaseMemory() => Contents.ForEach(c => c.ReleaseMemory());

    public string DisplayName => TitleName ?? Path.GetFileNameWithoutExtension(Main.Archive.FilePath);

    public string Detail
    {
        get
        {
            var meta = Main.Meta;
            if (!IsBaseGame)
                return $"DLC 版本 {meta.Version}";
            // In the words the Switch app uses: the version people know the game by, and whether its update is in it.
            return Update is { } u
                ? MadeHere
                    ? "已压缩，并已并入更新补丁"
                    : $"版本 {Version}，含更新补丁"
                : $"版本 {Version}";
        }
    }

    /// <summary>The version people know the game by, or its number where its control data doesn't say.</summary>
    public string Version => DisplayVersion ?? $"v{(Update ?? Main).Meta.Version}";

    public string Describe() => $"{DisplayName} [{Main.Meta.TitleId:X16}]：{Detail}";

    public string Stem(uint version, bool sharedFile)
    {
        var meta = Main.Meta;
        if (IsBaseGame && TitleName is { } name)
            return $"{LocalNames.ForOutputName(name)} [{meta.TitleId:X16}][v{version}]";
        string stem = LocalNames.ForOutputName(Path.GetFileNameWithoutExtension(Main.Archive.FilePath));
        return sharedFile ? $"{stem} [{meta.TitleId:X16}]" : stem;
    }
}

/// <summary>Opens the input files and works out which titles they hold and which update goes with which base game.</summary>
internal sealed class TitleSet : IDisposable
{
    /// <summary>Whether this tool made each title, worked out once from its headers.</summary>
    private readonly Dictionary<TitleItem, bool> _madeHere = [];

    /// <summary>
    /// Whether this tool made a title: its game data has a compressed section with key generation 0, which its makers can't have made, as
    /// compression came with firmware 12. Its headers say, whatever the file is called. Worked out once for each.
    /// </summary>
    public bool IsMadeHere(TitleItem item)
    {
        lock (_madeHere)
        {
            if (_madeHere.TryGetValue(item, out bool known))
                return known;
        }
        bool made = false;
        try
        {
            foreach (var record in item.Meta.Records.Where(r => r.Type is Cnmt.ContentTypeProgram or Cnmt.ContentTypeData))
            {
                var nca = OpenContent(item, record);
                made = made || nca.Sections.Any(section => section is { HasCompression: true } && nca.IsRebuiltCompressed(section));
            }
        }
        catch (Exception e) when (e is ToolException or IOException or CryptographicException)
        {
            // What can't be read isn't something this made, as far as anything here can say. Any damage is reported when it's gone through.
        }
        lock (_madeHere)
            _madeHere[item] = made;
        return made;
    }

    /// <summary>A game this small with nothing else to it isn't a game, but something like a forwarder.</summary>
    private const long SmallGameBytes = 1 << 20;

    private readonly Keyset _keyset;
    private readonly TicketStore _tickets;
    private readonly List<GameArchive> _archives = [];
    private readonly List<TitleItem> _items = [];

    public List<TitleJob> Jobs { get; } = [];
    public List<InputIssue> Problems { get; } = [];
    public List<string> Notes { get; } = [];

    /// <summary>Game and DLC in one file: what a copy this tool made has to hold to be done.</summary>
    private bool _dlcInGameFile = true;

    /// <summary>The DLC of each game in the files, by title ID, at the newest version there.</summary>
    private readonly Dictionary<ulong, Dictionary<ulong, uint>> _dlcOf = [];

    /// <summary>Games given only as DLC, two or more, which one file holds together, and those no file does.</summary>
    private readonly HashSet<ulong> _dlcHeld = [], _dlcApart = [];

    /// <summary>DLC with no file of their own here, by title ID: no copy at the newest version is alone in its file or in a file this tool made,
    /// as with one only inside a merged original or another tool's DLC pack.</summary>
    private readonly HashSet<ulong> _noFile = [];

    private TitleSet(Keyset keyset)
    {
        _keyset = keyset;
        _tickets = new TicketStore(keyset);
    }

    /// <summary>DLC given without their game that one file holds together, made by this tool or with nothing to compress, so there's nothing
    /// to make of them.</summary>
    public bool DlcHeld(ulong game) => _dlcHeld.Contains(game);

    /// <summary>A DLC with no file of its own here. Beside this tool's copy of its game, it gets one with Game and DLC in one file off.</summary>
    public bool HasNoFileOfItsOwn(TitleJob dlc) => !dlc.IsBaseGame && _noFile.Contains(dlc.Main.Meta.TitleId);

    /// <summary>Whether the game's file holds this DLC, at least as new.</summary>
    public bool InFileOf(TitleJob game, TitleJob dlc) => _items.Any(i => i.Archive == game.Main.Archive && i.Meta.Type == Cnmt.MetaAddOnContent
        && i.Meta.TitleId == dlc.Main.Meta.TitleId && i.Meta.Version >= dlc.Main.Meta.Version);

    public bool IsInputPath(string path) =>
        _archives.Any(a => string.Equals(Path.GetFullPath(a.FilePath), Path.GetFullPath(path), StringComparison.OrdinalIgnoreCase));

    /// <summary>
    /// v19：这个游戏的所有输入文件，包括 Group() 只写进备注而没有放进 Jobs 的那些：较旧的更新补丁、
    /// 重复的 DLC、本程序先前生成的压缩副本。按 X 移除一行时必须连同它们一起放开。
    /// </summary>
    public string[] InputPathsFor(ulong applicationId) =>
        _items.Where(i => (i.Meta.Type == Cnmt.MetaApplication ? i.Meta.TitleId : i.Meta.LinkedId) == applicationId)
              .Select(i => i.Archive.FilePath)
              .Distinct(LocalNames.PathComparer)
              .ToArray();

    /// <summary>
    /// v20：上游 1.5.1 选中先前生成的压缩副本时会丢掉单独的 Update，但那份副本已把补丁并进去；
    /// 输入里还有补丁文件，或自制副本版本高于原始本体，都说明成品含更新补丁。
    /// </summary>
    public bool HasUpdateFor(ulong applicationId)
    {
        if (_items.Any(i => i.Meta.Type == Cnmt.MetaPatch && i.Meta.LinkedId == applicationId))
            return true;
        var applications = _items.Where(i => i.Meta.Type == Cnmt.MetaApplication && i.Meta.TitleId == applicationId).ToList();
        uint original = applications.Where(i => !IsMadeHere(i)).Select(i => i.Meta.Version).DefaultIfEmpty(0u).Min();
        return applications.Any(i => IsMadeHere(i) && i.Meta.Version > original);
    }

    public bool SharesFile(TitleJob job) => _items.Count(i => i.Archive == job.Main.Archive && i.Meta.Type != Cnmt.MetaPatch) > 1;

    /// <summary>Opens the files without putting their titles together into games, which is all <see cref="Summarize"/> needs. A lone update
    /// isn't a problem until it's to be done something with.</summary>
    public static TitleSet OpenOnly(IEnumerable<string> paths, Keyset keyset)
    {
        var set = new TitleSet(keyset);
        set.AddFiles(paths.ToList(), null);
        return set;
    }

    /// <summary>Opens every file. <paramref name="opened"/> hears how many of how many have been, as they are, from any thread.</summary>
    /// <param name="gameFolders">Where game folders found in the inputs go, as the command line builds them too; without it, they're searched.</param>
    /// <param name="dlcInGameFile">Game and DLC in one file, which decides which copy of a game is used.</param>
    public static TitleSet Load(IEnumerable<string> inputs, Keyset keyset, Action<int, int>? opened = null, CancellationToken token = default, List<string>? gameFolders = null,
        bool dlcInGameFile = true)
    {
        var set = new TitleSet(keyset) { _dlcInGameFile = dlcInGameFile };
        try
        {
            set.AddFiles(Expand(inputs, set.Problems, gameFolders, token), opened);
            set.Group();
        }
        catch
        {
            // Stopped, or worse: whatever was opened gets closed.
            set.Dispose();
            throw;
        }
        return set;
    }

    public void Dispose()
    {
        foreach (var archive in _archives)
            archive.Dispose();
    }

    /// <summary>Each title in the files, without going into any more of them than that takes. The same as the Switch's, which is what asks for it.</summary>
    public List<TitleSummary> Summarize()
    {
        var list = new List<TitleSummary>();
        foreach (var item in _items)
        {
            bool part = false;
            string? name = null, displayVersion = null;
            byte[]? icon = null;
            foreach (var record in item.Meta.Records)
            {
                try
                {
                    if (record.Type == Cnmt.ContentTypeProgram && item.Meta.Type == Cnmt.MetaApplication)
                        part = part || OpenContent(item, record).HoldsOnlyPart;
                    else if (record.Type == Cnmt.ContentTypeControl)
                        (name, icon, displayVersion) = ReadControl([new ContentPair(record.Type, record.IdOffset, OpenContent(item, record), null)]);
                }
                catch (Exception e) when (e is ToolException or IOException or CryptographicException)
                {
                    // A name and an icon are only niceties. Any real damage gets reported when the title itself is gone through.
                }
            }
            long original = 0;
            try
            {
                var control = item.Meta.Records.FirstOrDefault(r => r.Type == Cnmt.ContentTypeControl);
                if (control is not null)
                    original = CopyMark.InControl(OpenContent(item, control))?.OriginalBytes ?? 0;
            }
            catch (Exception e) when (e is ToolException or IOException or CryptographicException)
            {
            }
            list.Add(new TitleSummary(item.Meta.TitleId, item.Meta.Type, item.Meta.Version, item.Meta.LinkedId, InputBytesOf(item, null), part, name, displayVersion, icon)
                { MadeHere = IsMadeHere(item), OriginalBytes = original });
        }
        return list;
    }

    /// <summary>Applies --only and --skip, which go by title ID or part of the name.</summary>
    public void Filter(Options options)
    {
        static bool Matches(TitleJob job, List<string> patterns) => patterns.Any(p =>
            p.Equals($"{job.Main.Meta.TitleId:X16}", StringComparison.OrdinalIgnoreCase) || job.DisplayName.Contains(p, StringComparison.OrdinalIgnoreCase));

        int left = Jobs.RemoveAll(job => (options.OnlyTitles.Count > 0 && !Matches(job, options.OnlyTitles)) || Matches(job, options.SkipTitles));
        if (left > 0)
            Notes.Add($"因 --only / --skip 排除 {left} 个标题");
        int small = LeaveOutSmall(options);
        if (small > 0)
            Notes.Add($"因 --hidesmall 排除 {small} 个小于 1 MB 的游戏");
    }

    /// <summary>Applies --hidesmall, and says how many it left out. An update or a DLC can be small and still be what's wanted, and so can a game
    /// that has DLC.</summary>
    public int LeaveOutSmall(Options options) => options.HideSmall ? Jobs.RemoveAll(SmallGame().Invoke) : 0;

    /// <summary>How many games --hidesmall would leave out, while it isn't, so they can be hidden again.</summary>
    public int CountSmall() => Jobs.Count(SmallGame());

    private Func<TitleJob, bool> SmallGame()
    {
        var withDlc = Jobs.Where(j => !j.IsBaseGame).Select(j => j.ApplicationId).ToHashSet();
        return job => job.IsBaseGame && job.Update is null && job.InputBytes < SmallGameBytes && !withDlc.Contains(job.ApplicationId);
    }

    /// <summary>Turns files and folders into a list of files, searching folders and subfolders for game files. Given somewhere to put them, game
    /// folders go there instead of being searched.</summary>
    public static List<string> Expand(IEnumerable<string> inputs, List<InputIssue> problems, List<string>? gameFolders = null, CancellationToken token = default)
    {
        var files = new List<string>();
        var seen = new HashSet<string>(LocalNames.PathComparer);
        void Add(string path)
        {
            if (seen.Add(Path.GetFullPath(path)))
                files.Add(path);
        }

        foreach (string input in inputs)
        {
            if (Directory.Exists(input))
            {
                var folders = gameFolders is null ? [] : GameFolder.FindIn(input, token);
                foreach (string folder in folders)
                {
                    if (!gameFolders!.Contains(folder, LocalNames.PathComparer))
                        gameFolders!.Add(folder);
                }
                var everywhere = new EnumerationOptions { RecurseSubdirectories = true, IgnoreInaccessible = true, MatchCasing = MatchCasing.CaseInsensitive, AttributesToSkip = 0 };
                var found = Directory.EnumerateFiles(input, "*", everywhere)
                    .Where(p =>
                    {
                        token.ThrowIfCancellationRequested();
                        // What's been saved from inside games is a few of their parts, never a game.
                        return GameArchive.IsGameFile(p) && !GameInside.IsInFilesFromGames(Path.GetRelativePath(input, p));
                    })
                    .Where(p => !folders.Any(f => p.StartsWith(f + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase)))
                    .OrderBy(p => p, StringComparer.OrdinalIgnoreCase).ToList();
                // What this tool already made is read too: when it's as new as what it was made from, the title is done.
                found.ForEach(Add);
            }
            else if (!File.Exists(input))
            {
                problems.Add(new InputIssue(input, $"'{input}' not found"));
            }
            else
            {
                Add(input);
            }
        }
        return files;
    }

    private sealed class OpenedFile
    {
        public GameArchive? Archive;
        public readonly List<TitleItem> Items = [];
        public readonly List<InputIssue> Problems = [];
    }

    /// <summary>Anything unexpected comes out as itself, as it would have one at a time.</summary>
    private static void Together(int count, Action<int> work)
    {
        try
        {
            Parallel.For(0, count, work);
        }
        catch (AggregateException e) when (e.InnerException is not null)
        {
            System.Runtime.ExceptionServices.ExceptionDispatchInfo.Capture(e.InnerException).Throw();
        }
    }

    /// <summary>Several at a time, as each only takes a few small reads and a little work. What comes of them goes in the order they were given.</summary>
    private void AddFiles(List<string> paths, Action<int, int>? progress)
    {
        var opened = new OpenedFile[paths.Count];
        int done = 0;
        progress?.Invoke(0, paths.Count);
        try
        {
            Together(paths.Count, i =>
            {
                opened[i] = OpenFile(paths[i]);
                progress?.Invoke(Interlocked.Increment(ref done), paths.Count);
            });
        }
        catch
        {
            // Stopped part way: what's open of it is closed, as it never got as far as the set.
            foreach (var file in opened)
                file?.Archive?.Dispose();
            throw;
        }
        foreach (var file in opened)
        {
            if (file.Archive is not null)
                _archives.Add(file.Archive);
            _items.AddRange(file.Items);
            Problems.AddRange(file.Problems);
        }
    }

    private OpenedFile OpenFile(string path)
    {
        var opened = new OpenedFile();
        try
        {
            var nsp = new GameArchive(path);
            opened.Archive = nsp;
            _tickets.AddFrom(nsp);

            var metas = nsp.Entries.Where(e => e.Name.EndsWith(".cnmt.nca", StringComparison.OrdinalIgnoreCase)).ToList();
            if (metas.Count == 0)
                opened.Problems.Add(new InputIssue(path, $"'{Path.GetFileName(path)}' has no content meta, so it isn't an installable title"));
            foreach (var entry in metas)
            {
                if (entry.Size < 0xC00)
                {
                    opened.Items.Add(ItemFromContents(nsp, entry));
                    continue;
                }
                var nca = new NcaFile(nsp.Open(entry), entry.Name, _keyset, _tickets);
                var section = nca.Sections.FirstOrDefault(s => s?.FsType == NcaFsSection.FsPartition)
                    ?? throw new ToolException($"{entry.Name} has no PFS0 section");
                var pfs = nca.OpenPfs0(section, null);
                var cnmt = pfs.Files.FirstOrDefault(f => f.Name.EndsWith(".cnmt", StringComparison.OrdinalIgnoreCase))
                    ?? throw new ToolException($"{entry.Name} holds no .cnmt file");
                opened.Items.Add(new TitleItem { Archive = nsp, MetaName = entry.Name, MetaNca = nca, Meta = ContentMeta.Parse(pfs.ReadAll(cnmt), entry.Name) });
            }
        }
        catch (Exception e) when (e is ToolException or IOException or UnauthorizedAccessException or CryptographicException)
        {
            opened.Problems.Add(new InputIssue(path, $"'{Path.GetFileName(path)}': {Help.WithNextStep(e.Message)}"));
        }
        return opened;
    }

    /// <summary>Sphaira's forwarders have the wrong size recorded for their content meta, so that a dump of one only holds the start of it. All that
    /// was in it can be told from a program and its control data, as it's the same for all of them.</summary>
    private TitleItem ItemFromContents(GameArchive nsp, GameArchive.Entry metaEntry)
    {
        string why = $"{metaEntry.Name} 只有 {metaEntry.Size} 字节，而一个 NCA 至少需要 3 KB";
        const int HeaderSize = 0x20, ExtendedSize = 0x10, RecordSize = 0x38, DigestSize = 0x20, Parts = 2;
        NcaFile? program = null;
        var entries = new GameArchive.Entry?[Parts];
        ulong id = 0;
        foreach (var entry in nsp.Entries)
        {
            if (entry.Name == metaEntry.Name || entry.Name.EndsWith(".tik", StringComparison.OrdinalIgnoreCase) || entry.Name.EndsWith(".cert", StringComparison.OrdinalIgnoreCase))
                continue;
            if (!entry.Name.EndsWith(".nca", StringComparison.OrdinalIgnoreCase))
                throw new ToolException(why);
            var nca = new NcaFile(nsp.Open(entry), entry.Name, _keyset, _tickets);
            int part = nca.ContentType == NcaContentType.Program ? 0 : nca.ContentType == NcaContentType.Control ? 1 : -1;
            if (part < 0 || entries[part] is not null || (id != 0 && id != nca.ProgramId))
                throw new ToolException(why);
            entries[part] = entry;
            id = nca.ProgramId;
            if (part == 0)
                program = nca;
        }
        if (entries[0] is null || entries[1] is null || program is null)
            throw new ToolException(why);

        var raw = new byte[HeaderSize + ExtendedSize + Parts * RecordSize + DigestSize];
        var r = raw.AsSpan();
        BinaryPrimitives.WriteUInt64LittleEndian(r, id);
        r[0x0C] = Cnmt.MetaApplication;
        BinaryPrimitives.WriteUInt16LittleEndian(r[0x0E..], ExtendedSize);
        BinaryPrimitives.WriteUInt16LittleEndian(r[0x10..], Parts);
        BinaryPrimitives.WriteUInt64LittleEndian(r[HeaderSize..], id | 0x800);
        for (int part = 0; part < Parts; part++)
        {
            var e = r.Slice(HeaderSize + ExtendedSize + part * RecordSize, RecordSize);
            var entry = entries[part]!;
            string hex = entry.Name[..^4];
            if (hex.Length != 32 || !hex.All(Uri.IsHexDigit))
                throw new ToolException(why);
            Convert.FromHexString(hex).CopyTo(e[0x20..]);
            for (int b = 0; b < 6; b++)
                e[0x30 + b] = (byte)(entry.Size >> (8 * b));
            e[0x36] = part == 0 ? Cnmt.ContentTypeProgram : Cnmt.ContentTypeControl;
        }
        return new TitleItem { Archive = nsp, MetaName = metaEntry.Name, MetaNca = program, Meta = ContentMeta.Parse(raw, metaEntry.Name) };
    }

    /// <summary>
    /// One job for each base game, with its newest update, and for each DLC, in title ID order. Of several copies of the same one, the newest is
    /// used, and the others are noted on the job. What this tool already compressed takes the place of what it was made from when it's as new, so
    /// it isn't done twice. With Game and DLC in one file on, a copy that holds the game's DLC goes before one that doesn't, and one that doesn't
    /// is made again with them. It gives way to the original, unless it's newer, when one of them has no file of its own.
    /// </summary>
    private void Group()
    {
        // Each DLC whose contents open, as making it opens them, and whether it has nothing to compress. One that can't be made isn't looked
        // for in a game's file, or that file would be made again every time.
        var plain = new Dictionary<TitleItem, bool>();
        var compressing = new Options();
        foreach (var dlc in _items.Where(i => i.Meta.Type == Cnmt.MetaAddOnContent))
        {
            try
            {
                plain[dlc] = Analyzer.AllCompressed(dlc.Meta.Records.Where(IsCopied).Select(r => new ContentPair(r.Type, r.IdOffset, OpenContent(dlc, r), null)).ToList(), compressing);
            }
            catch (Exception e) when (e is ToolException or IOException or UnauthorizedAccessException or CryptographicException)
            {
                continue;
            }
            if (!_dlcOf.TryGetValue(dlc.Meta.LinkedId, out var of))
                _dlcOf[dlc.Meta.LinkedId] = of = [];
            of[dlc.Meta.TitleId] = Math.Max(of.GetValueOrDefault(dlc.Meta.TitleId), dlc.Meta.Version);
        }
        // Only headers say what this tool made, so a file of DLC it put together with nothing to compress counts as no file of their own: each
        // gets one once with the setting off, or the copy gives way to its original once with it on, and after that they have one.
        bool OwnFile(TitleItem copy) => _items.All(i => i.Archive != copy.Archive || i == copy) || _items.Any(i => i.Archive == copy.Archive && IsMadeHere(i));
        foreach (var (id, newest) in _dlcOf.Values.SelectMany(dlc => dlc))
        {
            if (!_items.Any(i => i.Meta.Type == Cnmt.MetaAddOnContent && i.Meta.TitleId == id && i.Meta.Version >= newest && OwnFile(i)))
                _noFile.Add(id);
        }
        // A file holds a game's DLC together when it has each at least as new, and this tool made something in it, or none of its DLC has
        // anything to compress, as they go into a file as they are.
        bool HoldsAll(GameArchive archive, ulong game) => HoldsDlc(archive, game) && (_items.Any(i => i.Archive == archive && IsMadeHere(i))
            || _items.Where(i => i.Archive == archive && i.Meta.Type == Cnmt.MetaAddOnContent).All(i => plain.GetValueOrDefault(i)));
        // DLC without their game go together into one file, which is done once a file holds them all.
        foreach (ulong game in _dlcOf.Where(g => _dlcInGameFile && g.Value.Count > 1).Select(g => g.Key)
                     .Where(game => !_items.Any(i => i.Meta.Type == Cnmt.MetaApplication && i.Meta.TitleId == game)))
        {
            bool held = _items.Where(i => i.Meta.Type == Cnmt.MetaAddOnContent && i.Meta.LinkedId == game).Select(i => i.Archive).Distinct().Any(a => HoldsAll(a, game));
            (held ? _dlcHeld : _dlcApart).Add(game);
        }

        var updates = _items.Where(i => i.Meta.Type == Cnmt.MetaPatch).ToList();
        var used = new HashSet<TitleItem>();
        var wanted = new List<(TitleItem Main, TitleItem? Update, List<string> Notes)>();
        // Compressed copies given with a newer update, which only go together if the update turns out to fit, and the update that's
        // in the copy already, when that's here too.
        var tryFit = new Dictionary<int, TitleItem?>();

        foreach (var group in _items.Where(i => i.Meta.Type == Cnmt.MetaApplication).GroupBy(i => i.Meta.TitleId))
        {
            var notes = new List<string>();
            var candidates = updates.Where(u => u.Meta.LinkedId == group.Key).ToList();
            used.UnionWith(candidates);
            var originals = group.Where(i => !IsMadeHere(i)).ToList();
            TitleItem? main = null, update = null;
            if (originals.Count > 0)
            {
                main = NewestAndNoteOthers(originals, "本体游戏", notes);
                if (candidates.Count > 0)
                    update = NewestAndNoteOthers(candidates, "update", notes);
                if (update is not null && !UpdateGoesWith(update, main))
                {
                    notes.Add($"文件 '{Path.GetFileName(update.Archive.FilePath)}' 中的更新补丁 v{update.Meta.Version} 已忽略：本体游戏已是 v{main.Meta.Version}");
                    update = null;
                }
            }
            else if (candidates.Count > 0)
            {
                // A compressed copy this made holds the original game's data, so a newer update can be made part of it, as of the original.
                var copy = group.OrderByDescending(i => i.Meta.Version).ThenBy(LacksDlc).First();
                // What's said of the other updates only goes with the newest being made part of it. Otherwise each is noted once below.
                var others = new List<string>();
                var newest = NewestAndNoteOthers(candidates, "update", others);
                if (newest.Meta.Version > copy.Meta.Version)
                {
                    notes.AddRange(others);
                    foreach (var other in group.Where(i => i != copy))
                    {
                        notes.Add(other.Meta.Version == copy.Meta.Version
                            ? $"另有相同的已压缩副本 v{other.Meta.Version}：'{Path.GetFileName(other.Archive.FilePath)}'"
                            : $"较旧的已压缩副本 v{other.Meta.Version}：'{Path.GetFileName(other.Archive.FilePath)}'");
                    }
                    tryFit[wanted.Count] = candidates.FirstOrDefault(u => u.Meta.Version == copy.Meta.Version);
                    wanted.Add((copy, newest, notes));
                    continue;
                }
                notes.AddRange(candidates.Select(u => $"更新补丁 v{u.Meta.Version} 位于 '{Path.GetFileName(u.Archive.FilePath)}'"));
            }
            wanted.Add(PreferCompressed(group, main, update, notes));
        }

        // In title ID order, so the first of a game's DLC is the one a file of them is named after.
        foreach (var group in _items.Where(i => i.Meta.Type == Cnmt.MetaAddOnContent).GroupBy(i => i.Meta.TitleId).OrderBy(g => g.Key))
        {
            var notes = new List<string>();
            var originals = group.Where(i => !IsMadeHere(i)).ToList();
            // A DLC in a file that holds its game's DLC together, as one with nothing to compress is passed through into it, goes before the
            // same DLC in a file of its own.
            var main = originals.Count > 0 ? NewestAndNoteOthers(originals, "DLC", notes, _dlcInGameFile ? i => HoldsAll(i.Archive, i.Meta.LinkedId) : null) : null;
            wanted.Add(PreferCompressed(group, main, null, notes));
        }

        // Several at a time here too, and in the same order however quickly each one comes.
        var made = new (TitleJob? Job, InputIssue? Problem)[wanted.Count];
        Together(wanted.Count, i => made[i] = tryFit.TryGetValue(i, out var earlier)
            ? FitUpdate(wanted[i].Main, wanted[i].Update!, earlier, wanted[i].Notes)
            : MakeJob(wanted[i].Main, wanted[i].Update));
        for (int i = 0; i < made.Length; i++)
        {
            var (job, problem) = made[i];
            if (job is not null)
            {
                job.Notes.AddRange(wanted[i].Notes);
                Jobs.Add(job);
            }
            if (problem is not null)
                Problems.Add(problem);
        }

        foreach (var update in updates.Where(u => !used.Contains(u)))
        {
            Problems.Add(new InputIssue(update.Archive.FilePath, $"「{Path.GetFileName(update.Archive.FilePath)}」是更新补丁 (v{update.Meta.Version})，但没有找到它的本体游戏 {update.Meta.LinkedId:X16}。" +
                         "升级补丁不能单独重建：请把本体游戏加入同一批处理"));
        }

        foreach (var other in _items.Where(i => i.Meta.Type is not (Cnmt.MetaApplication or Cnmt.MetaPatch or Cnmt.MetaAddOnContent)))
            Problems.Add(new InputIssue(other.Archive.FilePath, $"「{Path.GetFileName(other.Archive.FilePath)}」的内容元数据类型为 0x{other.Meta.Type:x2}，暂不支持"));
    }

    /// <summary>
    /// A compressed copy given with a newer update: the two made one title if the update fits the copy, which its own hashes say, read over
    /// it. Otherwise the copy is listed by itself, and the update says why it can't be made part of it: that it doesn't fit, or whatever else
    /// stopped it, such as a file that can't be read or a key that's missing, as it's said anywhere else.
    /// </summary>
    private (TitleJob? Job, InputIssue? Problem) FitUpdate(TitleItem main, TitleItem update, TitleItem? earlier, List<string> notes)
    {
        string copyName = Path.GetFileName(main.Archive.FilePath), updateName = Path.GetFileName(update.Archive.FilePath);
        string why;
        bool mapped = false;
        try
        {
            var job = BuildJob(main, update);
            mapped = KeepOriginal(job, earlier);
            CheckUpdateFits(job);
            notes.Insert(0, $"以 '{copyName}' 中的已压缩副本替代原始游戏，并把更新补丁 v{update.Meta.Version} 并入");
            return (job, null);
        }
        catch (MismatchException)
        {
            // Most likely an update was made part of it already, and the new one is for parts of the original that it took the place of. With no
            // map of what that update kept, its file says; with one, only the original game can make up for it.
            string give = main.Meta.Version > 0 && !mapped
                ? $"其中已含更新补丁 v{main.Meta.Version}，请把该补丁文件或原始游戏一并加入"
                : "请把原始游戏一并加入，它们会被一起压缩";
            why = $"'{updateName}'（v{update.Meta.Version}）需要原始游戏中的部分数据，而 '{copyName}' 里的已压缩副本没有这些数据。{give}";
        }
        catch (Exception e) when (e is ToolException or IOException or UnauthorizedAccessException or CryptographicException)
        {
            why = $"'{updateName}'（v{update.Meta.Version}）无法并入 '{copyName}' 中的已压缩游戏：{Help.WithNextStep(e.Message)}";
        }
        // A copy that can't be read itself says so, as any other file does: the update can't go anywhere without it.
        var (alone, problem) = MakeJob(main, null);
        return alone is null ? (null, problem) : (alone, new InputIssue(update.Archive.FilePath, why));
    }

    /// <summary>
    /// For a copy with an update in it already, where that update kept each part of the original, which is what a newer one reads: from
    /// the update itself if it's here, or from the map the copy was made with. False if neither says.
    /// </summary>
    private bool KeepOriginal(TitleJob job, TitleItem? earlier)
    {
        var map = (earlier is not null ? MapOf(earlier) : null) ?? UpdateMap.In(job);
        if (map is null)
            return false;
        foreach (var section in map.Sections)
        {
            foreach (var content in job.Contents.Where(c => c.Type == section.ContentType && c.IdOffset == section.IdOffset && c.Base is not null))
                content.Base!.KeptOriginal.Add(section);
        }
        return true;
    }

    /// <summary>
    /// The same for any game given with an update, as the Switch does it. An update is always made against the game as it first came out,
    /// and has a higher version, so a game given with an update of its own version has that update in it already, as a copy made with
    /// compression off has, unless it was cut down to go with it. A newer update then reads the parts of the original that one kept where
    /// it put them, known from its file or from the map the copy was made with. The original game has neither, and nothing changes. What
    /// can't be read is passed over, and the update's own hashes say whether it fits when it's read.
    /// </summary>
    private void KeepOriginalIfKnown(TitleJob job, TitleItem main, TitleItem update)
    {
        try
        {
            if (HoldsOnlyPart(main))
                return;
            var earlier = _items.FirstOrDefault(i => i != update && i.Meta.Type == Cnmt.MetaPatch && i.Meta.LinkedId == main.Meta.TitleId && i.Meta.Version == main.Meta.Version);
            KeepOriginal(job, earlier);
        }
        catch (Exception e) when (e is ToolException or IOException or UnauthorizedAccessException or CryptographicException)
        {
        }
    }

    /// <summary>A base game cut down to what its update reads from it, which has the same version as that update.</summary>
    private bool HoldsOnlyPart(TitleItem main) =>
        main.Meta.Records.Where(r => r.Type == Cnmt.ContentTypeProgram).Any(r => OpenContent(main, r).HoldsOnlyPart);

    /// <summary>What an update kept of the original, from its own tables.</summary>
    private UpdateMap? MapOf(TitleItem update)
    {
        var sections = new List<UpdateMap.Section>();
        foreach (var record in update.Meta.Records.Where(IsCopied))
        {
            var nca = OpenContent(update, record);
            foreach (var section in nca.Sections)
            {
                if (section is not null && section.HasIndirect && section.FsType == NcaFsSection.FsRomFs)
                    sections.Add(new UpdateMap.Section(record.Type, record.IdOffset, section.Index, nca.KeptInImage(section, null)));
            }
        }
        return sections.Count > 0 ? new UpdateMap { Version = update.Meta.Version, Sections = sections } : null;
    }

    /// <summary>
    /// Whether an update reads true over what it's given as its base game, by reading some of the game data it makes, spread across
    /// it, with the hash checks on. A compressed copy with an earlier update in it doesn't hold all of the original that a newer one
    /// reads, and that shows up here as a <see cref="MismatchException"/>. Anything else that goes wrong, such as a file that can't be read,
    /// is thrown as it is. Anything that slips through is still caught by the same checks when it's compressed.
    /// </summary>
    private static void CheckUpdateFits(TitleJob job)
    {
        const int Samples = 32;
        foreach (var content in job.Contents.Where(c => c.Base is not null && c.Patch is not null))
        {
            foreach (var section in content.Patch!.Sections)
            {
                if (section is null || !section.HasIndirect || section.FsType != NcaFsSection.FsRomFs || content.Base!.Sections[section.Index] is null)
                    continue;
                var image = content.Patch.OpenRomFsImage(section, content.Base, verify: true);
                try
                {
                    var buffer = new byte[IvfcBuilder.BlockSize];
                    for (int i = 0; i < Samples; i++)
                    {
                        long at = image.Length * i / Samples / IvfcBuilder.BlockSize * IvfcBuilder.BlockSize;
                        image.Read(at, buffer, 0, (int)Math.Min(buffer.Length, image.Length - at));
                    }
                }
                finally
                {
                    image.Release();
                }
            }
        }
    }

    /// <summary>The newest compressed copy, if it's at least as new as what there is to make one from, and of those one that holds the game's DLC.
    /// One that gives way to its original (<see cref="GivesWay"/>) goes only when it's newer. Otherwise what to make one from.</summary>
    private (TitleItem Main, TitleItem? Update, List<string> Notes) PreferCompressed(IEnumerable<TitleItem> group, TitleItem? main, TitleItem? update, List<string> notes)
    {
        var compressed = group.Where(IsMadeHere).OrderByDescending(i => i.Meta.Version).ThenBy(LacksDlc).ToList();
        uint madeFrom = Math.Max(main?.Meta.Version ?? 0, update?.Meta.Version ?? 0);
        if (compressed.Count > 0 && (main is null || compressed[0].Meta.Version > madeFrom || (compressed[0].Meta.Version == madeFrom && !GivesWay(compressed[0]))))
        {
            foreach (var older in compressed.Skip(1))
                notes.Add($"另有相同的已压缩副本 v{older.Meta.Version}：'{Path.GetFileName(older.Archive.FilePath)}'");
            if (main is not null)
            {
                string from = update is null ? $"'{Path.GetFileName(main.Archive.FilePath)}'" : $"'{Path.GetFileName(main.Archive.FilePath)}' and '{Path.GetFileName(update.Archive.FilePath)}'";
                notes.Insert(0, $"由 {from} 压缩而来，这些文件也在当前目录中");
            }
            return (compressed[0], null, notes);
        }
        foreach (var older in compressed)
        {
            notes.Add(older.Meta.Version < madeFrom
                ? $"较旧的已压缩副本 v{older.Meta.Version}：'{Path.GetFileName(older.Archive.FilePath)}'。再次压缩会生成 v{madeFrom} 的副本"
                : $"缺少部分 DLC 的已压缩副本位于「{Path.GetFileName(older.Archive.FilePath)}」。再次压缩会生成包含全部 DLC 的副本");
        }
        return (main!, update, notes);
    }

    /// <summary>Whether a file holds every DLC of this game in the files, each at least as new.</summary>
    private bool HoldsDlc(GameArchive archive, ulong game) => !_dlcOf.TryGetValue(game, out var dlc) || dlc.All(d =>
        _items.Any(i => i.Archive == archive && i.Meta.Type == Cnmt.MetaAddOnContent && i.Meta.TitleId == d.Key && i.Meta.Version >= d.Value));

    /// <summary>A copy this tool made of a game, without DLC of it that's in the files, while they go in one file.</summary>
    private bool LacksDlc(TitleItem copy) =>
        _dlcInGameFile && copy.Meta.Type == Cnmt.MetaApplication && IsMadeHere(copy) && !HoldsDlc(copy.Archive, copy.Meta.TitleId);

    /// <summary>A copy without a DLC of its game that has no file of its own here, which only its original, given too, can put in the new file.
    /// One whose DLC all have a file of their own, as 1.5.0 left them, is kept and made again with them.</summary>
    private bool GivesWay(TitleItem copy) => LacksDlc(copy) && _dlcOf[copy.Meta.TitleId].Keys.Any(_noFile.Contains);

    /// <summary>An update is newer than the base game it's for, unless that base game was cut down to go with it, in which case they have the same version.</summary>
    private bool UpdateGoesWith(TitleItem update, TitleItem main)
    {
        if (update.Meta.Version != main.Meta.Version)
            return update.Meta.Version > main.Meta.Version;
        try
        {
            return HoldsOnlyPart(main);
        }
        catch (ToolException)
        {
            return false;
        }
    }

    /// <summary>The newest, and of copies just as new, one that <paramref name="first"/> says goes first, if it's given, and then one that isn't
    /// packed with NCZ, as that takes longer to read.</summary>
    private static TitleItem NewestAndNoteOthers(IEnumerable<TitleItem> items, string kind, List<string> notes, Func<TitleItem, bool>? first = null)
    {
        var ordered = items.OrderByDescending(i => i.Meta.Version).ThenBy(i => first is not null && first(i) ? 0 : 1)
            .ThenBy(i => Path.GetExtension(i.Archive.FilePath).ToLowerInvariant() is ".nsz" or ".xcz" ? 1 : 0).ToList();
        foreach (var other in ordered.Skip(1))
        {
            string where = Path.GetFileName(other.Archive.FilePath);
            notes.Add(other.Meta.Version == ordered[0].Meta.Version
                ? $"另有相同的{kind}位于 '{where}'"
                : $"较旧的{kind} v{other.Meta.Version} 位于 '{where}'");
        }
        return ordered[0];
    }

    private (TitleJob? Job, InputIssue? Problem) MakeJob(TitleItem main, TitleItem? update)
    {
        try
        {
            var job = BuildJob(main, update);
            if (update is not null)
                KeepOriginalIfKnown(job, main, update);
            return (job, null);
        }
        catch (Exception e) when (e is ToolException or IOException or UnauthorizedAccessException or CryptographicException)
        {
            return (null, new InputIssue(main.Archive.FilePath, $"'{Path.GetFileName(main.Archive.FilePath)}': {Help.WithNextStep(e.Message)}"));
        }
    }

    /// <summary>The job, or what's wrong with it, thrown.</summary>
    private TitleJob BuildJob(TitleItem main, TitleItem? update)
    {
        var contents = new List<ContentPair>();
        foreach (var record in main.Meta.Records.Where(IsCopied))
            contents.Add(new ContentPair(record.Type, record.IdOffset, OpenContent(main, record), null));

        if (update is not null)
        {
            foreach (var record in update.Meta.Records.Where(IsCopied))
            {
                var patch = OpenContent(update, record);
                int index = contents.FindIndex(c => c.Type == record.Type && c.IdOffset == record.IdOffset);
                if (index >= 0)
                    contents[index] = contents[index] with { Patch = patch };
                else
                    contents.Add(new ContentPair(record.Type, record.IdOffset, null, patch));
            }
        }

        // Whatever an update doesn't take over has to be all there.
        foreach (var content in contents)
        {
            for (int i = 0; i < 4; i++)
            {
                if (content.SectionAt(i) is { } source && source.Section.HasSparse)
                    throw new ToolException("这只包含游戏的一部分，其余内容在升级补丁里，两者缺一不可");
            }
        }

        var (name, icon, displayVersion) = ReadControl(contents);
        return new TitleJob { Main = main, Update = update, Contents = contents, InputBytes = InputBytesOf(main, update), TitleName = name, Icon = icon,
            DisplayVersion = displayVersion is { Length: > 0 } ? displayVersion : null, MadeHere = IsMadeHere(main),
            DlcNotInFile = update is null && (LacksDlc(main) || (main.Meta.Type == Cnmt.MetaAddOnContent && _dlcApart.Contains(main.Meta.LinkedId))) };
    }

    /// <summary>The size of the files a title came from, or if a file holds other titles too, just this title's share of it.</summary>
    private long InputBytesOf(TitleItem main, TitleItem? update)
    {
        long total = 0;
        var items = update is null ? [main] : new[] { main, update };
        foreach (var archive in items.Select(i => i.Archive).Distinct())
        {
            if (!_items.Any(i => i.Archive == archive && i != main && i != update))
            {
                total += archive.Length;
                continue;
            }
            var mine = items.Where(i => i.Archive == archive).SelectMany(i => i.Meta.Records).Select(r => r.Id);
            total += archive.Entries.Where(e => mine.Contains(Path.GetFileNameWithoutExtension(e.Name), StringComparer.OrdinalIgnoreCase)).Sum(e => e.Size);
            total += items.Where(i => i.Archive == archive).Sum(i => archive.Find(i.MetaName)?.Size ?? 0);
        }
        return total;
    }

    private static bool IsCopied(ContentMetaRecord record) =>
        record.Type is not (Cnmt.ContentTypeMeta or Cnmt.ContentTypeDeltaFragment);

    private NcaFile OpenContent(TitleItem item, ContentMetaRecord record)
    {
        string name = record.Id + ".nca";
        if (item.Archive.Find(name) is { } entry)
            return new NcaFile(item.Archive.Open(entry), name, _keyset, _tickets);

        string compressed = record.Id + ".ncz";
        if (item.Archive.Find(compressed) is not { } packed)
            throw new ToolException($"'{Path.GetFileName(item.Archive.FilePath)}' 缺少内容元数据中列出的 {name}");
        var source = new NczSource(item.Archive.Open(packed), compressed);
        if (source.Length != record.Size)
            throw new ToolException($"{compressed} would decompress to {source.Length} bytes, but its content meta says {record.Size}. The file is damaged");
        return new NcaFile(source, name, _keyset, _tickets);
    }

    /// <summary>What a title's called, its icon and the version people know it by, from its control NCA.</summary>
    private static (string? name, byte[]? icon, string? displayVersion) ReadControl(List<ContentPair> contents)
    {
        string? name = null, displayVersion = null;
        byte[]? icon = null;
        try
        {
            var control = contents.FirstOrDefault(c => c.Type == Cnmt.ContentTypeControl);
            if (control is null)
                return (null, null, null);
            var nca = control.Primary;
            var section = nca.Sections.FirstOrDefault(s => s?.FsType == NcaFsSection.FsRomFs);
            if (section is null)
                return (null, null, null);

            // It only says what a title's called and what it looks like, which isn't worth reading all of the rest to check.
            var image = nca.OpenRomFsImage(section, control.Patch is not null ? control.Base : null, verify: false);
            var files = RomFsArchive.Open(image, nca.Name).Files;
            if (files.FirstOrDefault(f => f.Path == "control.nacp") is { } nacp)
            {
                var bytes = new byte[Math.Min(nacp.Size, Nacp.Size)];
                image.Read(nacp.Offset, bytes, 0, bytes.Length);
                name = Nacp.Titles(bytes).Select(t => t.Name).FirstOrDefault(n => n.Length > 0);
                displayVersion = Nacp.DisplayVersion(bytes);
            }
            const long LargestIcon = 1 << 20;
            var picture = files.FirstOrDefault(f => f.Path == "icon_AmericanEnglish.dat") ?? files.FirstOrDefault(f => f.Path.StartsWith("icon_", StringComparison.Ordinal));
            if (picture is { Size: > 0 and <= LargestIcon })
            {
                icon = new byte[picture.Size];
                image.Read(picture.Offset, icon, 0, icon.Length);
            }
        }
        catch (ToolException)
        {
            // A name and an icon are only niceties. Any real damage gets reported when the title itself is processed.
        }
        return (name, icon, displayVersion);
    }
}
