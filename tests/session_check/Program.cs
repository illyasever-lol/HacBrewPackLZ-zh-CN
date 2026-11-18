// Checks the list the GUI, the home screen and Android share against itself, rather than against what someone thought to write down.
// Settings get changed, games ticked, analyzed and compressed, in an order that comes from a seed, and after each step:
//   - a fresh session, with the same files and settings, has to show the same list, as anything else is something the running one
//     didn't keep up with;
//   - what a row says it comes to is checked against what converting it really comes to;
//   - what's asked about games, such as their gaining nothing, is checked against what their rows say;
//   - a job shows only its games, with what came of each, and going back to the list from it puts what it compressed under Done.
//
//   session_check <prod.keys> <work folder> <seed> <steps> <game files or folders>...
//
// It uses its own settings, in the work folder, and only writes there.
namespace HacBrewPackLz;

internal sealed class QuietSink : IUiSink
{
    public TimeSpan ProgressInterval => TimeSpan.FromSeconds(1);
    public void Message(UiLevel level, string text) { }
    public void Progress(ProgressSnapshot snapshot) { }
    public void ProgressDone(string label, TimeSpan elapsed) { }
}

internal static class Program
{
    private static int s_failures;
    private static int s_passes;
    private static string s_work = "";
    private static string[] s_inputs = [];
    private static Keyset? s_keyset;
    private static readonly Dictionary<string, long> s_realSizes = [];
    /// <summary>What of it was found held again while compressing, which no estimate knows of.</summary>
    private static readonly Dictionary<string, long> s_foundSizes = [];

    private static void Report(string name, bool ok, string detail = "")
    {
        if (ok)
        {
            s_passes++;
            Console.WriteLine($"PASS  {name}");
        }
        else
        {
            s_failures++;
            Console.WriteLine($"FAIL  {name}  {detail}");
        }
    }

    private static int Main(string[] args)
    {
        if (args.Length == 1 && args[0] == "versions")
            return CheckVersions();
        if (args.Length == 1 && args[0] == "plain")
            return CheckPlainReasons();
        if (args.Length == 3 && args[0] == "names")
            return CheckNames(Path.GetFullPath(args[1]), Path.GetFullPath(args[2]));
        if (args.Length == 3 && args[0] == "answers")
            return CheckAnswers(Path.GetFullPath(args[1]), Path.GetFullPath(args[2]));
        if (args.Length < 5)
        {
            Console.WriteLine("session_check <prod.keys> <work folder> <seed> <steps> <game files or folders>...");
            return 2;
        }
        string keys = Path.GetFullPath(args[0]);
        s_work = Path.GetFullPath(args[1]);
        int seed = int.Parse(args[2]);
        int steps = int.Parse(args[3]);
        s_inputs = args[4..].Select(Path.GetFullPath).ToArray();
        if (Directory.Exists(s_work))
            Directory.Delete(s_work, true);
        Directory.CreateDirectory(s_work);
        string settings = Path.Combine(s_work, "settings.txt");
        Environment.SetEnvironmentVariable("HACBREWPACKLZ_SETTINGS", settings);
        // Nowhere to send reports, so the checker never reaches the real report service.
        Environment.SetEnvironmentVariable("HACBREWPACKLZ_REPORT_SITE", "none");

        var random = new Random(seed);
        string[] minimums = ["Off", "5%", "10%"];
        var lines = new List<string> { "welcomed=1", $"--keyset={keys}", "--noautoupdate=1", $"--nspdir={Path.Combine(s_work, "out")}", $"--outdir={Path.Combine(s_work, "extracted")}",
            $"--minsaving={minimums[random.Next(3)]}" };
        if (random.Next(2) == 0)
            lines.Add("--nocompress=1");
        if (random.Next(2) == 0)
            lines.Add("--showcompressed=1");
        // A thorough analysis, as it is to begin with, reads every game in full after its first look, or quick, only the first look.
        if (random.Next(2) == 0)
            lines.Add("--measure=0");
        File.WriteAllLines(settings, lines);
        s_keyset = Keyset.Load(keys);
        CheckDlcInGameFile(keys, settings);

        var session = new Session();
        Ui.Sink = session;
        session.AddInputs(s_inputs);
        WaitUntilIdle(session);
        Console.WriteLine($"Seed {seed}, starting with {Describe(session)}");
        CheckFailingJob(session);
        CheckEveryEstimate(session);
        CheckUnusableAlone(session);
        int jobs = 0;
        for (int i = 1; i <= steps; i++)
        {
            string did = Step(session, random, ref jobs);
            WaitUntilIdle(session);
            bool compressingNow = session.Settings.Get("--nocompress") != "1";
            foreach (var row in Everything(session).Where(r => r.IsDone(BatchMode.Convert)))
            {
                s_made.Add(row.Key);
                if (compressingNow)
                    s_compressed.Add(row.Key);
            }
            string step = $"step {i} ({did}; {Describe(session)})";
            Console.WriteLine(step);
            var running = session.Snapshot();
            if (running.ShowingJob)
            {
                CheckJob(running, step);
                // Half the time the next step is taken from what the job came to, as on the screen anything can be pressed there too.
                if (random.Next(2) == 0)
                {
                    session.BackToList();
                    CheckBack(session, running.Rows, step);
                    running = session.Snapshot();
                }
            }
            if (!running.ShowingJob)
                Compare(running, Everything(session), Fresh(session, running), step);
            CheckViews(session, step);
            CheckAsked(session, step);
            CheckSizes(session, running, step);
            Ui.Sink = session;
        }
        session.Dispose();
        Console.WriteLine($"{s_passes} passed, {s_failures} failed");
        return s_failures > 0 ? 1 : 0;
    }

    /// <summary>A test version comes after the test versions before it and before its release, which comes after every one of them.</summary>
    /// <summary>What a failed game's line under its name says, in a few plain words worked out from the failure (Help.PlainReason), for failures
    /// as testers' logs and the tests have had them: the same as the Switch's (app_test's plainReasons), with the info button where it says.</summary>
    /// <summary>
    /// What the PC answers the Switch about a game folder it shares: a first look, and measured when the Switch asks for that, so the Switch
    /// doesn't read all of it across the network itself. A folder under 1 GB, as the Switch lists it, is measured exactly, with no margin.
    ///   session_check answers &lt;prod.keys&gt; &lt;game folder&gt;
    /// </summary>
    private static int CheckAnswers(string keys, string folder)
    {
        Ui.Sink = new QuietSink();
        var answers = new SendAnswers(inside => inside == "game" ? folder : null, () => Keyset.Load(keys));
        var options = new Options();
        string[] Ask(bool measure)
        {
            // As the Switch asks: level, block size, compressing, the game, and 1 to have it measured, then where the game is.
            string request = $"{options.Level}\t{options.BlockSize}\t1\t0{(measure ? "\t1" : "")}\ngame";
            var clock = System.Diagnostics.Stopwatch.StartNew();
            var answer = answers.Estimate(request);
            while (!answer.Ready && clock.Elapsed < TimeSpan.FromMinutes(5))
            {
                Thread.Sleep(100);
                answer = answers.Estimate(request);
            }
            return answer.Done ? System.Text.Encoding.UTF8.GetString(answer.Data).Split(' ') : [];
        }
        var first = Ask(false);
        Report("a game folder's first look is answered, and says it isn't measured", first.Length >= 7 && first[6] == "0", string.Join(' ', first));
        var measured = Ask(true);
        Report("asked for it measured, the PC measures it rather than leave it to the Switch", measured.Length >= 7 && measured[6] == "1", string.Join(' ', measured));
        Report("exactly, as it's under 1 GB: with no margin", measured.Length >= 7 && measured[2] == "0", string.Join(' ', measured));
        Console.WriteLine($"{s_passes} passed, {s_failures} failed");
        return s_failures > 0 ? 1 : 0;
    }

