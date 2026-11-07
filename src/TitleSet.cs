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
internal sealed record InputIssue(string Path, string Message);

/// <summary>What's known of a title from the start of its file.</summary>
internal sealed record TitleSummary(ulong TitleId, byte Type, uint Version, ulong LinkedId, long Bytes, bool HoldsOnlyPart, string? Name, string? DisplayVersion, byte[]? Icon);

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
    public const string UpdatesWontWork = "An update made for the original game won't work with a compressed copy of it, and the game won't open with one installed. To keep an update, give it along with the game, and they come out as one title.";

    public required TitleItem Main { get; init; }
    public TitleItem? Update { get; init; }
    public required List<ContentPair> Contents { get; init; }
    public required long InputBytes { get; set; }

    /// <summary>Other titles (DLC) packed into the same NSP with --mergedlc. Each keeps its own content meta.</summary>
    public List<TitleJob> Extra { get; } = [];

    /// <summary>This title first, then the ones packed alongside it.</summary>
    public IEnumerable<TitleJob> All
    {
        get
        {
            yield return this;
            foreach (var extra in Extra)
                yield return extra;
        }
    }
    public string? TitleName { get; init; }

    /// <summary>control.nacp 里给玩家看的版本号，如 1.3.2。</summary>
    public string? DisplayVersion { get; init; }

    /// <summary>The picture the Switch shows for it, as the JPEG it's kept as.</summary>
    public byte[]? Icon { get; init; }

    public bool IsBaseGame => Main.Meta.Type == Cnmt.MetaApplication;

    /// <summary>The game it's part of: itself for a base game, and the game a DLC is for.</summary>
    public ulong ApplicationId => IsBaseGame ? Main.Meta.TitleId : Main.Meta.LinkedId;

    /// <summary>Something this tool made, which only gets made once there's nothing newer to make it from.</summary>
    public bool IsOwnOutput => Update is null && TitleSet.IsOwnOutput(Main.Archive.FilePath);

    /// <summary>What there is to know about it that isn't the title itself: other copies of it, and what was left aside.</summary>
    public List<string> Notes { get; } = [];

    public void ReleaseMemory()
    {
        foreach (var title in All)
            title.Contents.ForEach(c => c.ReleaseMemory());
    }

    public string DisplayName => TitleName ?? Path.GetFileNameWithoutExtension(Main.Archive.FilePath);

    public string Detail
    {
        get
        {
            var meta = Main.Meta;
            int dlc = Extra.Count;
            string count = dlc > 0 ? $" + {dlc} 个 DLC" : "";
            if (!IsBaseGame)
                return dlc > 0 ? $"DLC v{meta.Version}{count}, 重新打包为一个文件" : $"DLC v{meta.Version}";
            return Update is { } u
                ? $"本体 v{meta.Version} + 更新包 v{u.Meta.Version}{count}, 重新打包为一个文件"
                : dlc > 0 ? $"本体 v{meta.Version}{count}, 重新打包为一个文件" : $"本体 v{meta.Version}";
        }
    }

    public string Describe() => $"{DisplayName} [{Main.Meta.TitleId:X16}]: {Detail}";

    public string Stem(uint version, bool sharedFile, bool withDlcTag = false)
    {
        var meta = Main.Meta;
        string stem;
        bool hasVersion = false;
        if (IsBaseGame && TitleName is { } name)
        {
            stem = $"{LocalNames.ForOutputName(name)} [{meta.TitleId:X16}][v{version}]";
            hasVersion = true;
        }
        else
        {
            stem = LocalNames.ForOutputName(Path.GetFileNameWithoutExtension(Main.Archive.FilePath));
            if (sharedFile)
                stem += $" [{meta.TitleId:X16}]";
        }

        // 整合 DLC 时标出游戏版本号与 DLC 个数，例如 [v1.3.2][2DLC]。
        if (withDlcTag && Extra.Count > 0)
        {
            if (!hasVersion)
                stem += $"[v{version}]";
            if (DisplayVersion is { Length: > 0 } shown)
                stem += $"[v{LocalNames.ForOutputName(shown)}]";
            stem += $"[{Extra.Count}DLC]";
        }
        return stem;
    }
}

/// <summary>Opens the input files and works out which titles they hold and which update goes with which base game.</summary>
internal sealed class TitleSet : IDisposable
{
    private const string OwnOutputMarker = "[LZ4]";

