using System.Security.Cryptography;

namespace HacBrewPackLz;

/// <summary>
/// A game opened for Details and Look inside on a thread of its own, which then reads what's asked of it, one thing at a time and the newest
/// first: what's under the pointer, to show it, or something to save. A preview given up on as the pointer moves on is dropped, and saving can
/// be stopped. The same in every version, as the Switch's reader thread is (App::insideReaderFor).
/// </summary>
internal sealed class InsideReader : IDisposable
{
    private readonly object _gate = new();
    private readonly CancellationTokenSource _closing = new();
    private readonly Thread _thread;
    private CancellationTokenSource? _previewing;
    private CancellationTokenSource? _saving;
    private int _want = -1;
    private bool _wantBytes;
    private (int index, string folder, bool chosen)? _save;

    /// <summary>Anything changed: it opened, a preview came, saving got further. From the reader's thread.</summary>
    public event Action? Changed;

    public string Name { get; }
    /// <summary>The game, once it's open, and why it couldn't be, if it couldn't.</summary>
    public GameInside? Inside { get; private set; }
    public string? Problem { get; private set; }
    public bool Opening { get; private set; } = true;

    /// <summary>What's shown for what was last asked for, and which that was: a preview of something else is never shown for it.</summary>
    public GameInside.Preview? Preview { get; private set; }
    public int PreviewOf { get; private set; } = -1;
    public bool PreviewAsBytes { get; private set; }

    /// <summary>How saving is going, and where the last thing saved went, or why it couldn't be saved.</summary>
    public bool Saving { get; private set; }
    public long SavedBytes { get; private set; }
    public long SavingBytes { get; private set; }
    public int SavedFiles { get; private set; }
    public int SavingFiles { get; private set; }
    public string? SavedTo { get; private set; }
    public string? SaveProblem { get; private set; }

    public InsideReader(string name, IReadOnlyList<string> files, Func<Keyset> keys)
    {
        Name = name;
        _thread = new Thread(() => Run(files, keys)) { IsBackground = true, Name = "InsideReader" };
        _thread.Start();
    }

    /// <summary>Shows this, as it is or as bytes, once it's read. Whatever was being read to be shown before is given up.</summary>
    public void Want(int index, bool asBytes)
    {
        lock (_gate)
        {
            if (_want == index && _wantBytes == asBytes)
                return;
            _want = index;
            _wantBytes = asBytes;
            _previewing?.Cancel();
            Monitor.PulseAll(_gate);
        }
    }

    /// <summary>Saves a file, or all of a folder, to Files from games in that folder, in a folder named after the game, or straight into a
    /// folder that was chosen for it.</summary>
    public void Save(int index, string filesFromGames, bool chosenFolder = false)
    {
        lock (_gate)
        {
            if (Saving)
                return;
            _save = (index, filesFromGames, chosenFolder);
            Saving = true;
            SavedBytes = SavingBytes = 0;
            SavedFiles = SavingFiles = 0;
            SavedTo = SaveProblem = null;
            _previewing?.Cancel();
            Monitor.PulseAll(_gate);
        }
        Changed?.Invoke();
    }

    public void StopSaving()
    {
        lock (_gate)
            _saving?.Cancel();
    }

    /// <summary>Closes it on a thread of its own: closing waits for the reader to give up what it's doing, such as opening a game while
    /// measuring reads the same disk, which isn't for the screen to wait on.</summary>
    public void CloseInBackground() => new Thread(Dispose) { IsBackground = true, Name = "Closing a game" }.Start();

    public void Dispose()
    {
        lock (_gate)
        {
            _closing.Cancel();
            _previewing?.Cancel();
            _saving?.Cancel();
            Monitor.PulseAll(_gate);
        }
        _thread.Join();
        Inside?.Dispose();
    }

    private void Run(IReadOnlyList<string> files, Func<Keyset> keys)
    {
        try
        {
            // For the tests: an opening as slow as one sharing the disk with measuring, which no cancel cuts short.
            if (int.TryParse(Environment.GetEnvironmentVariable("HACBREWPACKLZ_TEST_SLOW_OPEN"), out int slow))
                Thread.Sleep(slow);
            Inside = GameInside.Open(Name, files, keys(), _closing.Token);
        }
        catch (Exception e) when (e is ToolException or IOException or UnauthorizedAccessException or CryptographicException or OperationCanceledException)
        {
            if (_closing.IsCancellationRequested)
                return;
            Problem = Failures.Describe(e);
        }
        Opening = false;
        Changed?.Invoke();
        if (Inside is null)
            return;

        while (true)
        {
            int want;
            bool asBytes;
            (int index, string folder, bool chosen)? save;
            CancellationTokenSource token;
            lock (_gate)
            {
                while (!_closing.IsCancellationRequested && _save is null && (_want < 0 || (_want == PreviewOf && _wantBytes == PreviewAsBytes)))
                    Monitor.Wait(_gate);
                if (_closing.IsCancellationRequested)
                    return;
                (want, asBytes, save) = (_want, _wantBytes, _save);
                _save = null;
                token = CancellationTokenSource.CreateLinkedTokenSource(_closing.Token);
                if (save is null)
                    _previewing = token;
                else
                    _saving = token;
            }
            try
            {
                if (save is { } saving)
                    SaveNow(saving.index, saving.folder, saving.chosen, token.Token);
                else
                    ShowNow(want, asBytes, token.Token);
            }
            finally
            {
                lock (_gate)
                {
                    _previewing = _previewing == token ? null : _previewing;
                    _saving = _saving == token ? null : _saving;
                }
                token.Dispose();
            }
        }
    }

    private void ShowNow(int index, bool asBytes, CancellationToken token)
    {
        GameInside.Preview preview;
        try
        {
            preview = Inside!.Show(index, asBytes, token);
        }
        catch (OperationCanceledException)
        {
            return;
        }
        catch (Exception e) when (e is ToolException or IOException or CryptographicException)
        {
            preview = new GameInside.Preview { Kind = "Unreadable", Chips = [Help.PlainReason(Failures.Describe(e), null)] };
        }
        catch (OutOfMemoryException)
        {
            preview = new GameInside.Preview { Kind = "Too big to show", Chips = ["There isn't enough memory to show it"] };
        }
        lock (_gate)
        {
            // Moved on from since, or asked to be shown another way.
            if (_want != index || _wantBytes != asBytes)
                return;
            Preview = preview;
            PreviewOf = index;
            PreviewAsBytes = asBytes;
        }
        Changed?.Invoke();
    }

    private void SaveNow(int index, string folder, bool chosen, CancellationToken token)
    {
        try
        {
            string saved = Inside!.Save(index, folder, (bytes, total, files, count) =>
            {
                (SavedBytes, SavingBytes, SavedFiles, SavingFiles) = (bytes, total, files, count);
                Changed?.Invoke();
            }, token, chosen);
            SavedTo = saved;
            Ui.Info($"已把 {Name} 内部的 {Inside.PathOf(index)} 保存到 {saved}");
        }
        catch (OperationCanceledException)
        {
            SaveProblem = "Stopped saving";
        }
        catch (Exception e) when (e is ToolException or IOException or UnauthorizedAccessException or CryptographicException)
        {
            SaveProblem = "Couldn't save it: " + Help.PlainReason(Failures.Describe(e), null);
        }
        Saving = false;
        Changed?.Invoke();
        // What was under the pointer is shown again, as saving took the reader from it.
        lock (_gate)
        {
            PreviewOf = -1;
            Monitor.PulseAll(_gate);
        }
    }
}