    private static int CheckPlainReasons()
    {
        (string failure, string said)[] seen =
        [
            ("f110b369185b0cf03a501931939e5dd1.nca section 1: hash check failed at offset 0x12e0000. The file is damaged. Dump or download it again, " +
             "and check that an update is for this game's version.", "Damaged: dump or download it again."),
            ("8d5340a7ab803fef0a84f3cb8da468fb.nca section 1: hash check failed at offset 0x20c96c000. The file is damaged, or this update doesn't belong " +
             "to this base game. If the game was compressed with an earlier update in it before version 1.5, or its developer compressed it, the " +
             "original game is needed for this update.", "Damaged, or its update is for another version: check both."),
            ("Program: the update needs a part of the original game that the compressed game doesn't have", "This update needs the original game."),
            ("Program: the update needs a part of the original game that the repacked game doesn't have", "This update needs the original game."),
            ("Program: the update needs a part of the original game that the compressed copy doesn't have", "This update needs the original game."),
            ("Couldn't create 'sdmc:/config/hacBrewPackLZ/analysis.txt' (2001-0123)", "The SD card stopped answering. Open the app again."),
            ("Couldn't create 'sdmc:/switch/hacBrewPackLZ/journal/journal.new' (2001-0106)", "The SD card was busy. Try again."),
            ("'Game.nsp' isn't a readable NCA3. Your keys may not be your console's, or may be damaged: dump them again with Lockpick_RCM.", "Your keys can't open this game."),
            ("key_area_key_application_10 is not in the keyset. Update the Switch's system version, then dump your keys again with Lockpick_RCM.", "Your keys are too old for this game."),
            ("0100A00019C1A000 needs a title key, and its ticket isn't there", "This game's ticket is missing."),
            ("There is not enough space on the disk.", "There isn't enough free space."),
            ("There isn't enough free memory to decode this file: it needs 2.00 GB", "There isn't enough memory."),
            ("'Game.nsp' is cut short: a partition lies outside the file", "Damaged: dump or download it again."),
            ("Its DLC 0100ABCD12341001 in 'dlc.nsp': f110b369185b0cf03a501931939e5dd1.nca section 0: hash check failed at offset 0x10000. The file is damaged",
             "A DLC is damaged: get it again, or turn off Game and DLC in one file."),
            ("Its DLC 0100ABCD12341001 in 'dlc.nsp': 0100ABCD12341001 needs a title key, and its ticket isn't there", "This game's ticket is missing."),
            ("Its DLC 0100ABCD12341001 in 'dlc.nsp': Couldn't read 'sdmc:/games/dlc.nsp' (2002-2001)", "The SD card couldn't be reached. Check it's in properly."),
            ("Cannot create 'E:\\\\out\\\\settings.txt' because a file or directory with the same name already exists.", "Something went wrong. The info button says what."),
            ("Couldn't open 'sdmc:/games/Game.nsp' (2002-0001)", "A file it needed isn't there any more."),
            ("Couldn't delete 'sdmc:/games/Game.nsp' (2002-0007)", "A file it needed was in use. Try again."),
            ("Couldn't write to 'sdmc:/switch/hacBrewPackLZ/x.nsp': the card may be full (2002-0030)", "There isn't enough free space."),
            ("Couldn't mount the new copy (2002-1004)", "This game's ticket is missing."),
            ("Couldn't read 'sdmc:/games/Game.nsp' (2002-2001)", "The SD card couldn't be reached. Check it's in properly."),
            ("Couldn't read the game card's game (2002-2520)", "The game card couldn't be read. Check it's in properly."),
            ("Couldn't open a game's record (2002-3201)", "There isn't enough memory."),
            ("Couldn't write the new copy (2002-3501)", "The console's own storage couldn't be reached. Restart it."),
            ("Couldn't read 'sdmc:/games/Game.nsp' (2002-4687)", "The SD card's file system is damaged. Check it on a PC."),
            ("Couldn't read the installed game (2002-4517)", "Damaged: dump or download it again."),
            ("Couldn't rename 'sdmc:/games/a.nsp' (2002-0002)", "Something went wrong. The info button says what."),
            ("This console can't start compressed games yet, so nothing was changed. It needs sigpatches, and system version 12.0.0 or newer. The firmware reads /random.bin differently from what was written, so the original stays", "This console can't start compressed games yet."),
            ("This console can't start compressed games yet, so nothing was changed. It needs sigpatches, and system version 12.0.0 or newer. Couldn't mount the test copy (2002-4604)", "This console can't start compressed games yet."),
            ("This console's firmware won't read compressed games, so nothing was changed. It needs firmware 12.0.0 or newer and sigpatches. Couldn't mount the test copy (2002-4604)", "This console can't start compressed games yet."),
        ];
        foreach (var (failure, said) in seen)
        {
            string got = Help.PlainReason(failure, "The info button says what.");
            Report($"\"{failure[..Math.Min(50, failure.Length)]}...\" says \"{said}\"", got == said, $"says \"{got}\"");
        }
        CheckSendingWhileShown();
        Console.WriteLine($"{s_passes} passed, {s_failures} failed");
        return s_failures > 0 ? 1 : 0;
    }

    /// <summary>Holds each line said until it's let go, as the list does while the window has it and is reading how sending is going.</summary>
    private sealed class HoldingSink(ManualResetEventSlim saying, ManualResetEventSlim letGo) : IUiSink
    {
        public TimeSpan ProgressInterval => TimeSpan.FromSeconds(1);
        public void Message(UiLevel level, string text)
        {
            saying.Set();
            letGo.Wait(10000);
        }
        public void Progress(ProgressSnapshot snapshot) { }
        public void ProgressDone(string label, TimeSpan elapsed) { }
    }

    /// <summary>
    /// A line about sending is said with nothing of the watch held. The list takes its own lock to keep a line, and the window holds that lock
    /// while it reads the watch, so a line said from inside the watch's lock left the two waiting on each other for good. Sending a game with
    /// its update, the Windows app froze that way and the Switch heard nothing more from it.
    /// </summary>
    private static void CheckSendingWhileShown()
    {
        var watch = new SendWatch();
        using var saying = new ManualResetEventSlim();
        using var letGo = new ManualResetEventSlim();
        var before = Ui.Sink;
        Ui.Sink = new HoldingSink(saying, letGo);
        try
        {
            var reading = Task.Run(() => watch.Read("game.nsp", 1 << 20, 1 << 30));
            bool said = saying.Wait(5000);
            var shown = Task.Run(() => watch.Snapshot());
            bool answered = shown.Wait(3000);
            letGo.Set();
            bool finished = reading.Wait(5000) && shown.Wait(5000);
            Report("sending: the window reads how it's going while a line about it is being said", said && answered && finished,
                $"said {said}, answered {answered}, finished {finished}");
        }
        finally
        {
            letGo.Set();
            Ui.Sink = before;
        }
    }

