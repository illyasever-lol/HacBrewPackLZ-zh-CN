using System.Diagnostics;
using System.Security.Cryptography;

namespace HacBrewPackLz;

internal enum BatchMode
{
    Ask,
    Analyze,
    Convert,
    Extract,
}

/// <summary>Convert and extract several inputs. One failed title doesn't stop the rest.</summary>
internal static class Batch
{
    public static int Run(Options options, Keyset keyset, BatchMode mode)
    {
        if (options.TitleId != 0 || options.TitleName is not null || options.TitlePublisher is not null || options.NoRomfs || options.NoLogo)
            throw new ToolException("--titleid, --titlename, --titlepublisher, --noromfs and --nologo only apply when building from folders");
        if (options.Inputs.Count == 0)
            throw new ToolException("未指定游戏文件或文件夹");

        using var set = TitleSet.Load(options.Inputs, keyset);
        set.Filter(options);
        if (options.MergeDlc)
            set.MergeDlcIntoBase();
        PrintPlan(set);
        if (set.Jobs.Count == 0)
        {
            Ui.Info("没有可处理的内容。");
            return 1;
        }
        if (mode == BatchMode.Analyze)
            return Analyzer.Report(set, options);
        if (options.OutputPath is not null && set.Jobs.Count > 1)
            throw new ToolException("-o can only name one output, but these inputs hold several titles. Use --nspdir instead");

        while (mode == BatchMode.Ask)
        {
            var chosen = AskMode(options.Compress);
            if (chosen is null)
                return 0;
            if (chosen == BatchMode.Analyze)
                Analyzer.Report(set, options);
            else
                mode = chosen.Value;
        }

        var converter = mode == BatchMode.Convert ? new TitleConverter(options, keyset) : null;
        var extractor = mode == BatchMode.Extract ? new TitleExtractor(options) : null;
        int failures = 0;
        int leftOut = 0;
        var clock = Stopwatch.StartNew();
        for (int i = 0; i < set.Jobs.Count; i++)
        {
            var job = set.Jobs[i];
            Ui.Info("");
            Ui.Heading($"[{i + 1}/{set.Jobs.Count}] {job.Describe()}");
            try
            {
                if (converter is not null && job.IsOwnOutput && options.Compress)
                {
                    leftOut++;
                    Ui.Info("已排除：已经是压缩格式");
                    continue;
                }
                if (converter is not null)
                {
                    if (SavingRule.Applies(options) && Analyzer.Estimate(job, options) is var estimate && !SavingRule.WorthCompressing(estimate, options))
                    {
                        leftOut++;
                        Ui.Warn($"Left out: {SavingRule.Reason(estimate, options)}");
                        continue;
                    }
                    converter.Run(job, set);
                }
                else
                {
                    extractor!.Run(job, set);
                }
            }
            catch (Exception e) when (e is ToolException or IOException or UnauthorizedAccessException or CryptographicException or AggregateException)
            {
                failures++;
                Ui.Error($"FAILED: {e.Message}");
            }
        }

        if (converter is not null && options.Compress && set.Jobs.Any(job => job.IsBaseGame && job.Update is null))
        {
            Ui.Info("");
            Ui.Info(TitleJob.UpdatesWontWork);
        }

        Ui.Info("");
        string summary = Summary(clock.Elapsed, set.Jobs.Count, failures, leftOut);
        if (failures == 0)
            Ui.Success(summary);
        else
            Ui.Error(summary);
        foreach (var problem in set.Problems)
            Ui.Warn($"Not processed: {problem.Message}");
        return failures > 0 || set.Problems.Count > 0 ? 1 : 0;
    }

    /// <summary>What a batch came to, in the words every version of the program uses.</summary>
    public static string Summary(TimeSpan elapsed, int titles, int failures, int leftOut)
    {
        int attempted = titles - leftOut;
        string took = $"任务用时  {ProgressReporter.FormatElapsed(elapsed)} ：";
        if (attempted == 0)
            return $"{took}全部 {leftOut} 个因不划算跳过。";
        return $"{took}{attempted - failures} / {attempted} 已完成{(leftOut > 0 ? $", {leftOut} 个因不划算跳过" : "")}";
    }

