using System.Diagnostics;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;

namespace HacBrewPackLz;

/// <summary>
/// Problems the app finds itself, sent to the developer by itself when the person has agreed to it: a game a job couldn't do, the app
/// stopping on something it didn't expect, and the app not answering until it was closed. Each problem goes once a version from a device,
/// and after that it's only counted. Everything goes by way of a folder of reports waiting to go, so no internet, or the app closing as it
/// sends, loses nothing; and the app stopping, or not answering, can't send anything as it happens, so that goes the next time it opens.
/// Whether it's still wanted is asked when it goes, so turning it off in Options drops whatever is waiting. See report-service\README.md.
/// </summary>
internal static class ProblemReports
{
    public const string Flag = "--sendproblems";

    private static readonly object Gate = new();
    private static int _sending;
    private static long _alive = Environment.TickCount64;
    private static Exception? _crashed;

    /// <summary>On the command line, which keeps no settings, it's agreed to with --sendproblems each time.</summary>
    public static bool AgreedOnCommandLine { get; set; }

    /// <summary>What goes with a problem: the version, what's set, the list and what's been said, with no keys in it.</summary>
    public static Func<string>? ReportText { get; set; }

    /// <summary>For the tests, which make it crash or stop answering on purpose.</summary>
    public static string TestProblem => Environment.GetEnvironmentVariable("HACBREWPACKLZ_TEST_PROBLEM") ?? "";

    /// <summary>Not answering for less than this is only something being slow.</summary>
    private static TimeSpan FreezeAfter => TimeSpan.FromSeconds(TestProblem == "freeze" ? 3 : 30);

    /// <summary>Read from the settings each time, as Options may have changed it since.</summary>
    public static bool Wanted => AgreedOnCommandLine || Settings.Load().Get(Flag) == "1";

    private static string Waiting => Path.Combine(Settings.Folder, "reports waiting");
    private static string Ledger => Path.Combine(Settings.Folder, "problems sent.txt");
    private static string StalledNote(int process) => Path.Combine(Settings.Folder, $"not answering {process}.txt");

    /// <summary>When the app opens: a freeze the last time that it never came out of joins whatever is waiting, and it all goes, beside
    /// whatever the app does next.</summary>
    public static void Opened()
    {
        try
        {
            if (Directory.Exists(Settings.Folder))
            {
                foreach (string path in Directory.GetFiles(Settings.Folder, "not answering *.txt"))
                {
                    string name = Path.GetFileNameWithoutExtension(path);
                    if (int.TryParse(name["not answering ".Length..], out int process) && StillRunning(process))
                        continue;
                    string note = File.ReadAllText(path);
                    File.Delete(path);
                    int end = note.IndexOf('\n');
                    Found("freeze", end < 0 ? note : note[..end].TrimEnd(), note);
                }
            }
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
        }
        SendWaiting(inBackground: true);
    }

    /// <summary>A game a job couldn't do. It goes straight away, beside the job, unless it's to wait for the end of it.</summary>
    public static void JobFailed(string message, bool sendNow = true)
    {
        Found("failed-job", message, null);
        if (sendNow)
            SendWaiting(inBackground: true);
    }

    /// <summary>For the tests: the problem asked for, on the screen's own thread.</summary>
    public static void MakeTestProblem()
    {
        if (TestProblem == "crash")
            throw new InvalidOperationException("A crash on purpose, for the tests");
        if (TestProblem == "freeze")
            Thread.Sleep(Timeout.Infinite);
    }

    /// <summary>The app stopping on something it didn't expect. It can't go now, so it goes the next time the app opens.</summary>
    public static void Crashed(Exception e)
    {
        // The same one can come by two ways, as it can on Android.
        if (ReferenceEquals(Interlocked.Exchange(ref _crashed, e), e))
            return;
        try
        {
            string where = string.Join(" < ", OwnFrames(e).Take(3));
            Found("crash", $"{e.GetType().Name}: {e.Message}{(where.Length > 0 ? " in " + where : "")}", $"{e}{Environment.NewLine}{Environment.NewLine}{CurrentReport()}");
        }
        catch (Exception)
        {
            // Stopping already, so whatever goes wrong here can only be let go.
        }
    }