    /// <summary>
    /// A file saved where one of that name is never goes over it: it takes " (2)", " (3)" and on, a game folder built beside the game it was
    /// extracted from included. What a copy was saved as, numbered or not, is still known as this tool's.
    /// </summary>
    private static int CheckNames(string gameFolder, string work)
    {
        if (Directory.Exists(work))
            Directory.Delete(work, true);
        Directory.CreateDirectory(work);
        string first = Path.Combine(work, "Game [0100ABCD12340000][v0][LZ4].nsp");
        Report("a name nothing has is used as it is", LocalNames.FreePath(first) == first);
        File.WriteAllText(first, "somebody else's");
        string second = Path.Combine(work, "Game [0100ABCD12340000][v0][LZ4] (2).nsp");
        Report("one that's taken gets (2)", LocalNames.FreePath(first) == second, LocalNames.FreePath(first));
        File.WriteAllText(second, "and another");
        Report("and then (3)", LocalNames.FreePath(first) == Path.Combine(work, "Game [0100ABCD12340000][v0][LZ4] (3).nsp"), LocalNames.FreePath(first));
        Directory.CreateDirectory(Path.Combine(work, "Folder.nsp"));
        Report("a folder of that name counts as taken", LocalNames.FreePath(Path.Combine(work, "Folder.nsp")) == Path.Combine(work, "Folder (2).nsp"));
        Report("what a numbered copy was saved as", LocalNames.WithoutCopyNumber("DLC [LZ4] (2).nsp") == "DLC [LZ4].nsp");
        Report("and one from before 1.5", LocalNames.WithoutCopyNumber("DLC [LZ4] (converted).nsp") == "DLC [LZ4].nsp");
        Report("a name with brackets of its own stays as it is", LocalNames.WithoutCopyNumber("Game DLC (Japan) [LZ4].nsp") == "Game DLC (Japan) [LZ4].nsp");

        // A game folder built where the game it came from is, named the way this names what it makes.
        var folder = GameFolder.Open(gameFolder);
        var options = Options.Parse([]);
        string usual = folder.BuildOptions(options).OutputPath!;
        Report("a game folder's NSP goes beside it", Path.GetDirectoryName(usual) == Path.GetDirectoryName(Path.GetFullPath(gameFolder)), usual);
        bool made = !File.Exists(usual);
        if (made)
            File.WriteAllText(usual, "the game it came from");
        string beside = folder.BuildOptions(Options.Parse([])).OutputPath!;
        Report("and beside the game it came from, under that very name, it takes (2)", beside == Path.Combine(Path.GetDirectoryName(usual)!,
            Path.GetFileNameWithoutExtension(usual) + " (2).nsp"), beside);
        if (made)
        {
            Report("which is left as it was", File.ReadAllText(usual) == "the game it came from");
            File.Delete(usual);
        }
        Directory.Delete(work, true);
        Console.WriteLine($"{s_passes} passed, {s_failures} failed");
        return s_failures > 0 ? 1 : 0;
    }