    private static void PrintPlan(TitleSet set)
    {
        Ui.Info($"Found {set.Jobs.Count} title(s):");
        foreach (var job in set.Jobs)
        {
            Ui.Info($"  {job.Describe()}{(job.IsOwnOutput ? "，已压缩" : "")}");
            foreach (string note in job.Notes)
                Ui.Detail($"    {note}");
        }
        foreach (string note in set.Notes)
            Ui.Detail($"  Note: {note}");
        foreach (var problem in set.Problems)
            Ui.Warn($"  {problem.Message}");
    }

    /// <summary>Null means quit.</summary>
    private static BatchMode? AskMode(bool compressing)
    {
        if (Console.IsInputRedirected)
            return BatchMode.Convert;

        Ui.Info("");
        while (true)
        {
            Console.Write(compressing ? "[C] 压缩为 NSP，[A] 分析体积，[E] 解包文件，或 [Q] 退出？（默认 C）：" : "[C] 重打包为 NSP，[A] 分析体积，[E] 解包文件，或 [Q] 退出？（默认 C）：");
            string answer = (Console.ReadLine() ?? "").Trim();
            if (answer.Length == 0 || answer.Equals("c", StringComparison.OrdinalIgnoreCase))
                return BatchMode.Convert;
            if (answer.Equals("a", StringComparison.OrdinalIgnoreCase))
                return BatchMode.Analyze;
            if (answer.Equals("e", StringComparison.OrdinalIgnoreCase))
                return BatchMode.Extract;
            if (answer.Equals("q", StringComparison.OrdinalIgnoreCase))
                return null;
        }
    }
}

/// <summary>
/// Rebuilds a title as a new Application or DLC NSP with no ticket needed. Program and Data RomFS sections are stored
/// compressed, an update is applied onto its base, and everything else is decrypted and re-encrypted byte for byte.
/// </summary>
internal sealed class TitleConverter
{
    private readonly Options _options;
    private readonly NcaWriter _writer;
    private readonly RSA? _signer;
    private readonly byte[]? _modulus;
    private readonly Keyset _keyset;

    public TitleConverter(Options options, Keyset keyset)
    {
        _options = options;
        _keyset = keyset;
        int generationIndex = Math.Max(options.KeyGeneration - 1, 0);
        var keyAreaKey = keyset.TryKeyAreaKeyApplication(generationIndex)
            ?? throw new ToolException($"key_area_key_application_{generationIndex:x2} (for keygeneration {options.KeyGeneration}) is not present in the keyset file");
        _writer = new NcaWriter(keyset.HeaderKey, keyAreaKey, options.KeyAreaKey, options.KeyGeneration, options.SdkVersion);
        if (!options.NoSignNcaSig2)
        {
            _signer = RSA.Create(2048);
            _modulus = _signer.ExportParameters(false).Modulus;
        }
    }

    /// <summary>What happens to one section. The estimate follows this too, so it can't drift from the real thing.</summary>
    public static SectionPlan Plan(ContentPair content, SectionSource source, Options options)
    {
        var (owner, section, _) = source;
        bool holdsGameData = owner.ContentType is NcaContentType.Program or NcaContentType.Data or NcaContentType.PublicData;
        bool isPatched = section.HasIndirect || section.Encryption == NcaFsSection.EncryptCtrEx;

        if (section.FsType == NcaFsSection.FsRomFs)
        {
            if (holdsGameData && options.Compress)
                return SectionPlan.CompressRomFs;
            return isPatched || (holdsGameData && section.HasCompression) ? SectionPlan.RebuildRomFs : SectionPlan.PassThrough;
        }
        return (IsExefs(content, section) && !options.NoSignNcaSig2) || isPatched ? SectionPlan.RebuildPfs0 : SectionPlan.PassThrough;
    }

    private static bool IsExefs(ContentPair content, NcaFsSection section) =>
        content.Type == Cnmt.ContentTypeProgram && section.Index == 0;

