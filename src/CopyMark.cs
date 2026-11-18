using System.Globalization;
using System.Text;

namespace HacBrewPackLz;

/// <summary>
/// What a copy this tool made says about itself, kept as a small file in its control data beside its name and icons, where nothing else looks:
/// which version made it, so a later one knows which copies it can make smaller still, and what the game came to before, so what compressing it
/// freed can be shown for as long as it's there. The Switch app writes the same bytes.
/// </summary>
internal sealed record CopyMark(string MadeBy, long OriginalBytes)
{
    public const string FileName = "hacbrewpacklz_copy.txt";
    private const string FirstLine = "hacBrewPackLZ copy 1";

    /// <summary>The first version that stores what a game holds more than once only once, wherever it is. A copy from before it can come out
    /// smaller made again.</summary>
    public const string StoresRepeatsOnce = "1.5.0";

    /// <summary>Whether this copy already stores repeats once, as this version would make it.</summary>
    public bool IsCurrent => CompareVersions(MadeBy, StoresRepeatsOnce) >= 0;

    /// <summary>
    /// What a copy made from this job says: this version, and the game it's made of as it was before this tool made anything of it, which is
    /// what was installed. A copy made again keeps what the first one said that was, and one with nothing to say has it worked out as it
    /// unpacks.
    /// </summary>
    public static CopyMark For(TitleJob job, Options options)
    {
        if (In(job) is { } before)
            return new CopyMark(AppInfo.Version, before.OriginalBytes);
        if (Analyzer.IsMadeByThisTool(job))
            return new CopyMark(AppInfo.Version, Analyzer.UnpackedBytes(job, options));
        return new CopyMark(AppInfo.Version, InstalledBytes(job));
    }

    /// <summary>What the game takes installed as it is: its contents as the content meta has them, whatever they're packed in, and those of the
    /// update made part of it.</summary>
    public static long InstalledBytes(TitleJob job) =>
        job.Main.Meta.Records.Where(Counts).Sum(r => r.Size) + (job.Update?.Meta.Records.Where(Counts).Sum(r => r.Size) ?? 0);

    private static bool Counts(ContentMetaRecord record) => record.Type is not (Cnmt.ContentTypeMeta or Cnmt.ContentTypeDeltaFragment);

    public byte[] ToBytes() => Encoding.ASCII.GetBytes($"{FirstLine}\nmade by {MadeBy}\noriginal {OriginalBytes.ToString(CultureInfo.InvariantCulture)}\n");

    /// <summary>Nothing, for anything that isn't a mark this made.</summary>
    public static CopyMark? Parse(byte[] data)
    {
        var lines = Encoding.ASCII.GetString(data).Split('\n');
        if (lines.Length < 3 || lines[0] != FirstLine || !lines[1].StartsWith("made by ", StringComparison.Ordinal) || !lines[2].StartsWith("original ", StringComparison.Ordinal))
            return null;
        return long.TryParse(lines[2]["original ".Length..], NumberStyles.None, CultureInfo.InvariantCulture, out long original)
            ? new CopyMark(lines[1]["made by ".Length..], original) : null;
    }

    /// <summary>The mark in a game's control data, if it has one.</summary>
    public static CopyMark? In(TitleJob job) =>
        job.Contents.Where(c => c.Type == Cnmt.ContentTypeControl).Select(c => InControl(c.Primary)).FirstOrDefault(m => m is not null);

    /// <summary>The mark in a control NCA, if it has one.</summary>
    public static CopyMark? InControl(NcaFile nca)
    {
        foreach (var section in nca.Sections)
        {
            if (section is null || section.FsType != NcaFsSection.FsRomFs || section.HasIndirect)
                continue;
            var image = nca.OpenRomFsImage(section, null, verify: false);
            var file = RomFsArchive.Open(image, $"{nca.Name} section {section.Index}").Files.FirstOrDefault(f => f.Path == FileName);
            if (file is null || file.Size > 4096)
                continue;
            var data = new byte[file.Size];
            image.Read(file.Offset, data, 0, data.Length);
            return Parse(data);
        }
        return null;
    }

    /// <summary>Versions by their numbers, 1.10 after 1.9, and a test version as the one it leads to.</summary>
    public static int CompareVersions(string a, string b)
    {
        int[] Parts(string v) => v.Split('-')[0].Split('.').Select(p => int.TryParse(p, NumberStyles.None, CultureInfo.InvariantCulture, out int n) ? n : 0).ToArray();
        int[] x = Parts(a), y = Parts(b);
        for (int i = 0; i < Math.Max(x.Length, y.Length); i++)
        {
            int c = (i < x.Length ? x[i] : 0).CompareTo(i < y.Length ? y[i] : 0);
            if (c != 0)
                return c;
        }
        return 0;
    }
}
