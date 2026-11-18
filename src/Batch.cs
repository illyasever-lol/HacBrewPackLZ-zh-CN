using System.Diagnostics;
using System.Security.Cryptography;

namespace HacBrewPackLz;

internal enum BatchMode
{
    Ask,
    Analyze,
    Convert,
    Extract,
    /// <summary>Read all of it back to see it's whole, writing nothing.</summary>
    Check,
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

        // Game folders in the folders given are built as the screens build them, rather than passed over.
        var folderPaths = new List<string>();
        // v21：上游原生整合停用，TitleSet 只需给出各自独立的 job；整合在输出阶段由 RunMerged 完成。
        using var set = TitleSet.Load(options.Inputs, keyset, gameFolders: folderPaths, dlcInGameFile: false);
        set.Filter(options);
        var folders = GameFolders(folderPaths, options, set);
        PrintPlan(set, folders);
        if (set.Jobs.Count == 0 && folders.Count == 0)
        {
            Ui.Info("没有可处理的内容。");
            return 1;
        }
        if (mode == BatchMode.Analyze)
            return Analyzer.Report(set, options, folders);
        if (options.OutputPath is not null && set.Jobs.Count + folders.Count > 1)
            throw new ToolException("-o can only name one output, but these inputs hold several titles. Use --nspdir instead");

        while (mode == BatchMode.Ask)
        {
            var chosen = AskMode(options.Compress);
            if (chosen is null)
                return 0;
            if (chosen == BatchMode.Analyze)
                Analyzer.Report(set, options, folders);
            else
                mode = chosen.Value;
        }
        // A game folder is what extracting makes already, and has nothing packed in it to read back.
        if (mode is BatchMode.Extract or BatchMode.Check && folders.Count > 0)
        {
            Ui.Info(mode == BatchMode.Extract ? $"已跳过 {folders.Count} 个游戏文件夹：它们本来就是解包后的文件夹"
                : $"已跳过 {folders.Count} 个游戏文件夹：只检查游戏文件");
            folders.Clear();
        }

