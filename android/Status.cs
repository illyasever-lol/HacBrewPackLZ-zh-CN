namespace HacBrewPackLz;

internal enum Tone
{
    Normal,
    Quiet,
    Good,
    Warn,
    Bad,
}

/// <summary>What the line along the bottom says, in the same words as the Windows GUI, and how far along the job is.</summary>
internal static class Status
{
    public static (string first, string second, double? fraction) Of(SessionSnapshot s, Settings settings)
    {
        var (first, _, second) = Describe(s, settings);
        return (first, second, Fraction(s));
    }

    public static double? Fraction(SessionSnapshot s)
    {
        // A job's own view goes by all of it together, as the game being done shows how far it's got in its row.
        if (s.Job is { Running: true } job)
            return job.Overall;
        if (s.Activity == SessionActivity.Sending)
        {
            if (s.Send is { Reading: { } file } send && send.Files.TryGetValue(file, out var sent) && sent.length > 0)
                return (double)sent.read / sent.length;
            return null;
        }
        if (s.Activity is SessionActivity.Compressing or SessionActivity.Extracting or SessionActivity.Verifying or SessionActivity.Analyzing or SessionActivity.Measuring
            && s.Progress is { Total: > 0 } progress)
            return (double)progress.Done / progress.Total;
        return null;
    }

    public static string Percent(long before, long after)
    {
        if (before <= 0)
            return "";
        double saved = (before - after) * 100.0 / before;
        return saved >= 0 ? $"{saved:0}%" : $"+{-saved:0}%";
    }

