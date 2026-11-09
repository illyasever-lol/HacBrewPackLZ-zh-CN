using System.Diagnostics;
using System.Security.Cryptography;
using System.Text;

namespace HacBrewPackLz;

internal enum SessionActivity
{
    Idle,
    Reading,
    Analyzing,
    Compressing,
    Extracting,
    Verifying,
    Sending,
}

internal enum RowState
{
    Ready,
    Working,
    Done,
    Failed,
}

internal sealed record TitleRow(string Key, string Name, string Detail, long InputBytes, string[] Files)
{
    public SizeEstimate? Estimate { get; init; }
    public RowState State { get; init; }
    public BatchMode DoneAs { get; init; }
    public long OutputBytes { get; init; }
    public string? OutputPath { get; init; }
    public string? Failure { get; init; }

    /// <summary>There's an update to make part of it, which is what repacking gains anything from.</summary>
    public bool HasUpdate { get; init; }

    /// <summary>A game laid out in folders, which is built into an NSP rather than converted, and is already as extracted as it gets.</summary>
    public bool IsFolder { get; init; }

    /// <summary>The picture the Switch shows for it, as the JPEG it's kept as.</summary>
    public byte[]? Icon { get; init; }

    /// <summary>What there is to know about it that isn't the title itself, such as other copies of it that were left aside.</summary>
    public string[] Notes { get; init; } = [];

    /// <summary>All of it is what this tool already made, as new as what it was made from.</summary>
    public bool AlreadyCompressed { get; init; }

    /// <summary>There's nothing for compressing to do: it already was, or all of it is stored that way.</summary>
    public bool IsCompressed => AlreadyCompressed || Estimate?.AlreadyCompressed == true;

    /// <summary>Whether jobs include this title. Everything starts out included.</summary>
    public bool Selected { get; init; } = true;

    /// <summary>Someone ticked or unticked it themselves, and from then on that's what counts.</summary>
    public bool ChosenByHand { get; init; }

    /// <summary>Why the minimum saving would leave this title out, if it would.</summary>
    public string? BelowMinimum { get; init; }

    /// <summary>The finished size if there is one, or else the estimate.</summary>
    public long? AfterBytes =>
        State == RowState.Done && DoneAs == BatchMode.Convert ? OutputBytes : Estimate?.OutputBytes;

    public bool IsDone(BatchMode mode) => State == RowState.Done && DoneAs == mode;

    public bool NeedsEstimate => Estimate is null && !IsDone(BatchMode.Convert);

    /// <summary>What kind of title this is and, once it's known, how far its game data compresses.</summary>
    public string DetailWithEstimate => BelowMinimum is { } reason
        ? $"{Detail}, {reason}"
        : Estimate is { RomFsBytes: > 0 } e && e.RomFsStoredBytes < e.RomFsBytes
        ? $"{Detail}, 游戏数据可压缩至 {e.RomFsFraction * 100:0}%"
        : Detail;
}

internal sealed record LogLine(UiLevel Level, string Text);

internal sealed record SessionSnapshot(
    SessionActivity Activity,
    TitleRow[] Rows,
    InputIssue[] Issues,
    string[] Notes,
    LogLine[] Log,
    ProgressSnapshot? Progress,
    string? KeyProblem,
    string? CurrentTitle,
    int CurrentIndex,
    int CurrentCount,
    string? Summary,
    UiLevel SummaryLevel,
    string? LastOutput,
    string? UpdateNote,
    SendSnapshot? Send,
    int HiddenCompressed,
    int HiddenSmall,
    int ShownCompressed)
{
    public bool IsBusy => Activity != SessionActivity.Idle;

    /// <summary>What reading is on, such as "正在打开游戏文件：12 个中的第 3 个".</summary>
    public string ReadingText => CurrentTitle is null ? "正在读取文件" : CurrentCount > 0 ? $"{CurrentTitle}: {CurrentIndex}/{CurrentCount}" : CurrentTitle;
}

/// <summary>
/// What the console screen and the GUI have in common: the list of inputs, and one job at a time running in the
/// background. Each of them only shows snapshots of this and calls the methods below.
/// </summary>
internal sealed class Session : IUiSink, IDisposable
{
    private const int MaxLogLines = 4000;