    private static int CheckVersions()
    {
        (string candidate, string current, bool newer)[] cases =
        [
            ("1.5.0", "1.5.0-test.1", true), ("1.5.0-test.2", "1.5.0-test.1", true), ("1.5.0-test.1", "1.5.0-test.2", false),
            ("1.5.0-test.1", "1.5.0", false), ("1.5.0-test.1", "1.4.3", true), ("1.4.3", "1.5.0-test.1", false), ("1.5.1", "1.5.0", true),
            ("1.5.0", "1.5.0", false), ("1.5.0-test.10", "1.5.0-test.9", true), ("1.5.0-test.x", "1.4.3", false), ("1.5.1-test.1", "1.5.0", true),
        ];
        foreach (var (candidate, current, newer) in cases)
            Report($"{candidate} {(newer ? "comes after" : "doesn't come after")} {current}", Updater.IsNewer(candidate, current) == newer);
        Report("1.5.0-test.3 is a test version, and 1.5.0 isn't", Updater.IsTestVersion("1.5.0-test.3") && !Updater.IsTestVersion("1.5.0"));
        Console.WriteLine($"{s_passes} passed, {s_failures} failed");
        return s_failures > 0 ? 1 : 0;
    }

    private static string Describe(Session session) =>
        $"compression {(session.Settings.Get("--nocompress") == "1" ? "off" : "on")}, minimum {(session.Settings.Get("--minsaving") is { Length: > 0 } m ? m : "default")}, " +
        $"starting on {(session.Settings.Get("--showcompressed") == "1" ? "everything" : "what's to do")}, showing {session.Snapshot().View}";

    private static void WaitUntilIdle(Session session)
    {
        int quiet = 0;
        var started = DateTime.UtcNow;
        while (quiet < 5 && DateTime.UtcNow - started < TimeSpan.FromMinutes(10))
        {
            Thread.Sleep(20);
            quiet = session.Snapshot().Activity == SessionActivity.Idle ? quiet + 1 : 0;
        }
    }

    private static void Set(Session session, string flag, string value)
    {
        session.Settings.Set(flag, value);
        session.Settings.Save();
        session.OptionChanged(flag);
    }

    private static string Step(Session session, Random random, ref int jobs)
    {
        var rows = session.Snapshot().Rows;
        switch (random.Next(13))
        {
            case 10:
                session.BackToList();
                return "back to the list";
            case 11:
                if (jobs >= 6)
                    return "nothing run, enough has been";
                jobs++;
                // Checking reads the ticked games back, and changes nothing about them once the list is gone back to.
                if (random.Next(2) == 0)
                    return session.Check() ? "checked what's ticked for damage" : "nothing to check";
                return session.Extract() ? "extracted what's ticked" : "nothing to extract";
            case 12:
            {
                if (jobs >= 6)
                    return "nothing run, enough has been";
                jobs++;
                // An NSP folder that can't be made, as it would be inside a file, so each game it would write fails, saying why. It's put back
                // once that's done, as nothing else need know.
                string good = session.Settings.Get("--nspdir");
                session.Settings.Set("--nspdir", Path.Combine(s_work, "settings.txt", "out"));
                session.Settings.Save();
                bool started = session.Compress();
                WaitUntilIdle(session);
                session.Settings.Set("--nspdir", good);
                session.Settings.Save();
                int failed = session.Snapshot().Job?.Failed ?? 0;
                return started ? $"compressed what's ticked, where it can't be written, {failed} couldn't be done" : "nothing to compress";
            }
            case 0:
            case 1:
                Set(session, "--nocompress", session.Settings.Get("--nocompress") == "1" ? "" : "1");
                return "compression changed";
            case 2:
            {
                string[] choices = ["Off", "5%", "10%", "20%"];
                string next = choices[random.Next(choices.Length)];
                Set(session, "--minsaving", next);
                return $"minimum saving {next}";
            }
            case 3:
                if (random.Next(2) == 0)
                {
                    Set(session, "--showcompressed", session.Settings.Get("--showcompressed") == "1" ? "" : "1");
                    return "the view the list starts on changed";
                }
                var view = (GamesShown)random.Next(3);
                session.SetView(view);
                return $"showing {view}";
            case 4:
                if (rows.Length == 0)
                    return "nothing to tick";
                var row = rows[random.Next(rows.Length)];
                session.Toggle(row);
                return $"ticked or unticked {row.Name}";
            case 5:
                session.SelectAll(random.Next(2) == 0);
                return "ticked or unticked all";
            case 6:
            case 7:
                // Analyzing the ticked games again forgets what was known about them, and comes to what a first analysis does.
                if (random.Next(3) == 0)
                    return session.AnalyzeAgain() ? "analyzed the ticked games again" : "nothing ticked to analyze again";
                return session.Analyze() ? "analyzed" : "nothing to analyze";
            default:
            {
                if (jobs >= 6)
                    return "nothing run, enough has been";
                jobs++;
                if (session.TickedWithoutUpdate().Length > 0 && random.Next(2) == 0)
                    session.UntickWithoutUpdate();
                // What's ticked and still to do, and nothing else, as on the Switch: a game a first look can't vouch for waits for a thorough
                // analysis to tick it.
                var before = session.Snapshot();
                var ticked = before.ShowingJob ? null : before.Rows.Where(r => r.Selected && !r.IsDone(BatchMode.Convert)).Select(r => r.Key).ToHashSet();
                if (!session.Compress())
                    return "nothing to compress";
                if (ticked is not null && session.Snapshot().Rows.Select(r => r.Key).ToHashSet() is var job)
                    Report("compressing does what's ticked, and nothing else", job.SetEquals(ticked),
                        $"{job.Count} in the job, {ticked.Count} ticked, differing: {string.Join(", ", job.Except(ticked).Concat(ticked.Except(job)).Take(6))}");
                return "compressed what's ticked";
            }
        }
    }

    /// <summary>What would be asked about games with no update, were Compress pressed now, has to be right about them.</summary>
    private static void CheckAsked(Session session, string step)
    {
        foreach (string name in session.TickedWithoutUpdate())
        {
            foreach (var asked in session.Snapshot().Rows.Where(r => r.Name == name && r.Selected && r.Estimate is not null))
            {
                long change = Math.Abs(asked.Estimate!.OutputBytes - asked.InputBytes);
                Report($"{step}: '{name}' would be asked about as gaining nothing, and doesn't change size", change <= Math.Max(asked.InputBytes / 50, 1 << 20),
                    $"it's {asked.InputBytes} bytes and its row says {asked.Estimate.OutputBytes}");
            }
        }
    }

    /// <summary>A folder holding only a file that can't be used, in a session of its own: the list says what's wrong with it, rather than
    /// being empty as if nothing had been found, though such files found in folders are left out of To do.</summary>
    private static void CheckUnusableAlone(Session running)
    {
        string folder = Path.Combine(s_work, "unusable only");
        Directory.CreateDirectory(folder);
        // The start of a real game file, cut short, as a download that stopped part way leaves it.
        using (var whole = File.OpenRead(s_inputs[0]))
        {
            var start = new byte[Math.Min(700_000, whole.Length / 2)];
            whole.ReadExactly(start);
            File.WriteAllBytes(Path.Combine(folder, "Cut Short [0100ABCD00000000][v0].nsp"), start);
        }
        var lone = new Session();
        Ui.Sink = lone;
        lone.AddInputs([folder]);
        WaitUntilIdle(lone);
        // On To do, where such files found in folders are left out, and then back on the view that was saved.
        var before = lone.Snapshot().View;
        lone.SetView(GamesShown.ToDo);
        var shown = lone.Snapshot();
        lone.SetView(before);
        lone.Dispose();
        Ui.Sink = running;
        Ui.Token = CancellationToken.None;
        Report("a folder holding only a file that can't be used says so, rather than showing an empty list",
            shown.Rows.Length == 0 && shown.Issues.Length == 1, $"{shown.Rows.Length} rows, {shown.Issues.Length} issues, on {shown.View}");
    }

    /// <summary>
    /// Game and DLC in one file: on, a game, its update and a DLC with nothing to compress come out as one file holding both titles. Off,
    /// they come out as two. A file made with it on and opened again from its folder is one row, compressed, under Done, with its DLC, and
    /// isn't compressed again. The DLC in it had nothing to compress, so its headers don't say this tool made it. DLC without their game come
    /// out as one file. A copy made without the DLC it's given with is to do, saying so, and doing it makes the game's file again with the DLC,
    /// which opened again is done. Runs in sessions and settings of its own, before anything else.
    /// </summary>
    private static void CheckDlcInGameFile(string keys, string settings)
    {
        string? inputs = Environment.GetEnvironmentVariable("HBP_TEST_DLC_INPUTS");
        if (string.IsNullOrEmpty(inputs))
        {
            Console.WriteLine("skipped: a game's DLC in its file, as HBP_TEST_DLC_INPUTS isn't set");
            return;
        }
        string[] files = new[] { "base.nsp", "update.nsp", "dlc_small.nsp" }.Select(f => Path.Combine(inputs, f)).ToArray();
        string folder = Path.Combine(s_work, "dlc");
        string own = Path.Combine(folder, "settings.txt");
        Directory.CreateDirectory(folder);
        Environment.SetEnvironmentVariable("HACBREWPACKLZ_SETTINGS", own);
        try
        {
            string[] Made(string name, string[] given, params string[] more)
            {
                string output = Path.Combine(folder, name);
                File.WriteAllLines(own, ["welcomed=1", $"--keyset={keys}", "--noautoupdate=1", "--minsaving=Off", $"--nspdir={output}", .. more]);
                var session = new Session();
                Ui.Sink = session;
                session.AddInputs(given);
                WaitUntilIdle(session);
                session.Compress();
                WaitUntilIdle(session);
                session.Dispose();
                Ui.Token = CancellationToken.None;
                return Directory.Exists(output) ? Directory.GetFiles(output) : [];
            }
            // What the one file made holds.
            string Holds(string[] made)
            {
                if (made.Length != 1)
                    return $"{made.Length} files";
                using var set = TitleSet.Load(made, s_keyset!);
                return string.Join(", ", set.Jobs.Select(j => j.IsBaseGame ? "the game" : "a DLC").Order());
            }
            var together = Made("together", files);
            Report("Game and DLC in one file on: a game, its update and a DLC with nothing to compress come out as one file holding both titles",
                Holds(together) == "a DLC, the game", Holds(together));
            var apart = Made("apart", files, "--separatedlc=1");
            Report("and off, as two files", apart.Length == 2, string.Join(", ", apart.Select(Path.GetFileName)));

            // A DLC this tool compressed before is a file of its own already. With the setting off it's left as it is, as on the command line,
            // and with it on it goes into the game's file.
            string older = Path.Combine(folder, "older");
            Ui.Sink = new QuietSink();
            using (var set = TitleSet.Load([Path.Combine(inputs, "dlc.nsp")], s_keyset!))
                new TitleConverter(Options.Parse(["--nspdir", older]), s_keyset!).Run(set.Jobs[0], set);
            string[] withOlder = [files[0], files[1], .. Directory.GetFiles(older)];
            var olderApart = Made("older apart", withOlder, "--separatedlc=1");
            Report("off, a DLC this tool compressed before is left as it is, and only the game's file is made",
                Holds(olderApart) == "the game", $"{Holds(olderApart)}: {string.Join(", ", olderApart.Select(Path.GetFileName))}");
            var olderTogether = Made("older together", withOlder);
            Report("and on, it goes into the game's file", Holds(olderTogether) == "a DLC, the game", Holds(olderTogether));

            // Made with the setting on by TitleConverter itself, so this checks the file whatever the list did.
            string copy = Path.Combine(folder, "copy");
            Ui.Sink = new QuietSink();
            using (var set = TitleSet.Load(files, s_keyset!))
            {
                var game = set.Jobs.First(j => j.IsBaseGame);
                new TitleConverter(Options.Parse(["--nspdir", copy]), s_keyset!).Run(game, set, set.Jobs.Where(j => !j.IsBaseGame).ToList());
            }
            File.WriteAllLines(own, ["welcomed=1", $"--keyset={keys}", "--noautoupdate=1"]);
            var again = new Session();
            Ui.Sink = again;
            // Opened by its folder: a file added by itself counts as asked for and ready to send, so it would be to do.
            again.AddInputs([copy]);
            WaitUntilIdle(again);
            again.SetView(GamesShown.Done);
            var shown = again.Snapshot();
            again.SetView(GamesShown.Everything);
            bool offered = again.Compress();
            WaitUntilIdle(again);
            again.Dispose();
            Ui.Token = CancellationToken.None;
            var row = shown.Rows.FirstOrDefault();
            Report("that file opened again is one row, compressed, under Done, with its DLC, and isn't compressed again",
                shown.EverythingCount == 1 && shown.DoneCount == 1 && shown.ToDoCount == 0 && row is { IsCompressed: true } && row.Detail.Contains("1 DLC") && !offered,
                $"{shown.EverythingCount} rows, {shown.DoneCount} under Done, {shown.ToDoCount} to do, "
                + $"{(row is null ? "none shown there" : $"\"{row.Detail}\", {(row.IsCompressed ? "" : "not ")}compressed")}, {(offered ? "" : "not ")}compressed again");

            // DLC without their game go together into one file.
            var alone = Made("alone", [Path.Combine(inputs, "dlc.nsp"), files[2]]);
            Report("DLC without their game come out as one file holding both", Holds(alone) == "a DLC, a DLC", Holds(alone));

            // A copy of the game and its update made without DLC, given with a DLC and not the original: it's to do, saying its DLC isn't in its
            // file, and doing it makes the game's file again with the DLC, which opened again is done.
            string bare = Path.Combine(folder, "bare");
            Ui.Sink = new QuietSink();
            using (var set = TitleSet.Load(files[..2], s_keyset!))
                new TitleConverter(Options.Parse(["--nspdir", bare]), s_keyset!).Run(set.Jobs[0], set);
            File.Copy(files[2], Path.Combine(bare, Path.GetFileName(files[2])));
            string remade = Path.Combine(folder, "remade");
            File.WriteAllLines(own, ["welcomed=1", $"--keyset={keys}", "--noautoupdate=1", "--minsaving=Off", $"--nspdir={remade}"]);
            var given = new Session();
            Ui.Sink = given;
            // By its folder, as files added by themselves count as asked for, which would put them under To do whatever they are.
            given.AddInputs([bare]);
            WaitUntilIdle(given);
            given.SetView(GamesShown.ToDo);
            var toDo = given.Snapshot().Rows.FirstOrDefault();
            given.SetView(GamesShown.Everything);
            given.Compress();
            WaitUntilIdle(given);
            given.Dispose();
            Ui.Token = CancellationToken.None;
            var once = Directory.Exists(remade) ? Directory.GetFiles(remade) : [];
            Report("a copy given with a DLC not in its file is to do, saying so", toDo is { IsCompressed: false } && toDo.Detail.Contains("1 DLC not in its file"),
                toDo is null ? "nothing to do" : $"\"{toDo.Detail}\"");
            Report("and doing it makes one file holding the game and the DLC", Holds(once) == "a DLC, the game", Holds(once));
            File.WriteAllLines(own, ["welcomed=1", $"--keyset={keys}", "--noautoupdate=1"]);
            var reopened = new Session();
            Ui.Sink = reopened;
            reopened.AddInputs([remade]);
            WaitUntilIdle(reopened);
            reopened.SetView(GamesShown.Done);
            var done = reopened.Snapshot();
            reopened.Dispose();
            Ui.Token = CancellationToken.None;
            Report("which opened again is under Done", done.DoneCount == 1 && done.ToDoCount == 0, $"{done.DoneCount} under Done, {done.ToDoCount} to do");

            // Analyzed, the copy saves nothing, and the usual rules decide whether it's ticked. Ticked by hand, its file is still made again.
            string byHand = Path.Combine(folder, "by hand");
            File.WriteAllLines(own, ["welcomed=1", $"--keyset={keys}", "--noautoupdate=1", "--minsaving=5%", $"--nspdir={byHand}"]);
            var judged = new Session();
            Ui.Sink = judged;
            judged.AddInputs([bare]);
            WaitUntilIdle(judged);
            judged.Analyze();
            WaitUntilIdle(judged);
            var analyzed = Everything(judged).FirstOrDefault();
            judged.SetView(GamesShown.ToDo);
            bool toDoAfter = judged.Snapshot().Rows.Length == 1;
            judged.SetView(GamesShown.Everything);
            Console.WriteLine($"analyzed, the copy's row says \"{analyzed?.DetailWithEstimate}\", {(analyzed?.Selected == true ? "ticked" : "not ticked")}, under {(toDoAfter ? "To do" : "Done")}");
            Report("analyzed, the copy's row has a figure and isn't counted as compressed", analyzed is { IsCompressed: false, Estimate: not null },
                analyzed is null ? "no row" : $"\"{analyzed.DetailWithEstimate}\"");
            if (analyzed is { Selected: false })
                judged.Toggle(analyzed);
            judged.Compress();
            WaitUntilIdle(judged);
            judged.Dispose();
            Ui.Token = CancellationToken.None;
            var ticked = Directory.Exists(byHand) ? Directory.GetFiles(byHand) : [];
            Report("and ticked by hand, it makes one file holding the game and the DLC", Holds(ticked) == "a DLC, the game", Holds(ticked));

            // Turned off, Game and DLC in one file lets the copy be done, as its DLC would get a file of its own, and turned on again it's to do.
            File.WriteAllLines(own, ["welcomed=1", $"--keyset={keys}", "--noautoupdate=1", $"--nspdir={Path.Combine(folder, "flip")}"]);
            var flip = new Session();
            Ui.Sink = flip;
            flip.AddInputs([bare]);
            WaitUntilIdle(flip);
            var on = Everything(flip).FirstOrDefault();
            Set(flip, "--separatedlc", "1");
            WaitUntilIdle(flip);
            var off = Everything(flip).FirstOrDefault();
            Set(flip, "--separatedlc", "");
            WaitUntilIdle(flip);
            var back = Everything(flip).FirstOrDefault();
            flip.Dispose();
            Ui.Token = CancellationToken.None;
            string Said(TitleRow? r) => r is null ? "no row" : $"\"{r.Detail}\", {(r.IsCompressed ? "" : "not ")}compressed, {(r.Selected ? "" : "not ")}ticked";
            Report("turning Game and DLC in one file off makes the copy compressed, and on again it's to do with its DLC not in its file",
                on is { IsCompressed: false } && off is { IsCompressed: true } && back is { IsCompressed: false } && back.Detail.Contains("1 DLC not in its file"),
                $"{Said(on)}; off {Said(off)}; on again {Said(back)}");

            // On, the same copy given with its original and its DLC in a file of its own, as 1.5.0 left them, is kept, saying its DLC isn't in
            // its file. Analyzed, making it again saves nothing, so it's under Done and not ticked.
            string loose = Path.Combine(folder, "loose");
            Directory.CreateDirectory(loose);
            foreach (string each in Directory.GetFiles(bare).Concat(files[..2]))
                File.Copy(each, Path.Combine(loose, Path.GetFileName(each)));
            File.WriteAllLines(own, ["welcomed=1", $"--keyset={keys}", "--noautoupdate=1", "--minsaving=5%", $"--nspdir={Path.Combine(folder, "loose out")}"]);
            var kept = new Session();
            Ui.Sink = kept;
            kept.AddInputs([loose]);
            WaitUntilIdle(kept);
            kept.Analyze();
            WaitUntilIdle(kept);
            kept.SetView(GamesShown.Done);
            var keptShown = kept.Snapshot();
            kept.Dispose();
            Ui.Token = CancellationToken.None;
            var keptRow = keptShown.Rows.FirstOrDefault();
            Report("on, a copy given with its original and its DLC in a file of its own is kept, under Done and not ticked, saying its DLC isn't in its file",
                keptShown.DoneCount == 1 && keptRow is { Selected: false } && keptRow.Detail.Contains("1 DLC not in its file") && !keptRow.Files.Any(f => Path.GetFileName(f) == "base.nsp"),
                keptRow is null ? $"{keptShown.DoneCount} under Done" : $"{Said(keptRow)}: {string.Join(", ", keptRow.Files.Select(Path.GetFileName))}");

            // Off, the copy given with this tool's file of one DLC and the merged original, where the other DLC is: that DLC has no file of its
            // own, so the row is to do and ticked, analyzed with a minimum saving, and doing it makes that DLC a file. Opened again with that
            // file, it's done.
            string mergedIn = Path.Combine(folder, "merged apart"), mergedOut = Path.Combine(folder, "merged apart out");
            Directory.CreateDirectory(mergedIn);
            foreach (string each in Directory.GetFiles(bare).Where(f => Path.GetFileName(f) != Path.GetFileName(files[2])).Concat(Directory.GetFiles(older)).Append(Path.Combine(inputs, "merged.nsp")))
                File.Copy(each, Path.Combine(mergedIn, Path.GetFileName(each)));
            File.WriteAllLines(own, ["welcomed=1", $"--keyset={keys}", "--noautoupdate=1", "--minsaving=5%", "--separatedlc=1", $"--nspdir={mergedOut}"]);
            TitleRow? ApartRow(string[] given, out SessionSnapshot shown, out bool offered)
            {
                var session = new Session();
                Ui.Sink = session;
                session.AddInputs(given);
                WaitUntilIdle(session);
                session.Analyze();
                WaitUntilIdle(session);
                session.SetView(GamesShown.ToDo);
                shown = session.Snapshot();
                session.SetView(GamesShown.Everything);
                var row = Everything(session).FirstOrDefault();
                offered = session.Compress();
                WaitUntilIdle(session);
                session.Dispose();
                Ui.Token = CancellationToken.None;
                return row;
            }
            var apart1 = ApartRow([mergedIn], out var apartShown, out _);
            var apartMade = Directory.Exists(mergedOut) ? Directory.GetFiles(mergedOut) : [];
            Report("off, a copy whose other DLC is only inside its merged original is to do and ticked, analyzed",
                apartShown.ToDoCount == 1 && apart1 is { IsCompressed: false, Selected: true }, $"{apartShown.ToDoCount} to do, {Said(apart1)}");
            Report("and doing it makes a file of that DLC", Holds(apartMade) == "a DLC", Holds(apartMade));
            // Given the same files: the new file is found in the NSP folder, as it would be opened again.
            var apart2 = ApartRow([mergedIn], out var apartAgain, out bool apartOffered);
            int outFiles = Directory.Exists(mergedOut) ? Directory.GetFiles(mergedOut).Length : 0;
            Report("which opened again with that file is done, and nothing new is made",
                apartAgain.ToDoCount == 0 && apart2 is { IsCompressed: true } && !apartOffered && outFiles == apartMade.Length,
                $"{apartAgain.ToDoCount} to do, {Said(apart2)}, {(apartOffered ? "" : "not ")}compressed again, {outFiles} files");

            // DLC without their game that have nothing to compress go into one file as they are, and opened again with it, they're done.
            string small2 = Path.Combine(inputs, "dlc_small2.nsp");
            Report("the DLC inputs have make_patch_test.py's dlc_small2.nsp", File.Exists(small2), small2);
            if (File.Exists(small2))
            {
                string tiny = Path.Combine(folder, "tiny"), tinyOut = Path.Combine(folder, "tiny out");
                Directory.CreateDirectory(tiny);
                File.Copy(files[2], Path.Combine(tiny, Path.GetFileName(files[2])));
                File.Copy(small2, Path.Combine(tiny, Path.GetFileName(small2)));
                File.WriteAllLines(own, ["welcomed=1", $"--keyset={keys}", "--noautoupdate=1", "--minsaving=Off", $"--nspdir={tinyOut}"]);
                var tinyFirst = new Session();
                Ui.Sink = tinyFirst;
                tinyFirst.AddInputs([tiny]);
                WaitUntilIdle(tinyFirst);
                tinyFirst.Compress();
                WaitUntilIdle(tinyFirst);
                tinyFirst.Dispose();
                Ui.Token = CancellationToken.None;
                var tinyMade = Directory.Exists(tinyOut) ? Directory.GetFiles(tinyOut) : [];
                Report("DLC without their game and with nothing to compress come out as one file holding both", Holds(tinyMade) == "a DLC, a DLC", Holds(tinyMade));
                var tinyAgain = new Session();
                Ui.Sink = tinyAgain;
                tinyAgain.AddInputs([tiny]);
                WaitUntilIdle(tinyAgain);
                tinyAgain.SetView(GamesShown.Done);
                var tinyShown = tinyAgain.Snapshot();
                tinyAgain.SetView(GamesShown.Everything);
                bool tinyOffered = tinyAgain.Compress();
                WaitUntilIdle(tinyAgain);
                tinyAgain.Dispose();
                Ui.Token = CancellationToken.None;
                Report("and opened again with that file, they're under Done and not made again",
                    tinyShown.DoneCount == 1 && tinyShown.ToDoCount == 0 && !tinyOffered && Directory.GetFiles(tinyOut).Length == 1,
                    $"{tinyShown.DoneCount} under Done, {tinyShown.ToDoCount} to do, {(tinyOffered ? "" : "not ")}compressed again, {Directory.GetFiles(tinyOut).Length} files");
            }
        }
        finally
        {
            Environment.SetEnvironmentVariable("HACBREWPACKLZ_SETTINGS", settings);
        }
    }