        var converter = mode == BatchMode.Convert ? new TitleConverter(options, keyset) : null;
        var extractor = mode == BatchMode.Extract ? new TitleExtractor(options) : null;
        string? spool = options.TempDir ?? (options.NspDirSet ? options.NspDir : null);
        int failures = 0;
        int leftOut = 0;
        // v21：开启 --mergedlc 时 DLC 并入游戏文件，关闭时各自单独成文件。DLC without their
        // game go together into one file, judged as one. What a DLC's turn says when another's turn settled it goes here, and whether it was left out.
        var settled = new Dictionary<TitleJob, (string Said, bool LeftOut)>();
        var gamesMade = new HashSet<ulong>();
        var ownCopies = new HashSet<ulong>();
        var clock = Stopwatch.StartNew();
        for (int i = 0; i < set.Jobs.Count; i++)
        {
            var job = set.Jobs[i];
            Ui.Info("");
            Ui.Heading($"[{i + 1}/{set.Jobs.Count}] {job.Describe()}");
            // The titles that go into the file made at this turn, this one first.
            List<TitleJob> oneFile = [job];
            try
            {
                if (mode == BatchMode.Check)
                {
                    GameCheck.Run([job], spool);
                    Ui.Success("完整：回读校验通过");
                    continue;
                }
                if (settled.Remove(job, out var turn))
                {
                    if (turn.LeftOut)
                    {
                        leftOut++;
                        Ui.Warn(turn.Said);
                    }
                    else
                    {
                        Ui.Info(turn.Said);
                    }
                    continue;
                }
                // v21 自建整合：有本体的 DLC 一律由本体那一次 RunMerged 统一写出，
                // 无论拖入文件夹时文件枚举顺序如何，都不会先单独生成一个 DLC 文件。
                if (options.MergeDlc && !job.IsBaseGame && set.Jobs.Any(j => j.IsBaseGame && j.ApplicationId == job.ApplicationId))
                    continue;
                if (converter is not null && job.IsOwnOutput && options.Compress)
                {
                    leftOut++;
                    Ui.Info("已排除：已经是压缩格式");
                    if (job.IsBaseGame)
                        ownCopies.Add(job.ApplicationId);
                    continue;
                }
                // This tool's copy of its game, or a file holding DLC without their game together, isn't made again, and a DLC with nothing to
                // compress would come out just as it went in, likely next to the same DLC in that file. Beside the copy, a DLC with no file of its
                // own, such as one only inside a merged original, gets one, whatever it saves.
                bool needsFile = ownCopies.Contains(job.ApplicationId) && set.HasNoFileOfItsOwn(job);
                if (converter is not null && !job.IsBaseGame && ((ownCopies.Contains(job.ApplicationId) && !needsFile) || set.DlcHeld(job.ApplicationId))
                    && Analyzer.IsAlreadyCompressed(job, options))
                {
                    leftOut++;
                    Ui.Warn($"已跳过：{SavingRule.Reason(Analyzer.Estimate(job, options), options)}");
                    continue;
                }
                if (converter is not null)
                {
                    // Games come first, then DLC in title ID order, so the DLC still to come go in order, after this one, and a file of DLC is
                    // named after the first of them.
                    bool alone = !job.IsBaseGame && !set.Jobs.Any(j => j.IsBaseGame && j.ApplicationId == job.ApplicationId);
                    // v21：自建整合按 ApplicationId 全量取 DLC，不依赖 Skip(i+1) 的枚举顺序。
                    if (options.MergeDlc && (job.IsBaseGame || alone))
                        oneFile = [job, .. set.Jobs.Where(j => j != job && !j.IsBaseGame && j.ApplicationId == job.ApplicationId)];
                    // A copy made again so its DLC goes into its file saves nothing. It's made anyway, as that's what was asked for.
                    bool remade = job.IsBaseGame && job.DlcNotInFile;
                    if (remade)
                        Ui.Info("其 DLC 未包含在文件内，将重新生成含该 DLC 的游戏文件");
                    // What it saves on the console, which for an NSZ isn't what the file comes to. A game goes by itself, DLC without their game
                    // together.
                    List<TitleJob> judged = job.IsBaseGame ? [job] : oneFile;
                    if (SavingRule.Applies(options) && !gamesMade.Contains(job.ApplicationId) && !remade && !needsFile
                        && Judge(judged, measure: false) is var estimate && !SavingRule.WorthCompressing(estimate, options))
                    {
                        // A first look can say a game is worth it, but not that it isn't. One it can't vouch for is left out, as everywhere
                        // else, unless --measure asks for it to be read in full first, writing nothing.
                        string? reason = null;
                        if (SavingRule.NotSure(estimate, options) && !options.Measure)
                            reason = SavingRule.NotSureNote(options);
                        if (reason is null && SavingRule.NotSure(estimate, options))
                        {
                            Ui.Info("快速预览无法判断节省量，将完整读取全部数据（不写入任何文件）");
                            estimate = Judge(judged, measure: true);
                            Ui.Info(Analyzer.MeasuredLine(estimate));
                            Ui.Detail($"实测字节数：{estimate.OutputBytes} +/- {estimate.MarginBytes}");
                        }
                        if (reason is null && !SavingRule.WorthCompressing(estimate, options))
                            reason = SavingRule.Reason(estimate, options);
                        if (reason is not null)
                        {
                            leftOut++;
                            Ui.Warn($"已跳过：{reason}");
                            // DLC without their game are left out together, and a game's DLC get turns of their own.
                            foreach (var other in judged.Skip(1))
                                settled[other] = ($"已跳过：{reason}", true);
                            continue;
                        }
                    }
                    if (options.MergeDlc && oneFile.Count > 1)
                        converter.RunMerged(oneFile, set);
                    else
                        converter.Run(job, set);
                    if (job.IsBaseGame)
                        gamesMade.Add(job.ApplicationId);
                    foreach (var other in oneFile.Skip(1))
                        settled[other] = (job.IsBaseGame ? "已并入游戏文件" : "已与其它 DLC 合并为一个文件", false);
                }
                else
                {
                    extractor!.Run(job, set);
                }
            }
            catch (Exception e) when (!Failures.IsStop(e))
            {
                // Whatever it was, it's this game that couldn't be done, and the rest still get their turn. The DLC that were to go into its file
                // don't: made on their own, they'd take the room it asked for, and a damaged one would fail again.
                failures++;
                string message = Failures.Describe(e);
                Ui.Error($"失败：{message}");
                if (ProblemReports.AgreedOnCommandLine)
                    ProblemReports.JobFailed(message, sendNow: false);
                foreach (var other in oneFile.Skip(1))
                    settled[other] = (job.IsBaseGame ? "已跳过：其游戏文件未能生成" : "已跳过：包含其它 DLC 的文件未能生成", true);
            }
        }