    /// <summary>
    /// Watches the screen answering, beside it. One that hasn't for a while leaves a note saying what it was doing, which only becomes a
    /// report if the app is then closed without coming out of it, as a slow disk that catches up isn't a problem.
    /// </summary>
    /// <param name="answering">Whether the screen is answering now. Asked once a second.</param>
    /// <param name="doing">What the app is doing, in a few words, such as "compressing".</param>
    public static void WatchForFreezes(Func<bool> answering, Func<string> doing)
    {
        new Thread(() =>
        {
            string note = StalledNote(Environment.ProcessId);
            long since = 0;
            bool noted = false;
            while (true)
            {
                Thread.Sleep(1000);
                bool fine;
                try
                {
                    fine = answering();
                }
                catch (Exception)
                {
                    fine = true;
                }
                if (fine)
                {
                    since = 0;
                    if (noted)
                    {
                        Delete(note);
                        noted = false;
                    }
                    continue;
                }
                if (since == 0)
                    since = Environment.TickCount64;
                if (!noted && Environment.TickCount64 - since >= FreezeAfter.TotalMilliseconds && Wanted)
                {
                    noted = true;
                    // Neither of these may wait on what's stuck, so they get a moment each and no more.
                    string what = WithinMoment(doing) ?? "";
                    string report = WithinMoment(() => ReportText?.Invoke() ?? "") ?? "";
                    try
                    {
                        Directory.CreateDirectory(Settings.Folder);
                        File.WriteAllText(note, $"Stopped answering{(what.Length > 0 ? " while " + what : "")}{Environment.NewLine}{Environment.NewLine}{report}");
                    }
                    catch (Exception e) when (e is IOException or UnauthorizedAccessException)
                    {
                    }
                }
            }
        }) { IsBackground = true, Name = "Watching for freezes" }.Start();
    }

    /// <summary>The screen's own loop saying it's still going, for one that's watched that way.</summary>
    public static void Alive() => Volatile.Write(ref _alive, Environment.TickCount64);

    public static bool AnsweredLately() => Environment.TickCount64 - Volatile.Read(ref _alive) < 5000;

    /// <summary>
    /// What's waiting goes, then how many more times the problems sent before were seen. With nowhere to send it, or no internet, it waits
    /// for next time; when sending isn't wanted any more, it's dropped.
    /// </summary>
    public static void SendWaiting(bool inBackground)
    {
        if (inBackground)
        {
            new Thread(() =>
            {
                try
                {
                    SendWaiting(false);
                }
                catch (Exception)
                {
                    // Whatever it was, sending problems mustn't become one: what's waiting goes another time.
                }
            }) { IsBackground = true, Name = "Sending problems" }.Start();
            return;
        }
        if (Interlocked.Exchange(ref _sending, 1) == 1)
            return;
        try
        {
            if (!Wanted)
            {
                Drop();
                return;
            }
            if (!ReportSender.CanSend)
                return;
            string[] files = Directory.Exists(Waiting) ? Directory.GetFiles(Waiting, "*.json") : [];
            foreach (string path in files.OrderBy(File.GetLastWriteTimeUtc))
            {
                ReportSender.Tried tried;
                try
                {
                    using var document = JsonDocument.Parse(File.ReadAllText(path));
                    var r = document.RootElement;
                    string Get(string name) => r.TryGetProperty(name, out var value) && value.ValueKind == JsonValueKind.String ? value.GetString() ?? "" : "";
                    tried = ReportSender.TrySend(Get("kind"), Get("fingerprint"), Get("title"), "", Get("report"), Get("version"));
                }
                catch (JsonException)
                {
                    tried = new ReportSender.Tried(null, "it wasn't a report", false);
                }
                if (tried.Sent is null && tried.TryAgain)
                    return;
                Delete(path);
            }

            // Then the counts, each taken off once it's dealt with, as more may have been seen meanwhile.
            foreach (var (version, fingerprint, count) in ReadLedger().Where(e => e.Count > 0).ToList())
            {
                if (!ReportSender.Seen(fingerprint, version, count))
                    return;
                lock (Gate)
                    WriteLedger(ReadLedger().Select(e => e.Version == version && e.Fingerprint == fingerprint ? e with { Count = Math.Max(e.Count - count, 0) } : e));
            }
            // What other versions saw is done with once it's gone.
            lock (Gate)
                WriteLedger(ReadLedger().Where(e => e.Version == AppInfo.Version || e.Count > 0));
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
        }
        finally
        {
            Volatile.Write(ref _sending, 0);
        }
    }

    /// <summary>
    /// The words it's known by, without what's particular to one person's files: names in quotes, paths, title IDs and numbers, other than the
    /// error codes that say which problem it is. Problems that are the same come out the same, which is what groups them.
    /// </summary>
    public static string Normalized(string message)
    {
        string s = ReportSender.Prepared(message);
        // A quote starts and ends a name only away from letters, as in "couldn't" or a name like 'Assassin's Creed' it's an apostrophe.
        s = Regex.Replace(s, "(?<![\\p{L}\\d])'.+?'(?![\\p{L}\\d])|\"[^\"]*\"|“[^”]*”", "'...'");
        s = Regex.Replace(s, @"(?:[A-Za-z]:|~|\\\\[^\\\s]+)[\\/][^\s,;']*|(?:/[^\s/,;']+){2,}", "...");
        s = Regex.Replace(s, @"\b[0-9A-Fa-f]{16}\b", "<title ID>");
        s = Regex.Replace(s, @"\b\d{4}-\d{4}\b|0x[0-9A-Fa-f]+|\d+", m => m.Value.Contains('-') || m.Value.StartsWith("0x", StringComparison.Ordinal) ? m.Value : "#");
        s = Regex.Replace(s, @"\s+", " ").Trim();
        return s.Length > 200 ? s[..200] : s;
    }