    /// <summary>A new session with the same files and settings, analyzed as far as the running one was.</summary>
    private static SessionSnapshot Fresh(Session running, SessionSnapshot shown)
    {
        var fresh = new Session();
        Ui.Sink = fresh;
        fresh.AddInputs(s_inputs);
        WaitUntilIdle(fresh);
        // The view is chosen until the program closes, so the fresh one is shown the same.
        fresh.SetView(shown.View);
        if (shown.Rows.Any(r => r.Estimate is not null))
        {
            fresh.SelectAll(true);
            fresh.Analyze();
            WaitUntilIdle(fresh);
        }
        var snapshot = fresh.Snapshot();
        fresh.Dispose();
        Ui.Sink = running;
        // Closing it stops what it was doing, by way of the one token everything goes by.
        Ui.Token = CancellationToken.None;
        return snapshot;
    }

    private static string WithoutDigits(string text) => new(text.Select(c => char.IsDigit(c) ? '#' : c).ToArray());

    /// <summary>A job where nothing can be written, before anything's been compressed, so there's something for it to fail on: the games that
    /// couldn't be done come first, saying why, and are still to do back on the list. Everything is ticked for it, and stays so.</summary>
    private static void CheckFailingJob(Session session)
    {
        const string step = "a job where nothing can be written";
        session.SelectAll(true);
        WaitUntilIdle(session);
        string good = session.Settings.Get("--nspdir");
        session.Settings.Set("--nspdir", Path.Combine(s_work, "settings.txt", "out"));
        session.Settings.Save();
        bool started = session.Compress();
        WaitUntilIdle(session);
        session.Settings.Set("--nspdir", good);
        session.Settings.Save();
        var s = session.Snapshot();
        Report($"{step}: has games that couldn't be done", started && s.Job is { Failed: > 0 }, $"started {started}, {s.Job?.Failed ?? 0} couldn't be done of {s.Job?.Count ?? 0}");
        if (!s.ShowingJob)
            return;
        Console.WriteLine($"{step}: {s.Job!.Failed} of {s.Job.Count} couldn't be done, such as: {s.Rows.FirstOrDefault(r => r.State == RowState.Failed)?.Failure}");
        CheckJob(s, step);
        session.BackToList();
        CheckBack(session, s.Rows, step);
    }