    /// <returns>The finished file and its size.</returns>
    public (string path, long bytes) Run(TitleJob job, TitleSet set)
    {
        uint version = _options.TitleVersion ?? job.Update?.Meta.Version ?? job.Main.Meta.Version;
        string path = OutputPath(job, set, version);
        string directory = Path.GetDirectoryName(Path.GetFullPath(path))!;
        Directory.CreateDirectory(directory);
        RoomCheck.ForTitle(job, _options, directory);
        string tempDir = _options.TempDir ?? directory;
        Directory.CreateDirectory(tempDir);
        NczSource.SpoolFolder = tempDir;

        bool compressed = false;
        var titles = job.All.ToList();
        try
        {
            using var nsp = new NspWriter(path, titles.Sum(t => t.Contents.Count), titles.Count);
            // One content meta per title, with all of their content NCAs in the same PFS0.
            foreach (var title in titles)
            {
                var records = new List<ContentRecord>();
                foreach (var content in title.Contents)
                    records.Add(new ContentRecord(WriteContent(nsp, content, ref compressed), content.Type, content.IdOffset));

                var meta = title.Main.Meta;
                uint titleVersion = ReferenceEquals(title, job) ? version : meta.Version;
                var cnmt = meta.Rebuild(titleVersion, RequiredSystemVersion(title, compressed), records);
                var pfs0 = Pfs0Builder.Build([(Cnmt.FileName(meta.TitleId, meta.Type), new BytesSource(cnmt))]);
                var sections = new NcaSection?[] { NcaSections.Pfs0(pfs0, NcaSections.MetaHashBlockSize, !_options.Plaintext) };
                nsp.WriteNca(_writer, "Meta", NcaContentType.Meta, meta.TitleId, sections, null, ".cnmt.nca", 0, Sdk(title.Main.MetaNca));
            }

            nsp.Finish();
            if (_options.KeepNcaDir)
                nsp.ExportNcas(_options.NcaDir);
        }
        finally
        {
            job.ReleaseMemory();
        }

        long output = new FileInfo(path).Length;
        Ui.Info($"创建: {path}");
        Ui.Success($"容量 {ProgressReporter.Format(job.InputBytes)} 压缩前, {ProgressReporter.Format(output)} 压缩后 ({output * 100.0 / job.InputBytes:0.0}%)");

        if (_options.VerifyAfterBuild && Verifier.Check(path, _keyset, null, null) != 0)
            throw new ToolException($"'{path}' failed verification");
        return (path, output);
    }

    private string OutputPath(TitleJob job, TitleSet set, uint version)
    {
        if (_options.OutputPath is { } exact)
        {
            if (set.IsInputPath(exact))
                throw new ToolException("-o points at one of the input files, which are never overwritten");
            return exact;
        }

        string directory = _options.NspDirSet ? _options.NspDir : Path.GetDirectoryName(Path.GetFullPath(job.Main.Archive.FilePath))!;
        string stem = job.Stem(version, set.SharesFile(job), withDlcTag: true);
        string suffix = !_options.Compress ? "" : stem.EndsWith(']') ? "[LZ4]" : " [LZ4]";
        string path = Path.Combine(directory, stem + suffix + ".nsp");
        return set.IsInputPath(path) ? Path.Combine(directory, stem + suffix + " (converted).nsp") : path;
    }

    private uint? RequiredSystemVersion(TitleJob job, bool compressed)
    {
        if (_options.RequiredSystemVersion is { } chosen)
            return chosen;
        uint required = Math.Max(job.Main.Meta.RequiredSystemVersion, job.Update?.Meta.RequiredSystemVersion ?? 0);
        return compressed ? Math.Max(required, Options.CompressionMinSystemVersion) : required;
    }

    private uint Sdk(NcaFile source) => _options.SdkVersionSet ? _options.SdkVersion : source.SdkVersion;

    private NcaInfo WriteContent(NspWriter nsp, ContentPair content, ref bool compressed)
    {
        var primary = content.Primary;
        var order = content.SectionsInFileOrder().ToList();
        bool signed = false;

        if (!order.Any(i => Plan(content, content.SectionAt(i)!.Value, _options) == SectionPlan.CompressRomFs))
        {
            var sections = new NcaSection?[primary.Sections.Length];
            foreach (int i in order)
                sections[i] = BuildSection(content, content.SectionAt(i)!.Value, ref signed);
            return nsp.WriteNca(_writer, Label(content), primary.ContentType, primary.ProgramId, sections, signed ? _signer : null,
                ".nca", primary.ContentIndex, Sdk(primary));
        }

        compressed = true;
        var builder = nsp.BeginNca(_writer);
        foreach (int i in order)
        {
            var source = content.SectionAt(i)!.Value;
            if (Plan(content, source, _options) == SectionPlan.CompressRomFs)
                Compress(builder, i, source);
            else
                builder.Add(i, BuildSection(content, source, ref signed));
        }
        return nsp.EndNca(builder, primary.ContentType, primary.ProgramId, signed ? _signer : null, primary.ContentIndex, Sdk(primary));
    }

