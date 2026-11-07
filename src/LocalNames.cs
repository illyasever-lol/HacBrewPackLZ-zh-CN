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
        if (!OperatingSystem.IsWindows())
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