    /// <summary>What this tool makes is named so, which says it's compressed without reading it.</summary>
    public static bool IsOwnOutput(string path) => Path.GetFileName(path).Contains(OwnOutputMarker, StringComparison.OrdinalIgnoreCase);

    /// <summary>A game this small with nothing else to it isn't a game, but something like a forwarder.</summary>
    private const long SmallGameBytes = 1 << 20;

    private readonly Keyset _keyset;
    private readonly TicketStore _tickets;
    private readonly List<GameArchive> _archives = [];
    private readonly List<TitleItem> _items = [];

    public List<TitleJob> Jobs { get; } = [];
    public List<InputIssue> Problems { get; } = [];
    public List<string> Notes { get; } = [];

    private TitleSet(Keyset keyset)
    {
        _keyset = keyset;
        _tickets = new TicketStore(keyset);
    }

    public bool IsInputPath(string path) =>
        _archives.Any(a => string.Equals(Path.GetFullPath(a.FilePath), Path.GetFullPath(path), StringComparison.OrdinalIgnoreCase));

    public bool SharesFile(TitleJob job) => _items.Count(i => i.Archive == job.Main.Archive && i.Meta.Type != Cnmt.MetaPatch) > 1;

    /// <summary>属于同一个游戏的所有输入文件路径（本体、更新包、全部 DLC），供一次清除整个项目。</summary>
    public string[] InputPathsFor(ulong applicationId) =>
        _items.Where(i => (i.Meta.Type == Cnmt.MetaApplication ? i.Meta.TitleId : i.Meta.LinkedId) == applicationId)
              .Select(i => i.Archive.FilePath)
              .Distinct(LocalNames.PathComparer)
              .ToArray();

    /// <summary>Opens the files without putting their titles together into games, which is all <see cref="汇总"/> needs. A lone update
    /// isn't a problem until it's to be done something with.</summary>
    public static TitleSet OpenOnly(IEnumerable<string> paths, Keyset keyset)
    {
        var set = new TitleSet(keyset);
        set.AddFiles(paths.ToList(), null);
        return set;
    }

    /// <summary>Opens every file. <paramref name="已打开"/> hears how many of how many have been, as they are, from any thread.</summary>
    public static TitleSet Load(IEnumerable<string> inputs, Keyset keyset, Action<int, int>? opened = null)
    {
        var set = new TitleSet(keyset);
        set.AddFiles(Expand(inputs, set.Problems), opened);
        set.Group();
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
            list.Add(new TitleSummary(item.Meta.TitleId, item.Meta.Type, item.Meta.Version, item.Meta.LinkedId, InputBytesOf(item, null), part, name, displayVersion, icon));
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
            Notes.Add($"Left out {left} title(s) because of --only or --skip");
        int small = LeaveOutSmall(options);
        if (small > 0)
            Notes.Add($"Left out {small} game(s) under 1 MB because of --hidesmall");
    }

    /// <summary>Applies --hidesmall, and says how many it left out. An update or a DLC can be small and still be what's wanted, and so can a game
    /// that has DLC.</summary>
    public int LeaveOutSmall(Options options)
    {
        if (!options.HideSmall)
            return 0;
        var withDlc = Jobs.Where(j => !j.IsBaseGame || j.Extra.Count > 0).Select(j => j.ApplicationId).ToHashSet();
        return Jobs.RemoveAll(job => job.IsBaseGame && job.Update is null && job.InputBytes < SmallGameBytes && !withDlc.Contains(job.ApplicationId));
    }