    private NcaSection BuildSection(ContentPair content, SectionSource source, ref bool signed)
    {
        var (owner, section, original) = source;
        bool encrypted = !_options.Plaintext;
        switch (Plan(content, source, _options))
        {
            case SectionPlan.RebuildRomFs:
                var image = owner.OpenRomFsImage(section, original);
                var progress = new ProgressReporter("正在校验 RomFS", image.Length);
                var plain = NcaSections.RomFs(image, encrypted, progress);
                progress.Done();
                return plain;

            case SectionPlan.RebuildPfs0:
                bool isExefs = IsExefs(content, section);
                var archive = owner.OpenPfs0(section, original);
                signed |= isExefs && archive.Files.Any(f => f.Name == "main.npdm") && _modulus is not null;
                var files = new List<(string name, DataSource data)>();
                foreach (var file in archive.Files)
                {
                    files.Add(file.Name == "main.npdm" && isExefs && _modulus is not null
                        ? (file.Name, new BytesSource(Npdm.Patch(archive.ReadAll(file), $"{owner.Name} main.npdm", 0, _modulus, out _)))
                        : (file.Name, archive.Open(file)));
                }
                return NcaSections.Pfs0(Pfs0Builder.Build(files), archive.HashBlockSize, encrypted);

            default:
                return PassThrough(owner, section, encrypted);
        }
    }

    private void Compress(NcaBuilder builder, int index, SectionSource source)
    {
        var image = source.Owner.OpenRomFsImage(source.Section, source.Original);
        var progress = new ProgressReporter("正在压缩 RomFS", image.Length, showOutputSize: true);
        var data = builder.AddCompressed(index, image, _options.BlockSize, _options.Level, _options.Threads, !_options.Plaintext, progress);
        progress.Done();
        Ui.Detail($"  RomFS {ProgressReporter.Format(data.VirtualSize)}  存储为 {ProgressReporter.Format(data.DataSize)} ({data.DataSize * 100.0 / data.VirtualSize:0.0}%)");
    }

    /// <summary>The section's decrypted bytes are kept exactly as they are, so its hash tree stays valid.</summary>
    private static NcaSection PassThrough(NcaFile owner, NcaFsSection section, bool encrypted)
    {
        CheckHashes(owner, section);

        var header = (byte[])section.Header.Clone();
        bool keepEncrypted = encrypted && section.Encryption != NcaFsSection.EncryptNone;
        if (!keepEncrypted)
            header[4] = NcaFsSection.EncryptNone;
        return new NcaSection { FsHeader = header, Data = owner.OpenPhysical(section), Encrypted = keepEncrypted, UpperIv = section.UpperIv };
    }

    private static void CheckHashes(NcaFile owner, NcaFsSection section)
    {
        if (section.FsType == NcaFsSection.FsPartition)
        {
            owner.OpenPfs0(section, null);
            return;
        }

        var image = owner.OpenRomFsImage(section, null);
        var progress = new ProgressReporter("正在校验区段", image.Length);
        var buffer = new byte[1 << 20];
        for (long done = 0; done < image.Length;)
        {
            int n = (int)Math.Min(buffer.Length, image.Length - done);
            image.Read(done, buffer, 0, n);
            done += n;
            progress.Report(done, done);
        }
        progress.Done();
    }

    private static string Label(ContentPair content) => content.Type switch
    {
        Cnmt.ContentTypeProgram => content.IdOffset == 0 ? "Program" : $"Program {content.IdOffset}",
        Cnmt.ContentTypeData => content.IdOffset == 0 ? "Data" : $"Data {content.IdOffset}",
        Cnmt.ContentTypeControl => "Control",
        Cnmt.ContentTypeHtmlDocument => "HtmlDoc",
        Cnmt.ContentTypeLegalInformation => "LegalInfo",
        _ => $"Type {content.Type}",
    };
}

/// <summary>
/// Writes a title out as the same folders the build mode takes: exefs, romfs, logo, control, htmldoc and legalinfo.
/// With an update, the files are the updated game's.
/// </summary>
internal sealed class TitleExtractor(Options options)
{
    private const int CopyBufferSize = 1 << 22;