    private readonly object _gate = new();
    private readonly List<string> _files = [];
    private readonly List<string> _folders = [];
    private readonly Dictionary<string, GameFolder> _gameFolders = [];
    private readonly List<string> _queued = [];
    private readonly List<InputIssue> _expansionIssues = [];
    private readonly List<string> _notes = [];
    private readonly List<LogLine> _log = [];
    private List<string> _titleNotes = [];
    /// <summary>What each row is made of: a game with its update, and its DLC. A game folder has none.</summary>
    private readonly Dictionary<string, List<TitleJob>> _jobs = [];
    private int _hiddenSmall;
    private List<TitleRow> _rows = [];
    private List<InputIssue> _issues = [];
    private TitleSet? _set;
    private Keyset? _keyset;
    private string? _keyProblem;
    private SessionActivity _activity;
    private ProgressSnapshot? _progress;
    private string? _currentTitle;
    private int _currentIndex;
    private int _currentCount;
    private string? _summary;
    private UiLevel _summaryLevel;
    private string? _lastOutput;
    private CancellationTokenSource? _stop;
    private Thread? _worker;
    private bool _reloadNeeded;
    private bool _bestFirst;
    private int _leftOut;
    private string? _updateNote;
    private bool _updating;
    private SendWatch? _sendWatch;

    public Settings Settings { get; } = Settings.Load();

    public Session()
    {
        if (Settings.Get("--noautoupdate") != "1")
            CheckForUpdates(quietly: true);
    }

    /// <summary>On a thread of its own, as it has nothing to do with whatever else is going on.</summary>
    public void CheckForUpdates(bool quietly = false)
    {
        lock (_gate)
        {
            if (_updating)
                return;
            _updating = true;
            if (!quietly)
                _updateNote = "正在查找新版本...";
        }
        Notify();
        var thread = new Thread(() =>
        {
            var (updated, message) = Updater.Run();
            lock (_gate)
            {
                _updating = false;
                if (updated || !quietly)
                    _updateNote = message;
            }
            Notify();
        })
        { IsBackground = true, Name = "正在更新" };
        thread.Start();
    }

    /// <summary>Raised on any thread. Keep it short: just note that the screen needs drawing again.</summary>
    public event Action? Changed;

    public TimeSpan ProgressInterval => TimeSpan.FromMilliseconds(100);

    public SessionSnapshot Snapshot()
    {
        // SendWatch has its own lock. Take it before _gate, never inside it: progress
        // reporting used to run the other way round, which is a lock-order inversion.
        SendSnapshot? send = _sendWatch?.Snapshot();
        lock (_gate)
        {
            var shown = VisibleRowsLocked();
            int hidden = _rows.Count - shown.Count;
            // What could be hidden again, once it's been asked to be shown.
            int shownCompressed = Settings.Get("--showcompressed") == "1" ? _rows.Count(r => r.IsCompressed && r.State != RowState.Done) : 0;
            return new SessionSnapshot(_activity, shown.ToArray(), _issues.Concat(_expansionIssues).ToArray(), _notes.Concat(_titleNotes).ToArray(), _log.ToArray(),
                _progress, _keyProblem, _currentTitle, _currentIndex, _currentCount, _summary, _summaryLevel, _lastOutput, _updateNote, send,
                hidden, _hiddenSmall, shownCompressed);
        }
    }

    /// <summary>Hide compressed games, as on the Switch, leaves off what already was compressed when it was found. What's compressed here stays,
    /// with what it came to, until the list is read again. While sending, everything is there, as everything is being shared.</summary>
    private List<TitleRow> VisibleRowsLocked()
    {
        if (Settings.Get("--showcompressed") == "1" || _activity == SessionActivity.Sending)
            return _rows.ToList();
        return _rows.Where(r => !r.IsCompressed || r.State == RowState.Done).ToList();
    }

    public void AddInputs(IEnumerable<string> paths)
    {
        lock (_gate)
        {
            foreach (string path in paths)
            {
                if (!Directory.Exists(path) && Keyset.LooksLikeKeyFile(path))
                {
                    Settings.Set("--keyset", Path.GetFullPath(path));
                    Settings.Save();
                    _keyset = null;
                    _reloadNeeded = true;
                }
                else
                {
                    _queued.Add(path);
                }
            }
        }
        StartReading();
    }