    /// <summary>
    /// A problem found: once a version, it's put with what's waiting to go, and after that it's only counted. Nothing is kept when sending
    /// isn't wanted.
    /// </summary>
    private static void Found(string kind, string message, string? report)
    {
        if (!Wanted)
            return;
        string title = Normalized(message);
        string fingerprint = $"{kind}|{Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(title)))[..12].ToLowerInvariant()}";
        lock (Gate)
        {
            var ledger = ReadLedger().ToList();
            int at = ledger.FindIndex(e => e.Version == AppInfo.Version && e.Fingerprint == fingerprint);
            if (at >= 0)
            {
                ledger[at] = ledger[at] with { Count = ledger[at].Count + 1 };
                WriteLedger(ledger);
                return;
            }
            ledger.Add(new Entry(AppInfo.Version, fingerprint, 0));
            WriteLedger(ledger);
            try
            {
                Directory.CreateDirectory(Waiting);
                var body = new MemoryStream();
                using (var json = new Utf8JsonWriter(body))
                {
                    json.WriteStartObject();
                    json.WriteString("kind", kind);
                    json.WriteString("fingerprint", fingerprint);
                    json.WriteString("title", title);
                    json.WriteString("version", AppInfo.Version);
                    json.WriteString("report", ReportSender.Prepared(report ?? $"{message}{Environment.NewLine}{Environment.NewLine}{CurrentReport()}"));
                    json.WriteEndObject();
                }
                File.WriteAllBytes(Path.Combine(Waiting, fingerprint.Replace('|', '-') + ".json"), body.ToArray());
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException)
            {
            }
        }
    }

    private readonly record struct Entry(string Version, string Fingerprint, int Count);

    private static IEnumerable<Entry> ReadLedger()
    {
        try
        {
            if (!File.Exists(Ledger))
                return [];
            return File.ReadAllLines(Ledger).Select(line => line.Split('\t')).Where(p => p.Length == 3 && int.TryParse(p[2], out _))
                .Select(p => new Entry(p[0], p[1], int.Parse(p[2]))).ToList();
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
            return [];
        }
    }

    private static void WriteLedger(IEnumerable<Entry> entries)
    {
        try
        {
            Directory.CreateDirectory(Settings.Folder);
            File.WriteAllLines(Ledger, entries.Select(e => $"{e.Version}\t{e.Fingerprint}\t{e.Count}"));
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
        }
    }

    /// <summary>Sending was turned off: nothing waits to go, and nothing more is counted.</summary>
    private static void Drop()
    {
        try
        {
            if (Directory.Exists(Waiting))
                Directory.Delete(Waiting, recursive: true);
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
        }
        lock (Gate)
        {
            if (File.Exists(Ledger))
                WriteLedger(ReadLedger().Select(e => e with { Count = 0 }));
        }
    }

    private static string CurrentReport() => WithinMoment(() => ReportText?.Invoke() ?? "") ?? "";

    /// <summary>The answer, if it comes within a moment. Something stuck mustn't hold up whatever asks.</summary>
    private static string? WithinMoment(Func<string> ask)
    {
        try
        {
            var task = Task.Run(ask);
            return task.Wait(TimeSpan.FromSeconds(2)) ? task.Result : null;
        }
        catch (Exception)
        {
            return null;
        }
    }

    /// <summary>The app's own methods the exception went through, innermost first, such as "Session.ForEachRow".</summary>
    private static IEnumerable<string> OwnFrames(Exception e)
    {
        foreach (string line in (e.StackTrace ?? "").Split('\n'))
        {
            var match = Regex.Match(line, @"at HacBrewPackLz\.([\w.<>`]+)\(");
            if (match.Success)
                yield return match.Groups[1].Value;
        }
    }

    /// <summary>Another copy of the app, still open, which the note belongs to.</summary>
    private static bool StillRunning(int process)
    {
        if (process == Environment.ProcessId)
            return true;
        try
        {
            // The GUI and the home screen are programs of their own, with the same name to begin with.
            using var other = Process.GetProcessById(process);
            return !other.HasExited && other.ProcessName.StartsWith("hacbrewpacklz", StringComparison.OrdinalIgnoreCase);
        }
        catch (Exception e) when (e is ArgumentException or InvalidOperationException or System.ComponentModel.Win32Exception or NotSupportedException or PlatformNotSupportedException)
        {
            return false;
        }
    }

    private static void Delete(string path)
    {
        try
        {
            File.Delete(path);
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
        }
    }
}
