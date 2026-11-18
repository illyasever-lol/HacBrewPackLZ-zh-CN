using System.Text;

namespace HacBrewPackLz;

/// <summary>
/// Details and Look inside on the command line, from the same reader as the screens: what a game's Details say, as text, and what's inside it,
/// a folder at a time, with any of it saved to Files from games.
/// </summary>
internal static class LookCommand
{
    public static int Run(string command, string[] args)
    {
        // Names inside games, and what's said about them, are in UTF-8 wherever they go, a pipe as much as a window.
        Console.OutputEncoding = new System.Text.UTF8Encoding(false);
        string? keysetPath = null, to = null, outDir = null;
        bool save = false, all = false, measure = false;
        var given = new List<string>();
        for (int i = 0; i < args.Length; i++)
        {
            switch (args[i])
            {
                case "-k" or "--keyset" when i + 1 < args.Length: keysetPath = args[++i]; break;
                case "--to" when i + 1 < args.Length: to = args[++i]; break;
                case "--outdir" when i + 1 < args.Length: outDir = args[++i]; break;
                case "--save": save = true; break;
                case "--all": all = true; break;
                case "--measure": measure = true; break;
                default:
                    if (args[i].StartsWith('-'))
                        throw new ToolException($"Unknown option {args[i]} (use --help)");
                    given.Add(args[i]);
                    break;
            }
        }
        var keyset = Keyset.Load(Keyset.Find(keysetPath));
        if (command == "details")
            return Details(given, keyset, measure);

        // The game's file first, then what's inside it: what's after the last file that's there is a place inside the game.
        int files = given.TakeWhile(File.Exists).Count();
        if (files == 0)
            throw new ToolException(given.Count == 0 ? "Give it a game file to look inside" : $"'{given[0]}' not found");
        string inner = string.Join('/', given.Skip(files));
        using var inside = GameInside.Open(Path.GetFileNameWithoutExtension(given[0]), given.Take(files).ToList(), keyset);
        int at = inner.Length == 0 ? 0 : inside.Find(inner);
        if (at < 0)
            throw new ToolException($"Nothing in it is at '{inner}'. Look in the folder above it to see what is");
        var node = inside.Nodes[at];

        if (save)
        {
            string folder = to ?? GameInside.FilesFromGamesFor(given[0], outDir);
            var progress = new ProgressReporter("Saving", node.Size);
            string saved = inside.Save(at, folder, (done, _, _, _) => progress.Report(done, done));
            progress.Done();
            Ui.Success($"Saved to {saved}");
            return 0;
        }
        if (all)
        {
            // Every file under it, a line each, for going through with other tools.
            var stack = new Stack<int>();
            stack.Push(at);
            var lines = new List<string>();
            while (stack.Count > 0)
            {
                int index = stack.Pop();
                var each = inside.Nodes[index];
                if (each.Folder)
                {
                    foreach (int child in each.Children)
                        stack.Push(child);
                    continue;
                }
                lines.Add($"{inside.PathOf(index)}\t{each.Size}\t{each.Kind}");
            }
            lines.Sort(StringComparer.Ordinal);
            foreach (string line in lines)
                Console.WriteLine(line);
            return 0;
        }

        Ui.Heading(at == 0 ? $"{inside.Name}: {GameInside.WithCommas(inside.Files)} files, {ProgressReporter.Format(inside.Bytes)}" : inside.PathOf(at));
        if (node.Folder)
        {
            int nameWidth = Math.Min(48, node.Children.Select(c => inside.Nodes[c].Name.Length + (inside.Nodes[c].Folder ? 1 : 0)).DefaultIfEmpty(0).Max());
            foreach (int child in node.Children)
            {
                var each = inside.Nodes[child];
                string name = each.Folder ? each.Name + "/" : each.Name;
                string what = each.Folder ? GameInside.ItemsWord(each.Items) : each.Kind.ToLowerInvariant();
                Ui.Info($"  {name.PadRight(nameWidth)}  {ProgressReporter.Format(each.Size),10}  {what}");
            }
            return 0;
        }
        var preview = inside.Show(at, asBytes: false);
        Ui.Detail(string.Join("  ·  ", preview.Chips));
        if (preview.Kind == "Text")
        {
            foreach (string line in preview.Lines)
                Console.WriteLine(line);
        }
        else if (preview.Picture is not null)
        {
            Ui.Info($"A picture, {preview.PictureWidth} × {preview.PictureHeight}. --save saves it, to see it with any picture viewer.");
        }
        else if (preview.Raw is { } raw)
        {
            foreach (string line in GameInside.ByteLines(raw, 16, Math.Min(raw.Length, 512)))
                Console.WriteLine(line);
        }
        return 0;
    }