    /// <summary>Every game's estimate, compressed and repacked, against what converting it really comes to, before anything else, as the steps
    /// only come to it now and then. Compression is put back as it was, and everything stays ticked, as the job before left it.</summary>
    private static void CheckEveryEstimate(Session session)
    {
        string compression = session.Settings.Get("--nocompress");
        var view = session.Snapshot().View;
        session.SetView(GamesShown.Everything);
        foreach (string off in new[] { "", "1" })
        {
            Set(session, "--nocompress", off);
            WaitUntilIdle(session);
            session.SelectAll(true);
            session.Analyze();
            WaitUntilIdle(session);
            CheckSizes(session, session.Snapshot(), off == "1" ? "at the start, repacked" : "at the start, compressed");
        }
        Set(session, "--nocompress", compression);
        WaitUntilIdle(session);
        session.SetView(view);
    }

    /// <summary>Every game on the list, whichever view is up, which it's left on.</summary>
    private static TitleRow[] Everything(Session session)
    {
        var view = session.Snapshot().View;
        session.SetView(GamesShown.Everything);
        var all = session.Snapshot().Rows;
        session.SetView(view);
        return all;
    }

    /// <summary>To do, Done and Everything, with compression on or off, as on the Switch. With it off, what's done is what a job here did, or
    /// one in one piece, given with no update, that a repack gains nothing from.</summary>
    private static void CheckViews(Session session, string step)
    {
        var s = session.Snapshot();
        bool compressing = session.Settings.Get("--nocompress") != "1";
        GamesShown[] expected = [GamesShown.ToDo, GamesShown.Done, GamesShown.Everything];
        var problems = new List<string>();
        if (!s.ViewsOffered.SequenceEqual(expected))
            problems.Add($"offers {string.Join(", ", s.ViewsOffered)}");
        if (!s.ViewsOffered.Contains(s.View))
            problems.Add($"shows {s.View}, which isn't offered");
        if (!compressing)
        {
            var view = s.View;
            session.SetView(GamesShown.Done);
            foreach (var row in session.Snapshot().Rows)
            {
                if (!row.IsDone(BatchMode.Convert) && (row.HasUpdate || row.IsFolder))
                    problems.Add($"'{row.Name}' is under Done with compression off, but a repack would make its update part of it");
            }
            session.SetView(view);
        }
        Report($"{step}: the views offered go with compression being {(compressing ? "on" : "off")}", problems.Count == 0, string.Join("; ", problems));
    }

