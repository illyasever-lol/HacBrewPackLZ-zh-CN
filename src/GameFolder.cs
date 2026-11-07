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

    /// <summary>The control folder is "control", as extracting writes it and as the Switch app and other tools expect.
    /// Builds before v7 wrote "控制"; accept those too so a folder someone already extracted can still be packed.</summary>
    private static string ControlName(string folder) =>
        Directory.Exists(System.IO.Path.Combine(folder, "control")) ? "control" :
        Directory.Exists(System.IO.Path.Combine(folder, "控制")) ? "控制" : "control";

    public static bool IsOne(string folder) =>
        File.Exists(System.IO.Path.Combine(folder, "exefs", "main.npdm")) &&
        File.Exists(System.IO.Path.Combine(folder, ControlName(folder), "control.nacp"));

    public static GameFolder Open(string folder)
    {
        folder = System.IO.Path.GetFullPath(folder);
        Npdm.Load(System.IO.Path.Combine(folder, "exefs", "main.npdm"), 0, null, out ulong titleId);
        string control = System.IO.Path.Combine(folder, ControlName(folder));
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

        var everywhere = new EnumerationOptions { RecurseSubdirectories = true, IgnoreInaccessible = true, AttributesToSkip = 0 };
        long bytes = Parts(folder).Sum(part => new DirectoryInfo(part).EnumerateFiles("*", everywhere).Sum(f => f.Length));
        return new GameFolder { Path = folder, Name = name, TitleId = titleId, Version = version, Bytes = bytes, Icon = IconIn(control) };
    }

    /// <summary>The folders that go into the NSP.</summary>
    private static IEnumerable<string> Parts(string folder) =>
        new[] { "exefs", "romfs", "logo", ControlName(folder), "htmldoc", "legalinfo" }
            .Select(p => System.IO.Path.Combine(folder, p)).Where(Directory.Exists);

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
        chosen.ControlDir = Part(ControlName(Path));
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
        chosen.OutputPath = System.IO.Path.Combine(directory, $"{LocalNames.ForOutputName(Name)} [{TitleId:X16}][v{Version}]{(chosen.Compress ? "[LZ4]" : "")}.nsp");
        return chosen;
    }

    /// <summary>Game folders in a folder and the folders in it, though not inside a game folder, which can hold anything.</summary>
    public static List<string> FindIn(string root)
    {
        var found = new List<string>();
        var pending = new Stack<string>();
        pending.Push(root);
        while (pending.Count > 0)
        {
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
                    // A link back up would go round forever.
                    if (!child.Attributes.HasFlag(FileAttributes.ReparsePoint))
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