        // One title's estimate as it is, or several together.
        SizeEstimate Judge(List<TitleJob> jobs, bool measure)
        {
            var measuring = options.MeasuringGameOf(jobs.Sum(j => j.InputBytes));
            var each = jobs.Select(j => measure ? Analyzer.Measure(j, measuring) : Analyzer.Estimate(j, options)).ToList();
            return (each.Count == 1 ? each[0] : SizeEstimate.Together(each)).AsInstalled();
        }

        // Game folders, built as the screens build them: beside the folder, or in the NSP folder, under a free name, and left out by the same
        // rules as a game file.
        int titles = set.Jobs.Count + folders.Count;
        for (int i = 0; i < folders.Count; i++)
        {
            var folder = folders[i];
            Ui.Info("");
            Ui.Heading($"[{set.Jobs.Count + i + 1}/{titles}] {FolderLine(folder)}");
            try
            {
                if (SavingRule.Applies(options) && Analyzer.EstimateBuild(BuildOptionsFor(folder, options)) is var estimate && !SavingRule.WorthCompressing(estimate, options))
                {
                    if (SavingRule.NotSure(estimate, options) && !options.Measure)
                    {
                        leftOut++;
                        Ui.Warn($"已跳过：{SavingRule.NotSureNote(options)}");
                        continue;
                    }
                    if (SavingRule.NotSure(estimate, options))
                    {
                        Ui.Info("快速预览无法判断节省量，将完整读取全部数据（不写入任何文件）");
                        estimate = Analyzer.EstimateBuild(BuildOptionsFor(folder, options).MeasuringGameOf(folder.Bytes), measure: true);
                        Ui.Info(Analyzer.MeasuredLine(estimate));
                    }
                    if (!SavingRule.WorthCompressing(estimate, options))
                    {
                        leftOut++;
                        Ui.Warn($"已跳过：{SavingRule.Reason(estimate, options)}");
                        continue;
                    }
                }
                new Builder(BuildOptionsFor(folder, options), keyset).Run();
            }
            catch (Exception e) when (!Failures.IsStop(e))
            {
                failures++;
                string message = Failures.Describe(e);
                Ui.Error($"失败：{message}");
                if (ProblemReports.AgreedOnCommandLine)
                    ProblemReports.JobFailed(message, sendNow: false);
            }
        }

        if (converter is not null && options.Compress && set.Jobs.Any(job => job.IsBaseGame && job.Update is null))
        {
            Ui.Info("");
            Ui.Info(TitleJob.UpdatesWontWork);
        }

        // What couldn't be done goes before this ends, as nothing of this is left running afterwards.
        if (ProblemReports.AgreedOnCommandLine)
            ProblemReports.SendWaiting(inBackground: false);