    /// <summary>A job shows only its games: those that couldn't be done first, saying why, then any it left out, saying why, then what each
    /// of the rest came to, which adds up to what it says was saved.</summary>
    private static void CheckJob(SessionSnapshot s, string step)
    {
        var job = s.Job!;
        var problems = new List<string>();
        if (s.Rows.Length != job.Count)
            problems.Add($"{s.Rows.Length} games shown for a job of {job.Count}");
        if (s.Rows.Select(r => r.Key).Distinct().Count() != s.Rows.Length)
            problems.Add("a game is shown twice");
        if (s.Issues.Length > 0)
            problems.Add("files that can't be used are shown with it");
        static int Rank(TitleRow r) => r.State == RowState.Failed ? 0 : r.State == RowState.Done ? 2 : 1;
        for (int i = 1; i < s.Rows.Length; i++)
        {
            if (Rank(s.Rows[i]) < Rank(s.Rows[i - 1]))
                problems.Add($"'{s.Rows[i].Name}' ({s.Rows[i].State}) comes after '{s.Rows[i - 1].Name}' ({s.Rows[i - 1].State})");
        }
        if (job.Running || s.Rows.Any(r => r.State == RowState.Working))
            problems.Add("still running once it's over");
        if (Math.Abs(job.Overall - 1) > 0.001)
            problems.Add($"{job.Overall:P1} through it once it's over");
        long saved = s.Rows.Where(r => r.IsDone(BatchMode.Convert)).Sum(r => r.InputBytes - r.OutputBytes);
        if (saved != job.SavedBytes)
            problems.Add($"says {job.SavedBytes} saved, but its games saved {saved}");
        if (job.BeforeBytes != s.Rows.Sum(r => r.InputBytes))
            problems.Add($"says its games come to {job.BeforeBytes}, but they come to {s.Rows.Sum(r => r.InputBytes)}");
        foreach (var row in s.Rows)
        {
            if (row.State == RowState.Failed && string.IsNullOrWhiteSpace(row.Failure))
                problems.Add($"'{row.Name}' couldn't be done, and doesn't say why");
            // Not sure yet is a reason too: it's left out until Measure has read it.
            if (row.State == RowState.Ready && row.BelowMinimum is null && !row.NotSure && !row.IsCompressed)
                problems.Add($"'{row.Name}' was left out, and doesn't say why");
        }
        Report($"{step}: the job shows only its games, with what came of each", problems.Count == 0, string.Concat(problems.Take(8).Select(d => "\n      " + d)));
    }

    /// <summary>Back on the list from what a job came to, what it compressed is under Done and what it couldn't do is still to do.</summary>
    private static void CheckBack(Session session, TitleRow[] job, string step)
    {
        var s = session.Snapshot();
        var problems = new List<string>();
        if (s.ShowingJob)
            problems.Add("it's still showing the job");
        bool compressing = session.Settings.Get("--nocompress") != "1";
        var view = s.View;
        session.SetView(GamesShown.ToDo);
        var toDo = session.Snapshot().Rows.Select(r => r.Key).ToHashSet();
        session.SetView(GamesShown.Done);
        var done = session.Snapshot().Rows.Select(r => r.Key).ToHashSet();
        session.SetView(view);
        foreach (var row in job)
        {
            // Compressed, or with compression off repacked: either way, it's done.
            bool finished = row.IsDone(BatchMode.Convert);
            if (finished && (!done.Contains(row.Key) || toDo.Contains(row.Key)))
                problems.Add($"'{row.Name}' was {(compressing ? "compressed" : "repacked")}, but is {(toDo.Contains(row.Key) ? "still to do" : "not under Done")}");
            if (row.State == RowState.Failed && !toDo.Contains(row.Key))
                problems.Add($"'{row.Name}' couldn't be done, but isn't still to do");
        }
        Report($"{step}: back on the list, what the job compressed is under Done", problems.Count == 0, string.Concat(problems.Take(8).Select(d => "\n      " + d)));
    }

    /// <summary>What a job has made in this run. A fresh session finds it where NSPs go and takes it in place of the game it was made from, while
    /// this one goes on showing that game, done, and once an option changes, to do again, until its list is read again.</summary>
    private static readonly HashSet<string> s_made = [];

    /// <summary>Of those, what was compressed, which a fresh session finds and counts as compressed.</summary>
    private static readonly HashSet<string> s_compressed = [];