    /// <summary>What a game's Details say, as text: the figure that decides, then the four sections and what's good to know. Worked out with a
    /// first look, or measured in full with --measure, as analyze does.</summary>
    private static int Details(List<string> files, Keyset keyset, bool measure)
    {
        if (files.Count == 0)
            throw new ToolException("Give it a game's files: the game, and its update and DLC if there are any");
        var missing = files.FirstOrDefault(f => !File.Exists(f));
        if (missing is not null)
            throw new ToolException($"'{missing}' not found");
        var options = Options.Parse(measure ? ["--measure"] : []);
        GameInside? inside = null;
        string? problem = null;
        try
        {
            inside = GameInside.Open(Path.GetFileNameWithoutExtension(files[0]), files, keyset);
        }
        catch (Exception e) when (e is ToolException or IOException or System.Security.Cryptography.CryptographicException)
        {
            problem = Failures.Describe(e);
        }
        using var opened = inside;

        // A row as the screens' lists have it: its figures from a first look, or from reading all of it.
        using var set = TitleSet.Load(files, keyset, dlcInGameFile: false); // v21：上游原生整合停用
        var jobs = set.Jobs.ToList();
        var estimate = jobs.Count == 0 ? null
            : SizeEstimate.Together(jobs.Select(job => measure ? Analyzer.Measure(job, options.MeasuringGameOf(job.InputBytes)) : Analyzer.Estimate(job, options)).ToList()).AsInstalled();
        var main = jobs.FirstOrDefault(j => j.IsBaseGame) ?? jobs.FirstOrDefault();
        string name = main?.DisplayName ?? Path.GetFileNameWithoutExtension(files[0]);
        bool notSure = estimate is not null && SavingRule.NotSure(estimate, options);
        string? below = estimate is null || notSure || SavingRule.WorthCompressing(estimate, options) ? null : SavingRule.Reason(estimate, options);
        long fileBytes = files.Sum(f => new FileInfo(f).Length);
        var row = new TitleRow(name, name, main?.Detail ?? "", estimate?.InputBytes ?? fileBytes, files.ToArray())
        {
            Estimate = estimate, NotSure = notSure, BelowMinimum = below, HasUpdate = main?.Update is not null,
            PackedBytes = estimate is { PackedBytes: > 0 } ? fileBytes : 0, AlreadyCompressed = main?.IsOwnOutput == true,
        };

        var figure = GameDetails.Figure(row, options.Compress);
        Ui.Heading(inside?.Publisher is { Length: > 0 } publisher ? $"{name}, by {publisher}" : name);
        string share = figure.Share is { } comes && comes < 1 ? $", saving {Math.Round((1 - comes) * 100):0}%" : "";
        Ui.Info($"{figure.Label} {figure.Big}{(figure.Sub.Length > 0 ? $", {figure.Sub}" : "")}{share}");
        foreach (var section in GameDetails.Sections(row, inside, problem, options.Compress))
        {
            Ui.Info("");
            Ui.Heading(section.Title);
            int width = section.Lines.Max(l => l.Label.Length);
            foreach (var line in section.Lines)
            {
                string text = $"  {line.Label.PadRight(width)}  {line.Value}{(line.Note.Length > 0 ? $"  ({line.Note})" : "")}";
                if (line.Bad)
                    Ui.Warn(text);
                else if (line.Good)
                    Ui.Success(text);
                else
                    Ui.Info(text);
            }
        }
        var notes = GameDetails.Notes(row).Concat(main?.Notes ?? []).Distinct().ToList();
        if (notes.Count > 0)
        {
            Ui.Info("");
            Ui.Heading("Good to know");
            foreach (string note in notes)
                Ui.Info($"  {note}");
        }
        return problem is null ? 0 : 1;
    }
}
