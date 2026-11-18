namespace HacBrewPackLz;

/// <summary>A game laid out in folders, as extracting leaves it: exefs with main.npdm and control with control.nacp, and romfs, logo, htmldoc and
/// legalinfo when it has them. It's built into an NSP the same way the Switch app builds one.</summary>
internal sealed class GameFolder
{
    public required string Path { get; init; }
    public required string Name { get; init; }
    public required ulong TitleId { get; init; }
    public required uint Version { get; init; }
    public required long Bytes { get; init; }
    public byte[]? Icon { get; init; }

    public static bool IsOne(string folder) =>
        File.Exists(System.IO.Path.Combine(folder, "exefs", "main.npdm")) && File.Exists(System.IO.Path.Combine(folder, "control", "control.nacp")) &&
        !File.Exists(System.IO.Path.Combine(folder, NotFinished));

    /// <summary>
    /// What extracting puts in a game's folder first and takes away once all of it is out, the same on every version. While it's there, the
    /// folder's only part of a game, even with the exefs and control folders out already, which make it look like one: it's never listed, and
    /// extracting the game again clears it away first. A power cut or a crash part of the way through leaves it there.
    /// </summary>
    public const string NotFinished = "Not finished extracting.txt";

    public const string NotFinishedText = "hacBrewPackLZ was extracting a game into this folder and didn't finish. It isn't listed as a game. Extracting the game again starts afresh, or delete this folder.";

    public static GameFolder Open(string folder)
    {
        folder = System.IO.Path.GetFullPath(folder);
        Npdm.Load(System.IO.Path.Combine(folder, "exefs", "main.npdm"), 0, null, out ulong titleId);
        string control = System.IO.Path.Combine(folder, "control");
        var nacp = File.ReadAllBytes(System.IO.Path.Combine(control, "control.nacp"));
        if (nacp.Length < Nacp.Size)
            throw new ToolException($"'{System.IO.Path.Combine(control, "control.nacp")}' is too small to be a NACP");
        string name = Nacp.Titles(nacp).Select(t => t.Name).FirstOrDefault(n => n.Length > 0) ?? System.IO.Path.GetFileName(folder);

        // Extracting leaves the version in the folder's name, and a game had better not come back older than it was.
        uint version = 0;
        string last = System.IO.Path.GetFileName(folder);
        int tag = last.LastIndexOf("[v", StringComparison.Ordinal);
        if (tag >= 0)
        {
            int end = last.IndexOf(']', tag);
            if (end > tag && uint.TryParse(last.AsSpan(tag + 2, end - tag - 2), out uint found))
                version = found;
        }

        // Counted as building it goes through it, links to folders and all, a link back to a folder it's in said as building it says it.
        long bytes = Parts(folder).Sum(RomFsBuilder.FolderBytes);
        return new GameFolder { Path = folder, Name = name, TitleId = titleId, Version = version, Bytes = bytes, Icon = IconIn(control) };
    }

    /// <summary>The folders that go into the NSP.</summary>
    private static IEnumerable<string> Parts(string folder) =>
        new[] { "exefs", "romfs", "logo", "control", "htmldoc", "legalinfo" }.Select(p => System.IO.Path.Combine(folder, p)).Where(Directory.Exists);

    /// <summary>The icon in a control folder, or what the control NCA's RomFS holds: the English one if there is one, which most games have.</summary>
    public static byte[]? IconIn(string control)
    {
        try
        {
            string english = System.IO.Path.Combine(control, "icon_AmericanEnglish.dat");
            string? path = File.Exists(english) ? english : Directory.EnumerateFiles(control, "icon_*.dat").FirstOrDefault();
            return path is null ? null : File.ReadAllBytes(path);
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
            return null;
        }
    }

    /// <summary>The options to build it with: what's chosen for compression and where NSPs go, and everything about the folders from the folder.</summary>
    public Options BuildOptions(Options chosen)
    {
        string Part(string name) => System.IO.Path.Combine(Path, name);
        string? Optional(string name) => Directory.Exists(Part(name)) ? Part(name) : null;
        chosen.ExefsDir = Part("exefs");
        chosen.RomfsDir = Part("romfs");
        chosen.LogoDir = Part("logo");
        chosen.ControlDir = Part("control");
        chosen.HtmlDocDir = Optional("htmldoc");
        chosen.LegalInfoDir = Optional("legalinfo");
        chosen.NoRomfs = !Directory.Exists(chosen.RomfsDir);
        chosen.NoLogo = !Directory.Exists(chosen.LogoDir);
        chosen.TitleId = 0;
        chosen.TitleName = null;
        chosen.TitlePublisher = null;
        chosen.NoPatchNacpLogo = false;
        chosen.TitleVersion = Version;
        string directory = chosen.NspDirSet ? chosen.NspDir : System.IO.Path.GetDirectoryName(Path)!;
        // Beside the folder, where the game it was extracted from may well be, under this very name.
        chosen.OutputPath = LocalNames.FreePath(System.IO.Path.Combine(directory, $"{LocalNames.ForOutputName(Name)} [{TitleId:X16}][v{Version}]{(chosen.Compress ? "[LZ4]" : "")}.nsp"));
        return chosen;
    }

    /// <summary>Game folders in a folder and the folders in it, though not inside a game folder, which can hold anything.</summary>
    public static List<string> FindIn(string root, CancellationToken token = default)
    {
        var found = new List<string>();
        var pending = new Stack<string>();
        pending.Push(root);
        while (pending.Count > 0)
        {
            // A whole drive can take a while, and being asked to stop shouldn't have to wait for the end of it.
            token.ThrowIfCancellationRequested();
            string folder = pending.Pop();
            if (IsOne(folder))
            {
                found.Add(folder);
                continue;
            }
            try
            {
                foreach (var child in new DirectoryInfo(folder).EnumerateDirectories())
                {
                    // A link back up would go round forever. What's been saved from inside games is a few of their parts, never a game, and
                    // could pass for one or end up mixed into an extraction of one.
                    if (!child.Attributes.HasFlag(FileAttributes.ReparsePoint) && !string.Equals(child.Name, GameInside.FilesFromGames, StringComparison.OrdinalIgnoreCase))
                        pending.Push(child.FullName);
                }
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException)
            {
            }
        }
        found.Sort(StringComparer.OrdinalIgnoreCase);
        return found;
    }
}