    /// <returns>The folder the files went to and how much was written.</returns>
    public (string path, long bytes) Run(TitleJob job, TitleSet set)
    {
        string parent = options.OutDir ?? Path.GetDirectoryName(Path.GetFullPath(job.Main.Archive.FilePath))!;
        uint version = job.Update?.Meta.Version ?? job.Main.Meta.Version;
        string root = Path.GetFullPath(Path.Combine(parent, job.Stem(version, set.SharesFile(job))));
        Directory.CreateDirectory(root);
        RoomCheck.ForExtract(job, root);
        NczSource.SpoolFolder = parent;

        long files = 0, bytes = 0;
        var buffer = new byte[CopyBufferSize];
        try
        {
            foreach (var content in job.Contents)
            {
                foreach (int i in content.SectionsInFileOrder())
                {
                    var (owner, section, original) = content.SectionAt(i)!.Value;
                    string folder = Path.Combine(root, FolderFor(content, i));
                    var (count, size) = section.FsType == NcaFsSection.FsPartition
                        ? ExtractPfs0(owner, section, original, folder, buffer)
                        : ExtractRomFs(owner, section, original, folder, buffer);
                    files += count;
                    bytes += size;
                }
            }
        }
        finally
        {
            job.ReleaseMemory();
        }
        Ui.Success($"Extracted {files} files ({ProgressReporter.Format(bytes)}) to: {root}");
        return (root, bytes);
    }

    private static string FolderFor(ContentPair content, int section)
    {
        string name = content.Type switch
        {
            Cnmt.ContentTypeProgram => section switch { 0 => "exefs", 1 => "romfs", 2 => "logo", _ => $"section{section}" },
            Cnmt.ContentTypeData => "data",
            Cnmt.ContentTypeControl => "control",
            Cnmt.ContentTypeHtmlDocument => "htmldoc",
            Cnmt.ContentTypeLegalInformation => "legalinfo",
            _ => $"type{content.Type}",
        };
        if (content.Type != Cnmt.ContentTypeProgram && section != 0)
            name += $"_section{section}";
        if (content.IdOffset == 0)
            return name;
        return content.Type == Cnmt.ContentTypeProgram ? Path.Combine($"program{content.IdOffset}", name) : $"{name}{content.IdOffset}";
    }

    private static (long files, long bytes) ExtractPfs0(NcaFile owner, NcaFsSection section, NcaFile? original, string folder, byte[] buffer)
    {
        var archive = owner.OpenPfs0(section, original);
        Directory.CreateDirectory(folder);
        var seen = new HashSet<string>(LocalNames.PathComparer);
        long bytes = 0;
        foreach (var file in archive.Files)
        {
            Copy(archive.Data, file.Offset, file.Size, Target(folder, file.Name, seen), buffer, null, 0);
            bytes += file.Size;
        }
        return (archive.Files.Count, bytes);
    }

    private static (long files, long bytes) ExtractRomFs(NcaFile owner, NcaFsSection section, NcaFile? original, string folder, byte[] buffer)
    {
        var image = owner.OpenRomFsImage(section, original);
        var archive = RomFsArchive.Open(image, owner.Name);
        Directory.CreateDirectory(folder);
        foreach (string directory in archive.Directories)
            Directory.CreateDirectory(LocalNames.Combine(folder, directory));

        long total = archive.Files.Sum(f => f.Size);
        var progress = new ProgressReporter($"Extracting {Path.GetFileName(folder)}", total);
        var seen = new HashSet<string>(LocalNames.PathComparer);
        long done = 0;

        // In image order, so a hard drive reads steadily and each hash block is only checked once.
        foreach (var file in archive.Files.OrderBy(f => f.Offset))
        {
            Copy(image, file.Offset, file.Size, Target(folder, file.Path, seen), buffer, progress, done);
            done += file.Size;
        }
        progress.Done();
        return (archive.Files.Count, total);
    }

    /// <summary>Two names that differ only by case would silently overwrite each other on most Windows and Mac disks.</summary>
    private static string Target(string folder, string relative, HashSet<string> seen)
    {
        string path = LocalNames.Combine(folder, relative);
        if (!seen.Add(path))
            throw new ToolException($"'{relative}' would overwrite another file on this file system, whose names ignore case or can't hold every character. Extract it on a case-sensitive disk");
        return path;
    }

    private static void Copy(DataSource source, long offset, long size, string path, byte[] buffer, ProgressReporter? progress, long doneBefore)
    {
        using var output = new FileStream(path, FileMode.Create, FileAccess.Write, FileShare.None, 1 << 16, FileOptions.SequentialScan);
        for (long done = 0; done < size;)
        {
            int n = (int)Math.Min(buffer.Length, size - done);
            source.Read(offset + done, buffer, 0, n);
            output.Write(buffer, 0, n);
            done += n;
            progress?.Report(doneBefore + done, doneBefore + done);
        }
    }
}