    public static (string first, Tone tone, string second) Describe(SessionSnapshot s, Settings settings)
    {
        bool repack = settings.Get("--nocompress") == "1";
        if (s.Activity == SessionActivity.Reading)
            return (s.ReadingText + "…", Tone.Quiet, "");

        if (s.Activity == SessionActivity.Sending)
            return Sending(s);

        // A job's own view: the game it's on and all of it together while it runs, which the bar shows. Once it's over, what it came to, and
        // where the games are once the list is gone back to, in the same words as the Windows GUI.
        if (s.Job is { } job)
        {
            if (job.Running)
            {
                string doing = s.CurrentCount > 1 ? $"{job.Doing} {s.CurrentIndex} of {s.CurrentCount}: {s.CurrentTitle}" : $"{job.Doing} {s.CurrentTitle}";
                return (doing, Tone.Normal, $"All together {job.Overall * 100:0}%  \u00B7  {job.Progress}");
            }
            if (job.Kind == SessionActivity.Verifying)
                return (s.Summary ?? job.Headline("\u2192").what, job.Outcome == UiLevel.Success ? Tone.Good : Tone.Bad,
                    job.Failed > 0 ? "What was found is beside each game. Show all of it says more." : "Back on the list, every game is as it was.");
            var made = s.Rows.Where(r => r.State == RowState.Done).ToList();
            if (made.Count == 0)
                return (s.Summary ?? job.Headline("\u2192").what, job.Outcome == UiLevel.Error ? Tone.Bad : Tone.Warn,
                    "Back on the list, what wasn't done is still to do." + (job.Failed > 0 ? " Show all of it says what happened." : ""));
            if (job.Kind == SessionActivity.Extracting)
                return (s.Summary ?? job.Headline("\u2192").what, Tone.Good, string.Join(" ", job.Said));
            string sizes = $"{ProgressReporter.Format(made.Sum(r => r.InputBytes))} \u2192 {ProgressReporter.Format(made.Sum(r => r.OutputBytes))}";
            if (job.Repacking)
                return (s.Summary ?? job.Headline("\u2192").what, Tone.Good, sizes + ".");
            return (job.SavedBytes > 0 ? $"{ProgressReporter.Format(job.SavedBytes)} saved" : job.Headline("\u2192").what, job.Outcome == UiLevel.Success ? Tone.Good : Tone.Warn,
                $"{sizes}. Back on the list, they're under Done.");
        }

        if (s.IsBusy)
        {
            string name = s.Activity switch
            {
                SessionActivity.Analyzing => "Analyzing",
                SessionActivity.Measuring => "Measuring",
                SessionActivity.Compressing => repack ? "Repacking" : "Compressing",
                SessionActivity.Extracting => "Extracting",
                _ => "Checking",
            };
            string first = s.CurrentCount > 0 ? $"{name} {s.CurrentIndex} of {s.CurrentCount}: {s.CurrentTitle}" : name + "…";
            string second = "";
            if (s.Progress is { } progress)
            {
                var parts = new List<string> { progress.Label };
                if (progress.ShowSizes)
                    parts.Add($"{ProgressReporter.Format(progress.Done)} of {ProgressReporter.Format(progress.Total)}");
                if (progress.ShowSizes && progress.BytesPerSecond > 0)
                    parts.Add($"{progress.BytesPerSecond / 1048576:0} MB/s");
                if (Terminal.TimeLeft(progress) is { Length: > 0 } left)
                    parts.Add(left);
                second = string.Join("  ·  ", parts);
            }
            return (first, Tone.Normal, second);
        }

        // A newer version has a banner of its own, and how checking went is said in Options, so this is only about the games.
        // Where they're saved is a setting, said in Options, until a job has saved something.
        string output = settings.Get("--nspdir") is { Length: > 0 } folder ? $"Saves to {Storage.Friendly(folder)}" : "Saves beside the source files";
        if (s.Summary is not null)
        {
            var tone = s.SummaryLevel switch { UiLevel.Success => Tone.Good, UiLevel.Warning => Tone.Warn, _ => Tone.Bad };
            return (s.Summary, tone, s.LastOutput is { } last ? $"The new files are in {Storage.Friendly(Path.GetDirectoryName(last) ?? last)}" : output);
        }

        // Everything ticked is already compressed, so what's left is putting it on the Switch, which the big button then offers.
        if (!s.Rows.Any(r => r.LeftToCompress(!repack)) && s.Rows.Any(r => r.Selected && (r.IsDone(BatchMode.Convert) || r.IsCompressed)))
            return ("Ready for the Switch", Tone.Normal, "Send to Switch shares the games over Wi-Fi, for the Switch to install.");
        // Games a first look isn't sure of aren't ticked, and may save more than it seems: a thorough analysis reads them in full, and ticks the
        // ones worth it. Compressing only does what's ticked.
        int notSure = s.Rows.Count(r => r.NotSure && !r.Selected && !r.IsDone(BatchMode.Convert) && !r.IsFolder);
        string maybe = notSure == 0 ? "" : $"{(notSure == 1 ? "1 game" : $"{notSure} games")} not sure yet may save more: Thorough analysis reads {(notSure == 1 ? "it" : "them")} in full. ";
        // On Done with nothing ticked, what compressing them came to, as in the other versions.
        int compressed = s.View == GamesShown.Done ? s.Rows.Count(r => r.IsCompressed || r.IsDone(BatchMode.Convert)) : 0;
        if (compressed > 0 && !s.Rows.Any(r => r.Selected))
        {
            long freedAll = s.Rows.Sum(r => r.State == RowState.Done && r.OutputBytes < r.InputBytes ? r.InputBytes - r.OutputBytes : r.Freed);
            return ($"{(compressed == 1 ? "1 game" : $"{compressed} games")} compressed{(freedAll > 0 ? $"  ·  {ProgressReporter.Format(freedAll)} freed in all" : "")}",
                Tone.Normal, "Tick games to extract or check them.");
        }
        if (s.Rows.Length > 0 && !s.Rows.Any(r => r.Selected))
            return ("Nothing is ticked", Tone.Normal, $"{maybe}Tick the games you want to work on.");

        var known = s.Rows.Where(r => r.Selected && r.Estimate is not null).ToList();
        if (known.Count > 0 && known.All(r => r.NotSure))
        {
            // Ticked by hand: nothing to add up until they're measured, and they're compressed whatever they save.
            return (known.Count == 1 ? "Not sure yet what it saves" : "Not sure yet what they save", Tone.Normal,
                $"Ticked, {(known.Count == 1 ? "it's" : "they're")} compressed whatever {(known.Count == 1 ? "it saves" : "they save")}.");
        }
        if (known.Count > 0)
        {
            // What they're sure to save, as the list has it, which is all an estimate can promise: a game can save more. One that isn't sure
            // yet counts as staying as it is.
            long before = known.Sum(r => r.InputBytes);
            long after = known.Sum(r => r.SureAfterBytes!.Value);
            bool atLeast = known.Any(r => r.Estimate!.AtLeast);
            string scope = known.Count == s.Rows.Length ? "" : $" for {known.Count} of {s.Rows.Length} games";
            string change = after <= before ? $"{(atLeast ? "at least " : "")}{Percent(before, after)} smaller" : $"{Percent(before, after).TrimStart('+')} larger";
            string estimate = atLeast ? "An estimate, and a game can save more. " : "";
            return ($"{ProgressReporter.Format(before)} to {ProgressReporter.Format(after)}, {change}{scope}", Tone.Normal, $"{estimate}{maybe}Nothing has been written yet.");
        }

        return s.Rows.Length > 0
            ? (s.Rows.Count(r => r.Selected) is var ticked && ticked < s.Rows.Length
                ? $"{ticked} of {s.Rows.Length} games ticked, {ProgressReporter.Format(s.Rows.Where(r => r.Selected).Sum(r => r.InputBytes))}"
                : $"{s.Rows.Length} game{(s.Rows.Length == 1 ? "" : "s")}, {ProgressReporter.Format(s.Rows.Sum(r => r.InputBytes))}", Tone.Normal,
                "Analyze shows how small they'd get, without writing anything.")
            : ("", Tone.Normal, output + ".");
    }

