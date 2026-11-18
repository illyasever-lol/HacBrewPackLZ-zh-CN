using System.Globalization;
using System.Text;

namespace HacBrewPackLz;

/// <summary>
/// Where an update put each part of the original game it kept as it was. A game compressed together with its update holds those parts
/// where that update put them, so with this, a newer update, which reads the same parts of the original, can be made part of it later
/// without the original. It's kept as a small file in the game's control data, beside its name and icons, where nothing else looks.
/// </summary>
internal sealed class UpdateMap
{
    public const string FileName = "hacbrewpacklz_update_map.txt";
    private const string FirstLine = "hacBrewPackLZ update map 1";

    /// <summary>Bytes of the original from <see cref="Original"/> on, which the updated game data has at <see cref="Image"/>.</summary>
    public sealed record Range(long Original, long Image, long Length);

    /// <summary>One section of one content.</summary>
    public sealed record Section(byte ContentType, byte IdOffset, int Index, Range[] Ranges);

    public required uint Version { get; init; }
    public required List<Section> Sections { get; init; }

    /// <summary>
    /// What a game made from this job keeps of the original, or nothing if there's nothing to keep track of. Only what's kept as it was
    /// in the game data counts: an update compressed by its makers keeps some of the original inside what it compresses. A copy made again,
    /// compressed, with no update of its own keeps the map it was made with, as its next update needs it. Uncompressed, its control data
    /// goes in as it is, map and all.
    /// </summary>
    public static UpdateMap? For(TitleJob job, bool compress)
    {
        if (job.Update is null)
            return compress ? In(job) : null;
        var sections = new List<Section>();
        foreach (var content in job.Contents.Where(c => c.Base is not null && c.Patch is not null))
        {
            foreach (var section in content.Patch!.Sections)
            {
                if (section is null || !section.HasIndirect || section.FsType != NcaFsSection.FsRomFs)
                    continue;
                sections.Add(new Section(content.Type, content.IdOffset, section.Index, content.Patch.KeptInImage(section, content.Base)));
            }
        }
        return sections.Count > 0 ? new UpdateMap { Version = job.Update.Meta.Version, Sections = sections } : null;
    }

    /// <summary>The map a copy was made with, in its control data, if it has one for its version.</summary>
    public static UpdateMap? In(TitleJob job)
    {
        foreach (var content in job.Contents.Where(c => c.Type == Cnmt.ContentTypeControl && c.Base is not null))
        {
            foreach (var section in content.Base!.Sections)
            {
                if (section is null || section.FsType != NcaFsSection.FsRomFs)
                    continue;
                var image = content.Base.OpenRomFsImage(section, null);
                var file = RomFsArchive.Open(image, $"{content.Base.Name} 区段 {section.Index}").Files.FirstOrDefault(f => f.Path == FileName);
                if (file is null || file.Size > 64 << 20)
                    continue;
                var data = new byte[file.Size];
                image.Read(file.Offset, data, 0, data.Length);
                if (Parse(data) is { } map && map.Version == job.Main.Meta.Version)
                    return map;
            }
        }
        return null;
    }

    public byte[] ToBytes()
    {
        var text = new StringBuilder();
        text.Append(FirstLine).Append('\n');
        text.Append($"update {Version}\n");
        foreach (var section in Sections)
        {
            text.Append($"section {section.ContentType} {section.IdOffset} {section.Index} {section.Ranges.Length}\n");
            foreach (var range in section.Ranges)
                text.Append($"{range.Original:x} {range.Image:x} {range.Length:x}\n");
        }
        return Encoding.ASCII.GetBytes(text.ToString());
    }

    /// <summary>Nothing, for anything that isn't a map this made.</summary>
    public static UpdateMap? Parse(byte[] data)
    {
        try
        {
            var lines = Encoding.ASCII.GetString(data).Split('\n');
            if (lines.Length < 2 || lines[0] != FirstLine || !lines[1].StartsWith("update ", StringComparison.Ordinal))
                return null;
            var map = new UpdateMap { Version = uint.Parse(lines[1][7..], CultureInfo.InvariantCulture), Sections = [] };
            int at = 2;
            while (at < lines.Length && lines[at].StartsWith("section ", StringComparison.Ordinal))
            {
                var f = lines[at++].Split(' ');
                int count = int.Parse(f[4], CultureInfo.InvariantCulture);
                var ranges = new Range[count];
                for (int i = 0; i < count; i++)
                {
                    var r = lines[at++].Split(' ');
                    ranges[i] = new Range(Hex(r[0]), Hex(r[1]), Hex(r[2]));
                }
                map.Sections.Add(new Section(byte.Parse(f[1], CultureInfo.InvariantCulture), byte.Parse(f[2], CultureInfo.InvariantCulture),
                    int.Parse(f[3], CultureInfo.InvariantCulture), ranges));
            }
            return map;
        }
        catch (Exception e) when (e is FormatException or OverflowException or IndexOutOfRangeException)
        {
            return null;
        }

        static long Hex(string s) => long.Parse(s, NumberStyles.HexNumber, CultureInfo.InvariantCulture);
    }
}

/// <summary>
/// The original game's section as an update reads it, from a copy that has an earlier update in it: each part the earlier update kept
/// is where it put it, in the copy's game data. A part it didn't keep isn't there, and says so.
/// </summary>
internal sealed class KeptOriginalSource : DataSource
{
    private readonly DataSource _image;
    private readonly long[] _starts;
    private readonly UpdateMap.Range[] _ranges;
    private readonly string _name;
    /// <summary>How the game it reads was made, "compressed" or "repacked", which a part it doesn't have is said with.</summary>
    private readonly string _kind;

    public KeptOriginalSource(DataSource image, UpdateMap.Section map, string name, string kind)
    {
        _image = image;
        _name = name;
        _kind = kind;
        _ranges = map.Ranges.OrderBy(r => r.Original).ToArray();
        _starts = _ranges.Select(r => r.Original).ToArray();
        Length = _ranges.Length == 0 ? 0 : _ranges.Max(r => r.Original + r.Length);
    }

    public override long Length { get; }
    public override bool ReadsAtOnce => _image.ReadsAtOnce;

    public override void Read(long offset, byte[] buffer, int bufferOffset, int count)
    {
        while (count > 0)
        {
            int i = Array.BinarySearch(_starts, offset);
            if (i < 0)
                i = ~i - 1;
            // Of the parts starting at or before it, one that reaches it: parts can overlap, as an update can take one part twice.
            int found = -1;
            for (int j = i; j >= 0 && found < 0; j--)
            {
                if (offset < _ranges[j].Original + _ranges[j].Length)
                    found = j;
            }
            if (found < 0)
                throw new MismatchException($"{_name}: the update needs a part of the original game that the {_kind} game doesn't have");
            var range = _ranges[found];
            int n = (int)Math.Min(count, range.Original + range.Length - offset);
            _image.Read(range.Image + (offset - range.Original), buffer, bufferOffset, n);
            offset += n;
            bufferOffset += n;
            count -= n;
        }
    }

    public override void Release() => _image.Release();
}
