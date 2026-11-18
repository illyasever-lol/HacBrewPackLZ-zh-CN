using System.Text;

namespace HacBrewPackLz;

/// <summary>Names inside an NSP are untrusted: they must never be able to write outside the chosen folder.</summary>
internal static class LocalNames
{
    private static readonly HashSet<string> Reserved = new(StringComparer.OrdinalIgnoreCase)
    {
        "CON", "PRN", "AUX", "NUL",
        "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9",
        "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9",
    };

    public static StringComparer PathComparer =>
        OperatingSystem.IsLinux() ? StringComparer.Ordinal : StringComparer.OrdinalIgnoreCase;

    public static string CheckEntryName(string name, string what)
    {
        if (name.Length == 0 || name is "." or ".." || name.IndexOfAny(['/', '\\', '\0']) >= 0)
            throw new ToolException($"{what} contains an unsafe entry name: \"{name}\"");
        return name;
    }

    /// <summary>Makes one name usable as a file name here. Returns the same string when it needs no change.</summary>
    public static string ForFileSystem(string name)
    {
        // Android's shared storage and memory cards take no more than Windows does.
        if (!OperatingSystem.IsWindows() && !OperatingSystem.IsAndroid())
            return name;

        var clean = new StringBuilder(name.Length);
        foreach (char c in name)
            clean.Append(c < 32 || c is '<' or '>' or ':' or '"' or '|' or '?' or '*' ? '_' : c);
        while (clean.Length > 0 && clean[^1] is '.' or ' ')
            clean[^1] = '_';

        string result = clean.ToString();
        int dot = result.IndexOf('.');
        return Reserved.Contains(dot >= 0 ? result[..dot] : result) ? "_" + result : result;
    }

    /// <summary>Turns a title name or file stem into something safe to use as a single file name.</summary>
    public static string ForOutputName(string text)
    {
        var clean = new StringBuilder(text.Length);
        foreach (char c in text)
            clean.Append(c < 32 || c is '<' or '>' or ':' or '"' or '|' or '?' or '*' or '/' or '\\' ? '_' : c);
        return clean.ToString().Trim().TrimEnd('.');
    }

    /// <summary>
    /// Where a file this saves goes: the path given if nothing's there, or else the same name with " (2)", " (3)" and on before its
    /// extension. What's there already is somebody's, and may well be what's being read, such as an original game named the way this
    /// names what it makes. Only a path given exactly (-o) is written over.
    /// </summary>
    public static string FreePath(string path)
    {
        string folder = Path.GetDirectoryName(Path.GetFullPath(path))!;
        string stem = Path.GetFileNameWithoutExtension(path), extension = Path.GetExtension(path);
        for (int copy = 1; copy <= MostCopies; copy++)
        {
            string candidate = copy == 1 ? path : Path.Combine(folder, $"{stem} ({copy}){extension}");
            if (!File.Exists(candidate) && !Directory.Exists(candidate))
                return candidate;
        }
        throw new ToolException($"'{folder}' has {MostCopies} files called '{stem}' already, with a number after it. Delete some, or choose another NSP folder");
    }

    private const int MostCopies = 999;

    /// <summary>
    /// The name a file was saved under, as <see cref="FreePath"/> may have made it: without a " (…)" just before its extension, as in
    /// "Game [LZ4] (2).nsp". Versions before 1.5 put " (converted)" there.
    /// </summary>
    public static string WithoutCopyNumber(string fileName)
    {
        string extension = Path.GetExtension(fileName), stem = Path.GetFileNameWithoutExtension(fileName);
        int open = stem.LastIndexOf(" (", StringComparison.Ordinal);
        return open > 0 && stem.EndsWith(')') ? stem[..open] + extension : fileName;
    }

    public static string Combine(string root, string relative)
    {
        string fullRoot = Path.GetFullPath(root);
        string combined = fullRoot;
        foreach (string part in relative.Split('/'))
            combined = Path.Combine(combined, ForFileSystem(part));
        string full = Path.GetFullPath(combined);
        string prefix = fullRoot.EndsWith(Path.DirectorySeparatorChar) ? fullRoot : fullRoot + Path.DirectorySeparatorChar;
        if (!full.StartsWith(prefix, StringComparison.Ordinal))
            throw new ToolException($"Refusing to write outside the output folder: {relative}");
        return full;
    }
}