        Ui.Info("");
        string summary = mode == BatchMode.Check
            ? $"用时 {ProgressReporter.FormatElapsed(clock.Elapsed)}：" + (failures == 0 ? "所有游戏回读校验通过。"
                : failures == 1 ? "有 1 个游戏回读校验未通过。" : $"有 {failures} 个游戏回读校验未通过。")
            : Summary(clock.Elapsed, titles, failures, leftOut);
        if (failures == 0)
            Ui.Success(summary);
        else
            Ui.Error(summary);
        foreach (var problem in set.Problems)
            Ui.Warn($"未处理：{problem.Message}");
        return failures > 0 || set.Problems.Count > 0 ? 1 : 0;
    }

    /// <summary>The game folders found among the inputs, opened, with --only and --skip as for titles. One that can't be read is a problem,
    /// as a file that can't be is.</summary>
    private static List<GameFolder> GameFolders(List<string> paths, Options options, TitleSet set)
    {
        var folders = new List<GameFolder>();
        foreach (string path in paths)
        {
            try
            {
                folders.Add(GameFolder.Open(path));
            }
            catch (Exception e) when (e is ToolException or IOException or UnauthorizedAccessException)
            {
                set.Problems.Add(new InputIssue(path, $"'{path}': {Help.WithNextStep(e.Message)}"));
            }
        }
        static bool Matches(GameFolder folder, List<string> patterns) => patterns.Any(p =>
            p.Equals($"{folder.TitleId:X16}", StringComparison.OrdinalIgnoreCase) || folder.Name.Contains(p, StringComparison.OrdinalIgnoreCase));
        int left = folders.RemoveAll(f => (options.OnlyTitles.Count > 0 && !Matches(f, options.OnlyTitles)) || Matches(f, options.SkipTitles));
        if (left > 0)
            set.Notes.Add($"因 --only / --skip 排除 {left} 个游戏文件夹");
        return folders;
    }

    public static string FolderLine(GameFolder folder) => $"{folder.Name} [{folder.TitleId:X16}]: game folder, v{folder.Version}";

    /// <summary>How a game folder is built from the command line: as the screens build it, with what's given, and exactly where -o says if
    /// it's given.</summary>
    public static Options BuildOptionsFor(GameFolder folder, Options options)
    {
        var build = folder.BuildOptions(options.Clone());
        if (options.OutputPath is { } exact)
            build.OutputPath = exact;
        return build;
    }

    /// <summary>What a batch came to, in the words every version of the program uses.</summary>
    public static string Summary(TimeSpan elapsed, int titles, int failures, int leftOut)
    {
        int attempted = titles - leftOut;
        string took = $"用时 {ProgressReporter.FormatElapsed(elapsed)}：";
        if (attempted == 0)
            return $"{took}全部 {leftOut} 个因收益过低跳过。";
        return $"{took}{attempted} 个中成功 {attempted - failures} 个{(leftOut > 0 ? $"，{leftOut} 个因收益过低跳过" : "")}。";
    }

    private static void PrintPlan(TitleSet set, List<GameFolder> folders)
    {
        Ui.Info($"找到 {set.Jobs.Count} 个标题{(folders.Count > 0 ? $"和 {folders.Count} 个游戏文件夹" : "")}：");
        foreach (var job in set.Jobs)
        {
            Ui.Info($"  {job.Describe()}{(job.IsOwnOutput ? ", already compressed" : "")}");
            foreach (string note in job.Notes)
                Ui.Detail($"    {note}");
        }
        foreach (var folder in folders)
            Ui.Info($"  {FolderLine(folder)}, in '{folder.Path}'");
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
            Console.Write(compressing ? "[C]ompress to NSP, [A]nalyze the size, [E]xtract the files, check for [D]amage, or [Q]uit? (default C): "
                : "[C] Repack to NSP, [A]nalyze the size, [E]xtract the files, check for [D]amage, or [Q]uit? (default C): ");
            string answer = (Console.ReadLine() ?? "").Trim();
            if (answer.Length == 0 || answer.Equals("c", StringComparison.OrdinalIgnoreCase))
                return BatchMode.Convert;
            if (answer.Equals("a", StringComparison.OrdinalIgnoreCase))
                return BatchMode.Analyze;
            if (answer.Equals("e", StringComparison.OrdinalIgnoreCase))
                return BatchMode.Extract;
            if (answer.Equals("d", StringComparison.OrdinalIgnoreCase))
                return BatchMode.Check;
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
    /// <summary>What the game being made keeps of the original, written into its control data.</summary>
    private UpdateMap? _map;
    /// <summary>What the copy being made says about itself, written into its control data too.</summary>
    private CopyMark? _mark;

    /// <summary>
    /// Game data found held again while compressing, rather than through an update, in everything this has made. No estimate knows of it,
    /// so what's made can come to up to that much less than its estimate.
    /// </summary>
    public long FoundBytes { get; private set; }

    public TitleConverter(Options options, Keyset keyset)
    {
        _options = options;
        _keyset = keyset;
        int generationIndex = options.KeyGeneration - 1;
        var keyAreaKey = keyset.TryKeyAreaKeyApplication(generationIndex)
            ?? throw new ToolException($"key_area_key_application_{generationIndex:x2} (for keygeneration {options.KeyGeneration}) is not present in the keyset file");
        _writer = new NcaWriter(keyset.HeaderKey, keyAreaKey, options.KeyAreaKey, options.KeyGeneration, options.SdkVersion);
        if (!options.NoSignNcaSig2)
        {
            _signer = RSA.Create(2048);
            _modulus = _signer.ExportParameters(false).Modulus;
        }
    }

    /// <summary>Less game data than this isn't worth compressing: the tables saying where each piece went take more room than it could
    /// save, as with a DLC that's only a few bytes, which came out at 48 KB.</summary>
    private const long MinGameDataToCompress = 1 << 20;

    /// <summary>What happens to one section. The estimate follows this too, so it can't drift from the real thing.</summary>
    public static SectionPlan Plan(ContentPair content, SectionSource source, Options options)
    {
        var (owner, section, _) = source;
        bool holdsGameData = owner.ContentType is NcaContentType.Program or NcaContentType.Data or NcaContentType.PublicData;
        bool isPatched = section.HasIndirect || section.Encryption == NcaFsSection.EncryptCtrEx;

        if (section.FsType == NcaFsSection.FsRomFs)
        {
            // Too little to gain anything, it stays as it is, compressed by Nintendo or not, unless an update has to be made part of it.
            long data = section.DataLevelSize;
            if (holdsGameData && options.Compress && data > 0 && data < MinGameDataToCompress)
                return isPatched ? SectionPlan.RebuildRomFs : SectionPlan.PassThrough;
            if (holdsGameData && options.Compress)
                return SectionPlan.CompressRomFs;
            return isPatched || (holdsGameData && section.HasCompression) ? SectionPlan.RebuildRomFs : SectionPlan.PassThrough;
        }
        return (IsExefs(content, section) && !options.NoSignNcaSig2) || isPatched ? SectionPlan.RebuildPfs0 : SectionPlan.PassThrough;
    }

    private static bool IsExefs(ContentPair content, NcaFsSection section) =>
        content.Type == Cnmt.ContentTypeProgram && section.Index == 0;

    /// <param name="dlc">DLC that goes into the game's file after it, in title ID order.</param>
    /// <returns>The finished file and its size.</returns>
    public (string path, long bytes) Run(TitleJob job, TitleSet set, IReadOnlyList<TitleJob>? dlc = null)
    {
        // For the tests, a game that can't be done, with a name and an error code in what it says, as real ones have.
        if (ProblemReports.TestProblem == "job")
            throw new ToolException($"Couldn't write '{job.Main.Meta.TitleId:X16} in a folder of yours' (2002-0002), for the tests");
        List<TitleJob> titles = [job, .. (dlc ?? []).OrderBy(d => d.Main.Meta.TitleId)];
        string path = OutputPath(job, set, VersionOf(job));
        string directory = Path.GetDirectoryName(Path.GetFullPath(path))!;
        Directory.CreateDirectory(directory);
        RoomCheck.ForTitles(titles, _options, directory);
        string tempDir = _options.TempDir ?? directory;
        Directory.CreateDirectory(tempDir);
        NczSource.SpoolFolder = tempDir;

        try
        {
            using var nsp = new NspWriter(path, titles.Sum(t => t.Contents.Count), titles.Count);
            foreach (var title in titles)
            {
                try
                {
                    WriteTitle(nsp, title);
                }
                catch (Exception e) when (title != job && IsDlcFailure(e))
                {
                    throw DlcFailed(title, e);
                }
                // What's kept of a title while it's written isn't needed for the next.
                title.ReleaseMemory();
            }
            nsp.Finish();
            if (_options.KeepNcaDir)
                nsp.ExportNcas(_options.NcaDir);
        }
        finally
        {
            titles.ForEach(t => t.ReleaseMemory());
        }

        long output = new FileInfo(path).Length;
        long input = titles.Sum(t => t.InputBytes);
        Ui.Info($"已生成：{path}");
        // An NSZ is compared with what it takes installed as it is, which installing unpacks it to, while this goes on the console as it is.
        // As a file it can be smaller than this, which is said, so a bigger file doesn't look like a loss.
        if (titles.Any(Analyzer.IsPacked))
        {
            long installed = titles.Sum(CopyMark.InstalledBytes);
            Ui.Success($"容量：安装后 {ProgressReporter.Format(installed)}，输出 {ProgressReporter.Format(output)}（{output * 100.0 / installed:0.0}%）");
            if (output > input)
                Ui.Info($"源文件 {ProgressReporter.Format(input)} 更小，因为它打包得更紧，但安装时会解包。本成品按原样安装"
                    + (installed > output ? $"，在主机上少占 {ProgressReporter.Format(installed - output)}。" : "."));
        }
        else
        {
            Ui.Success($"容量：输入 {ProgressReporter.Format(input)}，输出 {ProgressReporter.Format(output)}（{output * 100.0 / input:0.0}%）");
        }

        if (_options.VerifyAfterBuild && Verifier.Check(path, _keyset, null, null) != 0)
            throw new ToolException($"'{path}' failed verification");
        return (path, output);
    }

    /// <summary>
    /// v21：1.4.3 起自建的整合打包，后续上游更新一律沿用本方案。
    /// 同一 ApplicationId 下的本体 / 更新 / 全部 DLC 写进同一个 NSP，每个标题各写一份 CNMT，
    /// 绝不把 DLC 的版本号或内容记录混进本体。上游 1.5.1 的 Run(job, set, dlc) 依赖文件枚举
    /// 顺序分组，并把 --titleversion 套到每个 DLC 上；拖入文件夹时会漏 DLC 或写错 CNMT 版本，
    /// 产出实机无法启动的 NSP，因此整合不再走那条链路。
    /// </summary>
    public (string path, long bytes) RunMerged(IReadOnlyList<TitleJob> jobs, TitleSet set)
    {
        var primary = jobs[0];
        uint version = VersionOf(primary);
        string path = OutputPath(primary, set, version, jobs);
        string directory = Path.GetDirectoryName(Path.GetFullPath(path))!;
        Directory.CreateDirectory(directory);
        RoomCheck.ForTitles(jobs, _options, directory);
        string tempDir = _options.TempDir ?? directory;
        Directory.CreateDirectory(tempDir);
        NczSource.SpoolFolder = tempDir;

        try
        {
            using var nsp = new NspWriter(path, jobs.Sum(j => j.Contents.Count), jobs.Count);
            foreach (var job in jobs)
            {
                // 每个标题的更新映射与副本标记都要按自己那份重算，否则 DLC 会带上本体的标记。
                _map = UpdateMap.For(job, _options.Compress);
                _mark = _options.Compress ? CopyMark.For(job, _options) : null;
                var records = new List<ContentRecord>();
                bool compressed = false;
                foreach (var content in job.Contents)
                    records.Add(new ContentRecord(WriteContent(nsp, content, ref compressed), content.Type, content.IdOffset));

                var meta = job.Main.Meta;
                // 只有本体允许被 --titleversion 覆盖；DLC 必须保留自己的版本号。
                uint titleVersion = ReferenceEquals(job, primary) ? version : (job.Update?.Meta.Version ?? meta.Version);
                var cnmt = meta.Rebuild(titleVersion, RequiredSystemVersion(job, compressed), records);
                var pfs0 = Pfs0Builder.Build([(Cnmt.FileName(meta.TitleId, meta.Type), new BytesSource(cnmt))]);
                var sections = new NcaSection?[] { NcaSections.Pfs0(pfs0, NcaSections.MetaHashBlockSize, !_options.Plaintext) };
                nsp.WriteNca(_writer, "Meta", NcaContentType.Meta, meta.TitleId, sections, null, ".cnmt.nca", 0, Sdk(job.Main.MetaNca));
            }
            nsp.Finish();
            if (_options.KeepNcaDir)
                nsp.ExportNcas(_options.NcaDir);
        }
        finally
        {
            foreach (var job in jobs)
                job.ReleaseMemory();
        }

        long output = new FileInfo(path).Length;
        long input = jobs.Sum(j => j.InputBytes);
        Ui.Info($"已生成：{path}");
        Ui.Success($"容量 {ProgressReporter.Format(input)} 压缩前，{ProgressReporter.Format(output)} 压缩后 ({output * 100.0 / Math.Max(input, 1):0.0}%)");
        if (_options.VerifyAfterBuild && Verifier.Check(path, _keyset, null, null) != 0)
            throw new ToolException($"'{path}' failed verification");
        return (path, output);
    }

    /// <summary>What can go wrong with a DLC as it goes into its game's file, where it's said under the game's heading.</summary>
    public static bool IsDlcFailure(Exception e) =>
        e is ToolException or IOException or UnauthorizedAccessException or CryptographicException or AggregateException && !Failures.IsStop(e);

    /// <summary>The same, naming the DLC.</summary>
    public static ToolException DlcFailed(TitleJob dlc, Exception e) =>
        new($"其 DLC {dlc.Main.Meta.TitleId:X16}（位于「{Path.GetFileName(dlc.Main.Archive.FilePath)}」）：{e.Message}");

    /// <summary>A title's NCAs and then its content meta, made just as they would be in a file of their own.</summary>
    private void WriteTitle(NspWriter nsp, TitleJob job)
    {
        bool compressed = false;
        var records = new List<ContentRecord>();
        // The map goes in compressed or not: a newer update is made part of either with it, on the Switch over what's installed.
        _map = UpdateMap.For(job, _options.Compress);
        _mark = _options.Compress ? CopyMark.For(job, _options) : null;
        foreach (var content in job.Contents)
            records.Add(new ContentRecord(WriteContent(nsp, content, ref compressed), content.Type, content.IdOffset));

        var meta = job.Main.Meta;
        var cnmt = meta.Rebuild(VersionOf(job), RequiredSystemVersion(job, compressed), records);
        var pfs0 = Pfs0Builder.Build([(Cnmt.FileName(meta.TitleId, meta.Type), new BytesSource(cnmt))]);
        var sections = new NcaSection?[] { NcaSections.Pfs0(pfs0, NcaSections.MetaHashBlockSize, !_options.Plaintext) };
        nsp.WriteNca(_writer, "Meta", NcaContentType.Meta, meta.TitleId, sections, null, ".cnmt.nca", 0, Sdk(job.Main.MetaNca));
    }

    private uint VersionOf(TitleJob job) => _options.TitleVersion ?? job.Update?.Meta.Version ?? job.Main.Meta.Version;

    private string OutputPath(TitleJob job, TitleSet set, uint version, IReadOnlyList<TitleJob>? merged = null)
    {
        if (_options.OutputPath is { } exact)
        {
            if (set.IsInputPath(exact))
                throw new ToolException("-o points at one of the input files, which are never overwritten");
            return exact;
        }

        string directory = _options.NspDirSet ? _options.NspDir : Path.GetDirectoryName(Path.GetFullPath(job.Main.Archive.FilePath))!;
        string stem = job.Stem(version, set.SharesFile(job));
        // v21：成品名只按「本次真正并入的标题」标注，不再依赖上游 SeparateDlc 全局开关。
        // [NDLC] / [UPD] 与实际写进 NSP 的 CNMT 数一致，避免名字说有 DLC、文件里却没有。
        if (merged is { Count: > 1 })
        {
            int dlcs = merged.Count(j => !j.IsBaseGame);
            if (dlcs > 0)
                stem += $"[{dlcs}DLC]";
            if (merged.Any(j => j.Update is not null) || set.HasUpdateFor(job.ApplicationId))
                stem += "[UPD]";
        }
        string suffix = !_options.Compress ? "" : stem.EndsWith(']') ? "[LZ4]" : " [LZ4]";
        // An input of this run is there already, so it's never written over.
        return LocalNames.FreePath(Path.Combine(directory, stem + suffix + ".nsp"));
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
        if ((_map is not null || _mark is not null) && content.Type == Cnmt.ContentTypeControl && section.FsType == NcaFsSection.FsRomFs
            && ControlWithNotes(owner, section, original, encrypted) is { } control)
            return control;
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

    /// <summary>The control data with what this tool keeps there added: the map of what's kept of the original, and what the copy says about itself.</summary>
    private NcaSection? ControlWithNotes(NcaFile owner, NcaFsSection section, NcaFile? original, bool encrypted)
    {
        var image = owner.OpenRomFsImage(section, original);
        var archive = RomFsArchive.Open(image, $"{owner.Name} section {section.Index}");
        var files = ControlFiles(archive, image, _map, _mark);
        return NcaSections.RomFs(RomFsBuilder.BuildFrom(files, archive.Directories), encrypted, null);
    }

    /// <summary>The control data's files as a copy has them: whatever it had of the map and the mark before goes, and this one's go in. The
    /// estimate builds the same from placeholders, as only the sizes count there.</summary>
    public static List<(string path, DataSource data)> ControlFiles(RomFsArchive archive, DataSource? image, UpdateMap? map, CopyMark? mark)
    {
        var files = archive.Files.Where(f => f.Path != UpdateMap.FileName && f.Path != CopyMark.FileName)
            .Select(f => (f.Path, image is null ? (DataSource)new ZeroSource(f.Size) : new SubSource(image, f.Offset, f.Size))).ToList();
        if (map is not null)
            files.Add((UpdateMap.FileName, new BytesSource(map.ToBytes())));
        if (mark is not null)
            files.Add((CopyMark.FileName, new BytesSource(mark.ToBytes())));
        return files;
    }

    private void Compress(NcaBuilder builder, int index, SectionSource source)
    {
        var image = source.Owner.OpenRomFsImage(source.Section, source.Original);
        var kept = SharedPieces.KeptBy(source.Owner, source.Section, source.Original);
        var progress = new ProgressReporter("正在压缩 RomFS", image.Length, showOutputSize: true);
        var data = builder.AddCompressed(index, image, _options.BlockSize, _options.Level, _options.Threads, !_options.Plaintext, progress, kept);
        progress.Done();
        Ui.Detail($"  RomFS {ProgressReporter.Format(data.VirtualSize)} 压缩为 {ProgressReporter.Format(data.DataSize)}（{data.DataSize * 100.0 / data.VirtualSize:0.0}%）");
        if (data.SharedBytes > 0)
            Ui.Detail($"  其中 {ProgressReporter.Format(data.SharedBytes)} 为重复数据，只存一份");
        FoundBytes += data.FoundBytes;
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
        // Never over a whole one, nor into a folder of somebody else's: only what's left of an extraction that didn't finish is cleared away.
        string notFinished = Path.Combine(root, GameFolder.NotFinished);
        if (GameFolder.IsOne(root))
            throw new ToolException($"It's extracted already, in '{root}'. Delete that folder to extract it again");
        if (File.Exists(notFinished))
            Clear(root, notFinished, removeFolder: true);
        else if (Directory.Exists(root) && Directory.EnumerateFileSystemEntries(root).Any())
            throw new ToolException($"'{root}' is there already, with other things in it. Move it or delete it to extract this game there");
        bool made = !Directory.Exists(root);
        Directory.CreateDirectory(root);
        File.WriteAllText(notFinished, GameFolder.NotFinishedText);
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
            File.Delete(notFinished);
        }
        catch
        {
            // Stopped or failed: what came out goes, as with the exefs and control folders out it would look like a whole game. A folder that
            // was there before, empty, is left as it was.
            try
            {
                Clear(root, notFinished, removeFolder: made);
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException)
            {
                Ui.Warn($"已解包的内容未能全部从 '{root}' 删除：{e.Message}。该文件夹不会作为游戏列出，说明见其中的备注");
            }
            throw;
        }
        finally
        {
            job.ReleaseMemory();
        }
        Ui.Success($"已解包 {files} 个文件（{ProgressReporter.Format(bytes)}）到：{root}");
        return (root, bytes);
    }

    /// <summary>What's in a folder an extraction didn't finish in, with the note saying so going last: a folder that can't all be deleted still
    /// says it's only part of a game.</summary>
    private static void Clear(string root, string notFinished, bool removeFolder)
    {
        foreach (string entry in Directory.EnumerateFileSystemEntries(root).Where(e => !string.Equals(e, notFinished, StringComparison.OrdinalIgnoreCase)).ToList())
        {
            if (Directory.Exists(entry))
                Directory.Delete(entry, true);
            else
                File.Delete(entry);
        }
        File.Delete(notFinished);
        if (removeFolder)
            Directory.Delete(root);
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