    /// <summary>Whether the Switch is there yet, how to get it there, and what it's reading. Phones share over Wi-Fi only.</summary>
    private static (string, Tone, string) Sending(SessionSnapshot s)
    {
        if (s.Send is not { } send)
            return ("Getting ready to send to the Switch…", Tone.Normal, "");
        int compressed = s.Rows.Count(r => r.IsCompressed);
        string sharing = $"Sharing {s.Rows.Length} {(s.Rows.Length == 1 ? "game" : "games")}{(compressed > 0 ? $", {compressed} already compressed" : "")}";
        if (send.Connected.Length == 0)
        {
            return ($"{sharing}. Waiting for the Switch", Tone.Normal,
                $"On the Switch, open hacBrewPackLZ: these games come up on Game files, as being on {send.Machine}. The Switch has to be on the same Wi-Fi as this phone.");
        }

        string connected = $"The Switch is connected {string.Join(" and ", send.Connected)}";
        if (send.Reading is not { } file || !send.Files.TryGetValue(file, out var sent))
        {
            long done = send.Files.Values.Sum(f => f.read);
            return ($"{sharing}. {connected}", Tone.Normal, done > 0
                ? $"It has read {ProgressReporter.Format(done)} so far. Choose a game on the Switch to install it."
                : "Choose a game on the Switch to install it. Leave this open until it's done.");
        }

        var parts = new List<string> { $"{ProgressReporter.Format(sent.read)} of {ProgressReporter.Format(sent.length)}" };
        if (send.BytesPerSecond > 0)
        {
            parts.Add($"{send.BytesPerSecond / 1048576.0:0.0} MB/s");
            long left = (sent.length - sent.read) / Math.Max(send.BytesPerSecond, 1);
            if (left > 0)
                parts.Add(left >= 60 ? $"about {(left + 30) / 60} min left" : $"about {left} s left");
        }
        parts.Add(connected.Replace("The Switch is connected", "connected"));
        return ($"Sending {Path.GetFileName(file)}", Tone.Normal, string.Join("  ·  ", parts));
    }
}