    public void Remove(TitleRow row)
    {
        lock (_gate)
        {
            if (_activity != SessionActivity.Idle)
                return;

            // 一个项目是本体、更新包和全部 DLC 的总和。只删本体会把 UPD / DLC 留成孤立输入，
            // 重新读取后它们又变成独立行或「缺少本体」提示，所以按 ApplicationId 一次清干净。
            var doomed = new HashSet<string>(row.Files, LocalNames.PathComparer);
            _jobs.TryGetValue(row.Key, out var group);
            ulong applicationId = group is { Count: > 0 } ? group[0].ApplicationId : 0;
            if (applicationId != 0 && _set is { } set)
                foreach (string path in set.InputPathsFor(applicationId))
                    doomed.Add(path);
            if (group is not null)
                foreach (var job in group.SelectMany(j => j.All))
                {
                    doomed.Add(job.Main.Archive.FilePath);
                    if (job.Update is { } update)
                        doomed.Add(update.Archive.FilePath);
                }

            // 输入是文件夹时，只在它下面已展开的文件全都属于这个项目时才一并移除，
            // 免得把同文件夹里的其它游戏也删掉。
            var wholeFolders = _folders.Where(folder =>
            {
                string root = Path.GetFullPath(folder).TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar)
                              + Path.DirectorySeparatorChar;
                var inside = _files.Where(f => Path.GetFullPath(f).StartsWith(root, StringComparison.OrdinalIgnoreCase)).ToArray();
                return inside.Length > 0 && inside.All(f => doomed.Contains(f));
            }).ToArray();

            _files.RemoveAll(f => doomed.Contains(f));
            _folders.RemoveAll(f => wholeFolders.Contains(f, LocalNames.PathComparer));
            // 游戏文件夹行的输入就是文件夹本身，上面的 wholeFolders 只看散装文件，
            // 对这种行永远判定不成立，点 X 后重读时它会从 _folders 里回来。
            if (row.IsFolder)
            {
                var folderPaths = new HashSet<string>(row.Files.Select(Path.GetFullPath), LocalNames.PathComparer);
                _folders.RemoveAll(f => folderPaths.Contains(Path.GetFullPath(f)));
                _gameFolders.Remove(row.Key);
            }
            _issues.RemoveAll(i => doomed.Contains(i.Path));
            _expansionIssues.RemoveAll(i => doomed.Contains(i.Path));
            _rows.RemoveAll(r => r.Key == row.Key);
            _jobs.Remove(row.Key);
            _reloadNeeded = true;
        }
        StartReading();
    }

    public void Remove(InputIssue issue)
    {
        lock (_gate)
        {
            if (_activity != SessionActivity.Idle)
                return;
            _files.RemoveAll(f => LocalNames.PathComparer.Equals(f, issue.Path));
            _expansionIssues.Remove(issue);
            _issues.Remove(issue);
            _reloadNeeded = true;
        }
        StartReading();
    }

    public void RemoveAll()
    {
        lock (_gate)
        {
            if (_activity != SessionActivity.Idle)
                return;
            _files.Clear();
            _folders.Clear();
            _gameFolders.Clear();
            _rows.Clear();
            _issues.Clear();
            _expansionIssues.Clear();
            _notes.Clear();
            _titleNotes.Clear();
            _jobs.Clear();
            _summary = null;
            _lastOutput = null;
            _bestFirst = false;
            _set?.Dispose();
            _set = null;
        }
        Notify();
    }

    /// <summary>Ticks or unticks a title for the jobs to come.</summary>
    public void Toggle(TitleRow row)
    {
        lock (_gate)
        {
            if (_activity != SessionActivity.Idle)
                return;
            int index = _rows.FindIndex(r => r.Key == row.Key);
            if (index >= 0)
                _rows[index] = _rows[index] with { Selected = !_rows[index].Selected, ChosenByHand = true };
        }
        Notify();
    }

    /// <summary>
    /// With compression off, a title only comes out any smaller by having an update made part of it, so there's nothing
    /// in it for these. They may well be ticked for something else, so nothing happens to them without asking.
    /// </summary>
    public string[] TickedWithoutUpdate()
    {
        if (Settings.Get("--nocompress") != "1")
            return [];
        lock (_gate)
            return _rows.Where(r => r.Selected && !r.HasUpdate && !r.IsFolder && !r.IsDone(BatchMode.Convert)).Select(r => r.Name).ToArray();
    }

    public void UntickWithoutUpdate()
    {
        lock (_gate)
        {
            if (_activity != SessionActivity.Idle)
                return;
            _rows = _rows.Select(r => r.Selected && !r.HasUpdate && !r.IsFolder ? r with { Selected = false, ChosenByHand = true } : r).ToList();
        }
        Notify();
    }

    /// <summary>Ticks or unticks everything. Anything the minimum saving left out counts as chosen once it's ticked here.</summary>
    public void SelectAll(bool selected)
    {
        lock (_gate)
        {
            if (_activity != SessionActivity.Idle)
                return;
            _rows = _rows.Select(r => r with { Selected = selected, ChosenByHand = true }).ToList();
        }
        Notify();
    }

    /// <summary>
    /// Call when an option changed. Finished titles can then be run again with the new option, and if it decides the
    /// size, the old estimates don't stay on show.
    /// </summary>
    public void OptionChanged(string flag)
    {
        if (flag == "--noautoupdate")
            return;
        if (flag == "--showcompressed")
        {
            Notify();
            return;
        }
        if (flag == "--hidesmall")
        {
            lock (_gate)
                _reloadNeeded = true;
            StartReading();
            return;
        }

        bool decidesSize = flag is "--nocompress" or "--level" or "--blocksize";
        bool decidesSelection = flag is "--minsaving";
        lock (_gate)
        {
            if (!decidesSelection)
            {
                _rows = _rows.Select(r => r with
                {
                    Estimate = decidesSize ? null : r.Estimate,
                    State = r.State == RowState.Done ? RowState.Ready : r.State,
                    OutputPath = r.State == RowState.Done ? null : r.OutputPath,
                }).ToList();
                _summary = null;
            }
            if (TryReadOptions() is { } options)
                _rows = _rows.Select(r => Judged(r, options)).ToList();
        }
        Notify();
    }

    private Options? TryReadOptions()
    {
        try
        {
            return Settings.ToOptions(OptionUse.Convert);
        }
        catch (ToolException)
        {
            return null;
        }
    }

    /// <summary>Holds a title against the minimum saving, leaving alone anything someone has chosen by hand.</summary>
    private static TitleRow Judged(TitleRow row, Options options)
    {
        string? reason = row.Estimate is { } estimate && !SavingRule.WorthCompressing(estimate, options) ? SavingRule.Reason(estimate, options) : null;
        return row with { BelowMinimum = reason, Selected = row.ChosenByHand ? row.Selected : reason is null };
    }

    /// <summary>The titles that gain the most for the time they take first, then any with no estimate yet.</summary>
    private static List<TitleRow> BestFirst(IEnumerable<TitleRow> rows) => rows
        .OrderBy(r => r.IsDone(BatchMode.Convert) ? 1 : 0)
        .ThenBy(r => r.Estimate is null ? 1 : 0)
        .ThenByDescending(r => r.Estimate?.SavedFraction ?? 0)
        .ThenByDescending(r => r.Estimate?.SavedBytes ?? 0)
        .ToList();

    /// <summary>Keys may have changed, so read everything again.</summary>
    public void ReloadKeys()
    {
        lock (_gate)
        {
            _keyset = null;
            _reloadNeeded = true;
        }
        StartReading();
    }

    /// <summary>False if nothing was started, because every title already has an answer or something else is running.</summary>
    public bool Analyze()
    {
        var rows = CurrentRows().Where(r => r.Selected && r.NeedsEstimate).ToList();
        if (rows.Count == 0)
            return false;
        return Begin(SessionActivity.Analyzing, () =>
        {
            var options = Settings.ToOptions(OptionUse.Convert);
            ForEachRow(rows, (jobs, row) =>
            {
                var estimate = EstimateRow(jobs, row, options);
                return r => Judged(r with { Estimate = estimate, State = row.State == RowState.Failed ? RowState.Ready : row.State }, options);
            });
            lock (_gate)
            {
                _bestFirst = true;
                _rows = BestFirst(_rows);
            }
        });
    }

    public bool Compress()
    {
        var rows = CurrentRows().Where(r => r.Selected && !r.IsDone(BatchMode.Convert)).ToList();
        if (rows.Count == 0)
            return false;
        return Begin(SessionActivity.Compressing, () =>
        {
            var options = Settings.ToOptions(OptionUse.Convert);
            var converter = new TitleConverter(options, RequireKeyset());
            ForEachRow(rows, (jobs, row) =>
            {
                // A title nobody has analyzed yet gets its answer now, so the time isn't spent on one that gains too little. What's already
                // compressed never needs doing again, whoever ticked it.
                if ((!row.ChosenByHand && SavingRule.Applies(options)) || (options.Compress && row.IsCompressed))
                {
                    var estimate = row.Estimate ?? EstimateRow(jobs, row, options);
                    if (!SavingRule.WorthCompressing(estimate, options) && (!row.ChosenByHand || estimate.AlreadyCompressed))
                    {
                        _leftOut++;
                        Ui.Warn($"Left out: {SavingRule.Reason(estimate, options)}");
                        return r => Judged(r with { Estimate = estimate, State = RowState.Ready, Failure = null }, options);
                    }
                }

                string? path = null;
                long bytes = 0;
                if (row.IsFolder)
                {
                    var builder = new Builder(FolderOptions(row), RequireKeyset());
                    builder.Run();
                    path = builder.OutputPath!;
                    bytes = new FileInfo(path).Length;
                }
                // The game first and then its DLC, each its own NSP. Whatever of it is already compressed stays as it is.
                foreach (var job in jobs)
                {
                    if (options.Compress && (job.IsOwnOutput || Analyzer.IsAlreadyCompressed(job, options)))
                    {
                        Ui.Info($"{job.DisplayName}：已压缩");
                        bytes += job.InputBytes;
                        continue;
                    }
                    Ui.Heading(job.Describe());
                    var (made, size) = converter.Run(job, _set!);
                    path ??= made;
                    bytes += size;
                }
                lock (_gate)
                    _lastOutput = path;
                return r => r with { State = RowState.Done, DoneAs = BatchMode.Convert, OutputPath = path, OutputBytes = bytes };
            }, summarize: true);

            if (options.Compress && rows.Any(r => !r.HasUpdate && !r.IsFolder && !r.IsCompressed))
                Ui.Info(TitleJob.UpdatesWontWork);

            // What's been done goes out of the way of what's still to do.
            lock (_gate)
                _rows = _bestFirst ? BestFirst(_rows) : _rows.OrderBy(r => r.IsDone(BatchMode.Convert) ? 1 : 0).ToList();
        });
    }

    public bool Extract()
    {
        var rows = CurrentRows().Where(r => r.Selected && !r.IsFolder && !r.IsDone(BatchMode.Extract)).ToList();
        if (rows.Count == 0)
            return false;
        return Begin(SessionActivity.Extracting, () =>
        {
            var extractor = new TitleExtractor(Settings.ToOptions(OptionUse.Extract));
            ForEachRow(rows, (jobs, row) =>
            {
                string? path = null;
                long bytes = 0;
                foreach (var job in jobs)
                {
                    Ui.Heading(job.Describe());
                    var (made, size) = extractor.Run(job, _set!);
                    path ??= made;
                    bytes += size;
                }
                lock (_gate)
                    _lastOutput = path;
                return r => r with { State = RowState.Done, DoneAs = BatchMode.Extract, OutputPath = path, OutputBytes = bytes };
            }, summarize: true);
        });
    }

    /// <summary>A game with its DLC comes to what they come to together. The margins are independent, so they add up as a sum of squares.</summary>
    private SizeEstimate EstimateRow(List<TitleJob> jobs, TitleRow row, Options options)
    {
        if (row.IsFolder)
            return Analyzer.EstimateBuild(FolderOptions(row));
        var each = jobs.Select(job => Analyzer.Estimate(job, options)).ToList();
        // DLC with no game data to it says nothing either way, and nor does a part with nothing to gain, such as DLC with a few hundred bytes of it.
        bool compressed = each.Any(e => e.AlreadyCompressed) && each.All(e => e.AlreadyCompressed || e.RomFsBytes == 0 || e.SavedFraction < SavingRule.NothingToGain);
        return new SizeEstimate(each.Sum(e => e.InputBytes), each.Sum(e => e.OutputBytes), (long)Math.Sqrt(each.Sum(e => (double)e.MarginBytes * e.MarginBytes)),
            each.Sum(e => e.RomFsBytes), each.Sum(e => e.RomFsStoredBytes)) { AlreadyCompressed = compressed };
    }

    /// <summary>How a game folder is built: with what's chosen for compression and where NSPs go.</summary>
    private Options FolderOptions(TitleRow row)
    {
        GameFolder folder;
        lock (_gate)
            folder = _gameFolders[row.Key];
        return folder.BuildOptions(Settings.ToOptions(OptionUse.Build));
    }

    public bool Verify(string path, string? romfsDir, string? exefsDir) => Begin(SessionActivity.Verifying, () =>
    {
        bool passed = Verifier.Check(path, RequireKeyset(), romfsDir, exefsDir) == 0;
        string? first;
        lock (_gate)
            first = _log.FirstOrDefault(l => l.Level == UiLevel.Error)?.Text;
        Finish(passed ? "全部检查通过。" : $"此 NSP 存在问题：{first ?? "未通过检查"}", passed ? UiLevel.Success : UiLevel.Error);
    });

    /// <summary>Shares every game on the list with a Switch until it's stopped, hidden ones too, as what to install is chosen on the Switch. Ticks
    /// are for what the PC does.</summary>
    public bool SendToSwitch() => Begin(SessionActivity.Sending, () =>
    {
        string[] files;
        CancellationToken token;
        var watch = new SendWatch();
        watch.Changed += Notify;
        lock (_gate)
        {
            files = _rows.SelectMany(r => r.Files).Distinct(StringComparer.OrdinalIgnoreCase).ToArray();
            token = _stop!.Token;
            _sendWatch = watch;
        }
        if (files.Length == 0)
            throw new ToolException("请先添加要发送的游戏文件。");
        Sender.Run(files, token, watch, RequireKeyset);
    });

    public void Stop()
    {
        lock (_gate)
        {
            _stop?.Cancel();
        }
    }

    /// <summary>Stops whatever is running and waits for it, so a partly written file gets cleaned up before the program ends.</summary>
    public void Dispose()
    {
        Stop();
        _worker?.Join();
        lock (_gate)
        {
            _set?.Dispose();
            _set = null;
        }
    }

    public void Message(UiLevel level, string text)
    {
        lock (_gate)
        {
            if (text.Length == 0)
                return;
            if (_log.Count >= MaxLogLines)
                _log.RemoveRange(0, MaxLogLines / 4);
            _log.Add(new LogLine(level, text));
        }
        Notify();
    }

    public void Progress(ProgressSnapshot snapshot)
    {
        lock (_gate)
        {
            _progress = snapshot;
        }
        Notify();
    }

    public void ProgressDone(string label, TimeSpan elapsed)
    {
        lock (_gate)
        {
            _progress = null;
            if (elapsed.TotalSeconds >= 1)
                _log.Add(new LogLine(UiLevel.Detail, $"{label}: 完成时间 {elapsed.TotalSeconds:0.0}s"));
        }
        Notify();
    }

    private void Notify() => Changed?.Invoke();

    /// <summary>What's on show, which is what the buttons work on.</summary>
    private List<TitleRow> CurrentRows()
    {
        lock (_gate)
            return VisibleRowsLocked();
    }

    private void StartReading()
    {
        bool needed;
        lock (_gate)
            needed = _queued.Count > 0 || _reloadNeeded;
        if (needed)
            Begin(SessionActivity.Reading, ReadInputs);
    }

    private bool Begin(SessionActivity activity, Action work)
    {
        lock (_gate)
        {
            if (_activity != SessionActivity.Idle)
                return false;
            _activity = activity;
            _progress = null;
            _currentTitle = null;
            _currentIndex = _currentCount = 0;
            if (activity != SessionActivity.Reading)
            {
                _summary = null;
                _lastOutput = null;
                _log.Clear();
            }
            _leftOut = 0;
            if (activity != SessionActivity.Reading)
                _sendWatch = null;
            _stop = new CancellationTokenSource();
            Ui.Token = _stop.Token;
        }
        Notify();

        _worker = new Thread(() => RunWorker(work)) { IsBackground = true, Name = "Job" };
        _worker.Start();
        return true;
    }

    private void RunWorker(Action work)
    {
        try
        {
            work();
        }
        catch (OperationCanceledException)
        {
            Finish(StoppedMessage(), UiLevel.Warning);
        }
        catch (AggregateException e) when (e.Flatten().InnerExceptions.All(i => i is OperationCanceledException))
        {
            Finish(StoppedMessage(), UiLevel.Warning);
        }
        catch (Exception e)
        {
            // Whatever went wrong, the window stays open and says so.
            Finish(e.Message, UiLevel.Error);
        }

        lock (_gate)
        {
            _activity = SessionActivity.Idle;
            _progress = null;
            _currentTitle = null;
            _rows = _rows.Select(r => r.State == RowState.Working ? r with { State = RowState.Ready } : r).ToList();
        }
        Notify();
        StartReading();
    }

    private string StoppedMessage()
    {
        lock (_gate)
        {
            return _activity switch
            {
                SessionActivity.Extracting => "已停止。此前已解包的文件会保留。",
                SessionActivity.Sending => "已停止向 Switch 发送。",
                SessionActivity.Compressing => "已停止。没有留下写了一半的文件。",
                _ => "已停止。",
            };
        }
    }

    private void Finish(string summary, UiLevel level)
    {
        lock (_gate)
        {
            _summary = summary;
            _summaryLevel = level;
        }
    }

    /// <param name="任务">The game and its DLC, or nothing for a game folder.</param>
    private delegate Func<TitleRow, TitleRow> RowWork(List<TitleJob> jobs, TitleRow row);

    private void ForEachRow(IReadOnlyList<TitleRow> rows, RowWork work, bool summarize = false)
    {
        var clock = Stopwatch.StartNew();
        int failures = 0;
        for (int i = 0; i < rows.Count; i++)
        {
            Ui.Token.ThrowIfCancellationRequested();
            var row = rows[i];
            List<TitleJob> jobs;
            lock (_gate)
            {
                _currentIndex = i + 1;
                _currentCount = rows.Count;
                _currentTitle = row.Name;
                jobs = _jobs.GetValueOrDefault(row.Key) ?? [];
            }
            Update(row.Key, r => r with { State = RowState.Working, Failure = null });
            if (row.IsFolder || jobs.Count != 1)
                Ui.Heading($"{row.Name}: {row.Detail}");
            try
            {
                Update(row.Key, work(jobs, row));
            }
            catch (Exception e) when (e is ToolException or IOException or UnauthorizedAccessException or CryptographicException)
            {
                failures++;
                Ui.Error($"FAILED: {e.Message}");
                string message = e.Message;
                Update(row.Key, r => r with { State = RowState.Failed, Failure = message });
            }
        }

        if (summarize)
        {
            Finish(Batch.Summary(clock.Elapsed, rows.Count, failures, _leftOut), failures == 0 ? UiLevel.Success : UiLevel.Error);
        }
    }

    private void Update(string key, Func<TitleRow, TitleRow> change)
    {
        lock (_gate)
        {
            int index = _rows.FindIndex(r => r.Key == key);
            if (index >= 0)
                _rows[index] = change(_rows[index]);
        }
        Notify();
    }

    private Keyset RequireKeyset()
    {
        lock (_gate)
        {
            if (_keyset is not null)
                return _keyset;
        }

        var keyset = Keyset.Load(Keyset.Find(Settings.ToOptions(OptionUse.Convert).KeysetPath));
        _ = keyset.HeaderKey;
        lock (_gate)
            _keyset = keyset;
        return keyset;
    }

    /// <summary>DLC on its own has no name of its own, so it goes by its file's, without what's in brackets.</summary>
    private static string DlcName(TitleJob job)
    {
        string name = Path.GetFileNameWithoutExtension(job.Main.Archive.FilePath);
        int bracket = name.IndexOf('[');
        name = (bracket > 0 ? name[..bracket] : name).Trim();
        return name.Length > 0 ? $"{name} (DLC)" : "DLC";
    }

    private void ReadInputs()
    {
        List<string> queued;
        var notes = new List<string>();
        var issues = new List<InputIssue>();
        lock (_gate)
        {
            queued = _queued.ToList();
            _queued.Clear();
            _reloadNeeded = false;
        }

        // What it's on, for the window to show it's getting somewhere: going through folders, then opening each file.
        void Reading(string what, int index, int count)
        {
            lock (_gate)
            {
                _currentTitle = what;
                _currentIndex = index;
                _currentCount = count;
            }
            Notify();
        }

        Reading("正在浏览文件夹", 0, 0);
        var foundFolders = new List<string>();
        var found = TitleSet.Expand(queued, issues, foundFolders);
        List<string> files, folders;
        lock (_gate)
        {
            foreach (string folder in foundFolders)
            {
                if (!_folders.Contains(folder, LocalNames.PathComparer))
                    _folders.Add(folder);
            }
            folders = _folders.ToList();
            _notes.AddRange(notes);
            _expansionIssues.AddRange(issues.Where(i => !_expansionIssues.Any(known => known.Path == i.Path)));
            foreach (string file in found)
            {
                if (!_files.Contains(file, LocalNames.PathComparer))
                    _files.Add(file);
            }
            files = _files.ToList();
        }

        Keyset keyset;
        try
        {
            keyset = RequireKeyset();
            lock (_gate)
                _keyProblem = null;
        }
        catch (Exception e) when (e is ToolException or IOException or UnauthorizedAccessException)
        {
            lock (_gate)
            {
                _keyProblem = e.Message;
                _rows.Clear();
                _issues.Clear();
            }
            return;
        }

        var set = TitleSet.Load(files, keyset, (done, count) => Reading("正在打开游戏文件", done, count));
        if (TryReadOptions() is { } merge && merge.MergeDlc)
            set.MergeDlcIntoBase();
        int hiddenSmall = TryReadOptions() is { } current ? set.LeaveOutSmall(current) : 0;

        // Game folders are only read again when they're new, as their size means going through every file in them.
        var opened = new Dictionary<string, GameFolder>();
        var folderProblems = new List<InputIssue>();
        foreach (string path in folders)
        {
            string key = "folder|" + Path.GetFullPath(path);
            GameFolder? known;
            lock (_gate)
                known = _gameFolders.GetValueOrDefault(key);
            if (known is null)
                Reading("正在读取" + Path.GetFileName(path), opened.Count + 1, folders.Count);
            try
            {
                opened[key] = known ?? GameFolder.Open(path);
            }
            catch (Exception e) when (e is ToolException or IOException or UnauthorizedAccessException)
            {
                folderProblems.Add(new InputIssue(path, $"'{Path.GetFileName(path)}': {e.Message}"));
            }
        }

        lock (_gate)
        {
            var previous = _rows.ToDictionary(r => r.Key);
            _set?.Dispose();
            _set = set;
            _hiddenSmall = hiddenSmall;
            _jobs.Clear();
            _rows = [];
            // A game, its update and its DLC are one row, as they are on the Switch.
            foreach (var game in set.Jobs.GroupBy(j => j.ApplicationId))
            {
                var jobs = game.OrderBy(j => j.IsBaseGame ? 0 : 1).ToList();
                var main = jobs[0];
                int dlc = jobs.Count(j => !j.IsBaseGame);
                string key = $"game|{game.Key:X16}";
                var paths = jobs.SelectMany(j => j.All).SelectMany(j => j.Update is { } update ? new[] { j.Main.Archive.FilePath, update.Archive.FilePath } : [j.Main.Archive.FilePath]).ToArray();
                bool compressed = jobs.All(j => j.IsOwnOutput);
                string what = !main.IsBaseGame ? "" : compressed || main.IsOwnOutput ? $"compressed, v{main.Main.Meta.Version}" : main.Detail;
                string detail = string.Join(", ", new[] { what, dlc == 0 ? "" : $"{dlc} 个 DLC" }.Where(s => s.Length > 0));
                var rowNotes = jobs.SelectMany(j => j.IsBaseGame || j.Notes.Count == 0 ? j.Notes : j.Notes.Select(n => $"DLC {j.Main.Meta.TitleId:X16}: {n}")).ToArray();
                var row = new TitleRow(key, main.IsBaseGame ? main.DisplayName : DlcName(main), detail, jobs.Sum(j => j.InputBytes), paths)
                {
                    HasUpdate = main.Update is not null,
                    Icon = jobs.Select(j => j.Icon).FirstOrDefault(i => i is not null),
                    Notes = rowNotes,
                    AlreadyCompressed = compressed,
                    Selected = !compressed,
                };
                if (previous.TryGetValue(key, out var known))
                    row = known with { Name = row.Name, Detail = row.Detail, InputBytes = row.InputBytes, Files = paths, Icon = row.Icon, Notes = rowNotes, AlreadyCompressed = compressed };
                _rows.Add(row);
                _jobs[key] = jobs;
            }
            _gameFolders.Clear();
            foreach (var (key, folder) in opened)
            {
                string detail = folder.Version > 0 ? $"游戏文件夹 v{folder.Version}" : "游戏文件夹";
                var row = new TitleRow(key, folder.Name, detail, folder.Bytes, [folder.Path]) { IsFolder = true, Icon = folder.Icon };
                if (previous.TryGetValue(key, out var known))
                    row = known with { Name = row.Name, Detail = row.Detail, InputBytes = row.InputBytes, Icon = row.Icon };
                _rows.Add(row);
                _gameFolders[key] = folder;
            }
            if (_bestFirst)
                _rows = BestFirst(_rows);
            _issues = set.Problems.Concat(folderProblems).ToList();
            _titleNotes = set.Notes.ToList();
            _rows = _rows.OrderBy(r => r.Name, StringComparer.CurrentCultureIgnoreCase).ToList();
            if (_bestFirst)
                _rows = BestFirst(_rows);
        }
    }
}

/// <summary>The paths in text pasted into a console, which is what dropping files on one comes down to.</summary>
internal static class PathList
{
    public static List<string> Parse(string text)
    {
        text = text.Trim();
        if (text.Length == 0)
            return [];
        if (File.Exists(text) || Directory.Exists(text))
            return [text];

        var paths = new List<string>();
        var current = new StringBuilder();
        char quote = '\0';
        for (int i = 0; i < text.Length; i++)
        {
            char c = text[i];
            if (quote != '\0')
            {
                if (c == quote)
                    quote = '\0';
                else
                    current.Append(c);
            }
            else if (c is '"' or '\'')
            {
                quote = c;
            }
            else if (c == '\\' && !OperatingSystem.IsWindows() && i + 1 < text.Length)
            {
                current.Append(text[++i]);
            }
            else if (char.IsWhiteSpace(c))
            {
                if (current.Length > 0)
                    paths.Add(current.ToString());
                current.Clear();
            }
            else
            {
                current.Append(c);
            }
        }
        if (current.Length > 0)
            paths.Add(current.ToString());
        return paths;
    }
}