    /// <summary>Turns files and folders into a list of files, searching folders and subfolders for game files. Given somewhere to put them, game
    /// folders go there instead of being searched.</summary>
    public static List<string> Expand(IEnumerable<string> inputs, List<InputIssue> problems, List<string>? gameFolders = null)
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
                var folders = gameFolders is null ? [] : GameFolder.FindIn(input);
                foreach (string folder in folders)
                {
                    if (!gameFolders!.Contains(folder, LocalNames.PathComparer))
                        gameFolders!.Add(folder);
                }
                var everywhere = new EnumerationOptions { RecurseSubdirectories = true, IgnoreInaccessible = true, MatchCasing = MatchCasing.CaseInsensitive, AttributesToSkip = 0 };
                var found = Directory.EnumerateFiles(input, "*", everywhere).Where(GameArchive.IsGameFile)
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
        Together(paths.Count, i =>
        {
            opened[i] = OpenFile(paths[i]);
            progress?.Invoke(Interlocked.Increment(ref done), paths.Count);
        });
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
            opened.Problems.Add(new InputIssue(path, $"'{Path.GetFileName(path)}': {e.Message}"));
        }
        return opened;
    }

    /// <summary>Sphaira's forwarders have the wrong size recorded for their content meta, so that a dump of one only holds the start of it. All that
    /// was in it can be told from a program and its control data, as it's the same for all of them.</summary>
    private TitleItem ItemFromContents(GameArchive nsp, GameArchive.Entry metaEntry)
    {
        string why = $"{metaEntry.Name} is only {metaEntry.Size} bytes, where an NCA takes at least 3 KB";
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
    /// One job for each base game, with its newest update, and for each DLC. Of several copies of the same one, the newest is used, and the others
    /// are noted on the job. What this tool already compressed takes the place of what it was made from when it's as new, so it isn't done twice.
    /// </summary>
    private void Group()
    {
        var updates = _items.Where(i => i.Meta.Type == Cnmt.MetaPatch).ToList();
        var used = new HashSet<TitleItem>();
        var wanted = new List<(TitleItem Main, TitleItem? Update, List<string> Notes)>();

        foreach (var group in _items.Where(i => i.Meta.Type == Cnmt.MetaApplication).GroupBy(i => i.Meta.TitleId))
        {
            var notes = new List<string>();
            var candidates = updates.Where(u => u.Meta.LinkedId == group.Key).ToList();
            used.UnionWith(candidates);
            var originals = group.Where(i => !IsOwnOutput(i.Archive.FilePath)).ToList();
            TitleItem? main = null, update = null;
            if (originals.Count > 0)
            {
                main = NewestAndNoteOthers(originals, "本体游戏", notes);
                if (candidates.Count > 0)
                    update = NewestAndNoteOthers(candidates, "update", notes);
                if (update is not null && !UpdateGoesWith(update, main))
                {
                    notes.Add($"Its update v{update.Meta.Version} in '{Path.GetFileName(update.Archive.FilePath)}' was left aside, as the base game is already v{main.Meta.Version}");
                    update = null;
                }
            }
            else if (candidates.Count > 0)
            {
                notes.AddRange(candidates.Select(u => $"Its update v{u.Meta.Version} is in '{Path.GetFileName(u.Archive.FilePath)}'"));
            }
            wanted.Add(PreferCompressed(group, main, update, notes));
        }

        foreach (var group in _items.Where(i => i.Meta.Type == Cnmt.MetaAddOnContent).GroupBy(i => i.Meta.TitleId))
        {
            var notes = new List<string>();
            var originals = group.Where(i => !IsOwnOutput(i.Archive.FilePath)).ToList();
            var main = originals.Count > 0 ? NewestAndNoteOthers(originals, "DLC", notes) : null;
            wanted.Add(PreferCompressed(group, main, null, notes));
        }

        // Several at a time here too, and in the same order however quickly each one comes.
        var made = new (TitleJob? Job, InputIssue? Problem)[wanted.Count];
        Together(wanted.Count, i => made[i] = MakeJob(wanted[i].Main, wanted[i].Update));
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
            Problems.Add(new InputIssue(update.Archive.FilePath, $"'{Path.GetFileName(update.Archive.FilePath)}' 是更新包 (v{update.Meta.Version})，缺少对应的本体游戏 {update.Meta.LinkedId:X16}。" +
                         "升级补丁不能单独重建：请把本体游戏加入同一批处理"));
        }

        foreach (var other in _items.Where(i => i.Meta.Type is not (Cnmt.MetaApplication or Cnmt.MetaPatch or Cnmt.MetaAddOnContent)))
            Problems.Add(new InputIssue(other.Archive.FilePath, $"'{Path.GetFileName(other.Archive.FilePath)}' 含有不支持的内容元数据类型 0x{other.Meta.Type:x2}"));
    }

    /// <summary>Puts each DLC into its base game's job so they come out as one NSP holding several content metas.
    /// DLC whose base game isn't in this batch stays a job of its own.</summary>
    public void MergeDlcIntoBase()
    {
        var bases = new Dictionary<ulong, TitleJob>();
        foreach (var job in Jobs.Where(j => j.IsBaseGame))
            bases.TryAdd(job.Main.Meta.TitleId, job);

        foreach (var group in Jobs.Where(j => !j.IsBaseGame).GroupBy(j => j.ApplicationId).ToList())
        {
            if (!bases.TryGetValue(group.Key, out var main))
                continue;
            foreach (var dlc in group)
            {
                main.Extra.Add(dlc);
                main.InputBytes += dlc.InputBytes;
                foreach (string note in dlc.Notes)
                    main.Notes.Add($"DLC {dlc.Main.Meta.TitleId:X16}: {note}");
                Jobs.Remove(dlc);
            }
        }
    }

    /// <summary>The newest compressed copy, if it's at least as new as what there is to make one from. Otherwise what to make one from.</summary>
    private static (TitleItem Main, TitleItem? Update, List<string> Notes) PreferCompressed(IEnumerable<TitleItem> group, TitleItem? main, TitleItem? update, List<string> notes)
    {
        var compressed = group.Where(i => IsOwnOutput(i.Archive.FilePath)).OrderByDescending(i => i.Meta.Version).ToList();
        uint madeFrom = Math.Max(main?.Meta.Version ?? 0, update?.Meta.Version ?? 0);
        if (compressed.Count > 0 && (main is null || compressed[0].Meta.Version >= madeFrom))
        {
            foreach (var older in compressed.Skip(1))
                notes.Add($"Another compressed copy, v{older.Meta.Version}, is in '{Path.GetFileName(older.Archive.FilePath)}'");
            if (main is not null)
            {
                string from = update is null ? $"'{Path.GetFileName(main.Archive.FilePath)}'" : $"'{Path.GetFileName(main.Archive.FilePath)}' and '{Path.GetFileName(update.Archive.FilePath)}'";
                notes.Insert(0, $"Compressed from {from}, which are here too");
            }
            return (compressed[0], null, notes);
        }
        foreach (var older in compressed)
            notes.Add($"已存在旧版压缩副本 v{older.Meta.Version}（{Path.GetFileName(older.Archive.FilePath)}）。再次压缩会生成 v{madeFrom} 的副本");
        return (main!, update, notes);
    }

    /// <summary>An update is newer than the base game it's for, unless that base game was cut down to go with it, in which case they have the same version.</summary>
    private bool UpdateGoesWith(TitleItem update, TitleItem main)
    {
        if (update.Meta.Version != main.Meta.Version)
            return update.Meta.Version > main.Meta.Version;
        try
        {
            return main.Meta.Records.Where(r => r.Type == Cnmt.ContentTypeProgram).Any(r => OpenContent(main, r).HoldsOnlyPart);
        }
        catch (ToolException)
        {
            return false;
        }
    }

    /// <summary>The newest, and of copies just as new, one that isn't packed with NCZ, as that takes longer to read.</summary>
    private static TitleItem NewestAndNoteOthers(IEnumerable<TitleItem> items, string kind, List<string> notes)
    {
        var ordered = items.OrderByDescending(i => i.Meta.Version)
            .ThenBy(i => Path.GetExtension(i.Archive.FilePath).ToLowerInvariant() is ".nsz" or ".xcz" ? 1 : 0).ToList();
        foreach (var other in ordered.Skip(1))
        {
            string where = Path.GetFileName(other.Archive.FilePath);
            notes.Add(other.Meta.Version == ordered[0].Meta.Version
                ? $"Another copy of this {kind} is in '{where}'"
                : $"An older {kind}, v{other.Meta.Version}, is in '{where}'");
        }
        return ordered[0];
    }

    private (TitleJob? Job, InputIssue? Problem) MakeJob(TitleItem main, TitleItem? update)
    {
        try
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
            return (new TitleJob { Main = main, Update = update, Contents = contents, InputBytes = InputBytesOf(main, update), TitleName = name, Icon = icon, DisplayVersion = displayVersion }, null);
        }
        catch (Exception e) when (e is ToolException or IOException or UnauthorizedAccessException or CryptographicException)
        {
            return (null, new InputIssue(main.Archive.FilePath, $"'{Path.GetFileName(main.Archive.FilePath)}': {e.Message}"));
        }
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
            throw new ToolException($"'{Path.GetFileName(item.Archive.FilePath)}' is missing {name}, which its content meta lists");
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