    private static void Compare(SessionSnapshot a, TitleRow[] all, SessionSnapshot b, string step)
    {
        var differences = new List<string>();
        var byKey = b.Rows.ToDictionary(r => r.Key);
        // A copy compressed this time is found where NSPs go, and takes the place of the game it was made from. One with nothing compressing
        // could store compressed, such as a game with no game data, isn't counted as compressed, but is still what the fresh session goes by.
        string made = Path.GetFullPath(Path.Combine(s_work, "out"));
        foreach (string key in s_compressed)
        {
            if (byKey.TryGetValue(key, out var found) && !found.Files.Any(f => Path.GetFullPath(f).StartsWith(made + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase)))
                differences.Add($"'{found.Name}' was compressed this time, but a fresh session doesn't find its copy ({found.DetailWithEstimate}; {string.Join(", ", found.Files.Select(Path.GetFileName))})");
        }
        var inA = a.Rows.Select(r => r.Key).ToHashSet();
        // One done this time is under Done, which a fresh session knows about only from the copy it finds, whichever of the two shows it.
        var doneHere = all.Where(r => r.State != RowState.Ready).Select(r => r.Key).Concat(s_made).ToHashSet();
        foreach (var row in a.Rows.Where(r => !byKey.ContainsKey(r.Key) && !doneHere.Contains(r.Key)))
            differences.Add($"'{row.Name}' is listed, but not in a fresh session ({row.DetailWithEstimate})");
        foreach (var row in b.Rows.Where(r => !inA.Contains(r.Key) && !doneHere.Contains(r.Key)))
            differences.Add($"'{row.Name}' isn't listed, but is in a fresh session ({row.DetailWithEstimate})");
        // What's been done this time counts differently, which a fresh session doesn't know about, whichever view it's in.
        bool anyDone = all.Any(r => r.State != RowState.Ready) || s_made.Count > 0;
        if (!anyDone && (a.View != b.View || a.ToDoCount != b.ToDoCount || a.DoneCount != b.DoneCount || a.EverythingCount != b.EverythingCount))
            differences.Add($"{a.View}: {a.ToDoCount} to do, {a.DoneCount} done, {a.EverythingCount} in all, but {b.View}: {b.ToDoCount}, {b.DoneCount}, {b.EverythingCount} in a fresh session");
        if (a.ToDoCount + a.DoneCount != a.EverythingCount)
            differences.Add($"{a.ToDoCount} to do and {a.DoneCount} done don't add up to {a.EverythingCount}");

        foreach (var row in a.Rows)
        {
            if (!byKey.TryGetValue(row.Key, out var f) || row.State != RowState.Ready || s_made.Contains(row.Key))
                continue;
            string name = $"'{row.Name}'";
            if (row.Detail != f.Detail)
                differences.Add($"{name} says \"{row.Detail}\", but \"{f.Detail}\" in a fresh session");
            if (row.IsCompressed != f.IsCompressed)
                differences.Add($"{name} is {(row.IsCompressed ? "" : "not ")}counted as compressed, but is{(f.IsCompressed ? "" : " not")} in a fresh session");
            bool both = row.Estimate is not null && f.Estimate is not null;
            if (both)
            {
                long allowed = Math.Max(3 * Math.Max(row.Estimate!.MarginBytes, f.Estimate!.MarginBytes) + row.InputBytes / 100, 1 << 20);
                if (Math.Abs(row.Estimate.OutputBytes - f.Estimate.OutputBytes) > allowed)
                    differences.Add($"{name} comes to {row.Estimate.OutputBytes}, but {f.Estimate.OutputBytes} in a fresh session");
                if (WithoutDigits(row.DetailWithEstimate) != WithoutDigits(f.DetailWithEstimate))
                    differences.Add($"{name} says \"{row.DetailWithEstimate}\", but \"{f.DetailWithEstimate}\" in a fresh session");
            }
            if (!row.ChosenByHand && (both || (row.Estimate is null && f.Estimate is null)) && row.Selected != f.Selected)
                differences.Add($"{name} is {(row.Selected ? "ticked" : "unticked")}, but {(f.Selected ? "ticked" : "unticked")} in a fresh session");
        }
        Report($"{step}: a fresh session shows the same", differences.Count == 0, string.Concat(differences.Take(8).Select(d => "\n      " + d)));
    }

    /// <summary>What converting a row's files really comes to, with the settings it's shown with.</summary>
    private static void CheckSizes(Session session, SessionSnapshot running, string step)
    {
        var options = Options.Parse([.. session.Settings.ToArguments(OptionUse.Convert), "--nspdir", Path.Combine(s_work, "size")]);
        foreach (var row in running.Rows)
        {
            if (row.State != RowState.Ready || row.Estimate is null || row.IsFolder || (options.Compress && row.IsCompressed))
                continue;
            string key = string.Join("|", row.Files.Select(f => $"{f}:{new FileInfo(f).Length}")) + (options.Compress ? "|compressed" : "|repacked");
            if (!s_realSizes.TryGetValue(key, out long real))
            {
                string made = Path.Combine(s_work, "size");
                try
                {
                    using var set = TitleSet.Load(row.Files, s_keyset!, dlcInGameFile: !options.SeparateDlc);
                    var converter = new TitleConverter(options, s_keyset!);
                    // The same as the list makes it: once the game's file is made, its DLC goes into it, or each into a file of its own, and DLC
                    // without their game go into one file.
                    bool gameMade = false;
                    var jobs = set.Jobs.OrderBy(j => j.IsBaseGame ? 0 : 1).ToList();
                    for (int at = 0; at < jobs.Count; at++)
                    {
                        var job = jobs[at];
                        if (options.Compress && (job.IsOwnOutput || (!gameMade && Analyzer.IsAlreadyCompressed(job, options))))
                        {
                            real += job.InputBytes;
                            continue;
                        }
                        var dlc = !options.SeparateDlc && (job.IsBaseGame || !jobs[0].IsBaseGame) ? jobs.Skip(at + 1).Where(j => !j.IsBaseGame).ToList() : [];
                        real += converter.Run(job, set, dlc).bytes;
                        gameMade |= job.IsBaseGame;
                        if (dlc.Count > 0)
                            break;
                    }
                    s_foundSizes[key] = converter.FoundBytes;
                }
                catch (ToolException e)
                {
                    Report($"{step}: '{row.Name}' can be converted, to see what it comes to", false, e.Message);
                    real = -1;
                }
                if (Directory.Exists(made))
                    Directory.Delete(made, true);
                s_realSizes[key] = real;
            }
            if (real < 0)
                continue;
            // An estimate that read all of the game data has no guessing in it, only the padding between blocks taken as an average, which is
            // what tests/matrix.py holds small titles to. Regions that all came out the same say so too, which is only wrong if the rest differs.
            long allowed = row.Estimate.MarginBytes == 0 ? Math.Max(real / 200, 256 << 10) : Math.Max(3 * row.Estimate.MarginBytes + row.InputBytes / 50, 1 << 20);
            // What compressing found held again by itself isn't stored, which the estimate can't know: it can come to up to that much less.
            long found = s_foundSizes.GetValueOrDefault(key);
            long under = row.Estimate.OutputBytes - real;
            Report($"{step}: '{row.Name}' comes to what its row says ({(options.Compress ? "compressed" : "repacked")})", under <= allowed + found && -under <= allowed,
                $"the row says {row.Estimate.OutputBytes} +/- {row.Estimate.MarginBytes}, it comes to {real}{(found > 0 ? $", {found} found held again while compressing" : "")}");
        }
    }
}
