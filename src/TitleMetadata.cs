using System.Buffers.Binary;
using System.IO.Compression;
using System.Text;

namespace HacBrewPackLz;

/// <summary>main.npdm is patched in memory only: the file on disk stays as it was.</summary>
internal static class Npdm
{
    public const ulong MinTitleId = 0x0100000000000000;
    public const ulong MaxTitleId = 0x0FFFFFFFFFFFFFFF;

    public static byte[] Load(string path, ulong titleIdOverride, byte[]? acidModulus, out ulong titleId)
    {
        if (!File.Exists(path))
            throw new ToolException($"'{path}' not found, ExeFS needs a main.npdm");

        return Patch(File.ReadAllBytes(path), path, titleIdOverride, acidModulus, out titleId);
    }

    /// <summary>Patches a copy that is already in memory.</summary>
    public static byte[] Patch(byte[] npdm, string path, ulong titleIdOverride, byte[]? acidModulus, out ulong titleId)
    {
        if (npdm.Length < 0x80 || !npdm.AsSpan(0, 4).SequenceEqual("META"u8))
            throw new ToolException($"'{path}' is not a valid NPDM (bad META magic)");

        int aci0 = BinaryPrimitives.ReadInt32LittleEndian(npdm.AsSpan(0x70));
        int acid = BinaryPrimitives.ReadInt32LittleEndian(npdm.AsSpan(0x78));
        if (aci0 < 0 || aci0 + 0x18 > npdm.Length || !npdm.AsSpan(aci0, 4).SequenceEqual("ACI0"u8))
            throw new ToolException($"'{path}' is not a valid NPDM (bad ACI0 section)");
        if (acid < 0 || acid + 0x204 > npdm.Length || !npdm.AsSpan(acid + 0x200, 4).SequenceEqual("ACID"u8))
            throw new ToolException($"'{path}' is not a valid NPDM (bad ACID section)");

        titleId = titleIdOverride != 0 ? titleIdOverride : BinaryPrimitives.ReadUInt64LittleEndian(npdm.AsSpan(aci0 + 0x10));
        if (titleId < MinTitleId || titleId > MaxTitleId)
            throw new ToolException($"Invalid title id {titleId:x16}, the valid range is {MinTitleId:x16} - {MaxTitleId:x16}");
        if (titleId > 0x01FFFFFFFFFFFFFF)
            Ui.Warn($"Title id {titleId:x16} is outside the usual application range");

        if (titleIdOverride != 0)
            BinaryPrimitives.WriteUInt64LittleEndian(npdm.AsSpan(aci0 + 0x10), titleIdOverride);
        acidModulus?.CopyTo(npdm, acid + 0x100);
        return npdm;
    }

    public static ulong ReadTitleId(string path)
    {
        var npdm = File.ReadAllBytes(path);
        int aci0 = BinaryPrimitives.ReadInt32LittleEndian(npdm.AsSpan(0x70));
        return BinaryPrimitives.ReadUInt64LittleEndian(npdm.AsSpan(aci0 + 0x10));
    }
}

/// <summary>control.nacp is patched in memory only: the file on disk stays as it was.</summary>
internal static class Nacp
{
    public const int Size = 0x4000;

    private const int TitleCount = 16;
    private const int MostTitles = 32;
    private const int TitleEntrySize = 0x300;
    private const int NameSize = 0x200;
    private const int PublisherSize = 0x100;
    private const int TitlesFormat = 0x3215;
    private const int CompressedCapacity = 0x2FFE;

    private const int PresenceGroupId = 0x3038;
    private const int AddOnContentBaseId = 0x3070;
    private const int SaveDataOwnerId = 0x3078;
    private const int LocalCommunicationId = 0x30B0;
    private const int LogoHandling = 0x30F1;
    private const int DisplayVersionAt = 0x3060;
    private const int DisplayVersionSize = 0x10;

    /// <summary>The version people know it by, such as 1.0.2. Empty if there's none.</summary>
    public static string DisplayVersion(byte[] nacp)
    {
        if (nacp.Length < DisplayVersionAt + DisplayVersionSize)
            return "";
        var field = nacp.AsSpan(DisplayVersionAt, DisplayVersionSize);
        int end = field.IndexOf((byte)0);
        return Encoding.UTF8.GetString(end < 0 ? field : field[..end]);
    }

    /// <summary>
    /// One for each language, and empty where a language has none. Since firmware 21 they can be stored compressed, which
    /// looks like random symbols to anything that reads them the old way. Nothing at all if they can't be made sense of.
    /// </summary>
    public static List<(string Name, string Publisher)> Titles(byte[] nacp)
    {
        if (nacp.Length < TitleCount * TitleEntrySize)
            return [];
        int format = nacp.Length > TitlesFormat ? nacp[TitlesFormat] : 0;
        if (format == 0)
            return Entries(nacp, TitleCount);
        if (format != 1)
            return [];

        int packed = BinaryPrimitives.ReadUInt16LittleEndian(nacp);
        if (packed == 0 || packed > CompressedCapacity)
            return [];
        var plain = new byte[MostTitles * TitleEntrySize];
        int length = 0;
        try
        {
            using var inflate = new DeflateStream(new MemoryStream(nacp, 2, packed), CompressionMode.Decompress);
            while (length < plain.Length)
            {
                int n = inflate.Read(plain, length, plain.Length - length);
                if (n == 0)
                    break;
                length += n;
            }
        }
        catch (InvalidDataException)
        {
            return [];
        }
        return Entries(plain, length / TitleEntrySize);
    }

    private static List<(string, string)> Entries(byte[] data, int count)
    {
        var titles = new List<(string, string)>(count);
        for (int i = 0; i < count; i++)
            titles.Add((Field(data.AsSpan(i * TitleEntrySize, NameSize)), Field(data.AsSpan(i * TitleEntrySize + NameSize, PublisherSize))));
        return titles;
    }

    private static string Field(ReadOnlySpan<byte> field)
    {
        int end = field.IndexOf((byte)0);
        var text = end >= 0 ? field[..end] : field;
        try
        {
            return new UTF8Encoding(false, true).GetString(text);
        }
        catch (ArgumentException)
        {
            return "";
        }
    }

    public static byte[] Load(string path, ulong titleIdOverride, string? titleName, string? publisher, bool patchLogo)
    {
        if (!File.Exists(path))
            throw new ToolException($"'{path}' not found, the control directory needs a control.nacp");

        var nacp = File.ReadAllBytes(path);
        if (nacp.Length < Size)
            throw new ToolException($"'{path}' is too small to be a NACP ({nacp.Length} bytes)");

        var titles = Titles(nacp);
        if (titleName is null && !titles.Any(t => t.Name.Length > 0))
            throw new ToolException("control.nacp 中的游戏名称无效，请用 --titlename 指定");
        if (publisher is null && !titles.Any(t => t.Publisher.Length > 0))
            throw new ToolException("control.nacp 中的发行商无效，请用 --titlepublisher 指定");

        // To change one of them, they all have to be stored the old way, which only has room for the first languages.
        if ((titleName is not null || publisher is not null) && nacp[TitlesFormat] != 0)
        {
            Array.Clear(nacp, 0, TitleCount * TitleEntrySize);
            for (int i = 0; i < Math.Min(TitleCount, titles.Count); i++)
            {
                WriteString(nacp.AsSpan(i * TitleEntrySize, NameSize), titles[i].Name);
                WriteString(nacp.AsSpan(i * TitleEntrySize + NameSize, PublisherSize), titles[i].Publisher);
            }
            nacp[TitlesFormat] = 0;
        }

        for (int i = 0; i < TitleCount; i++)
        {
            if (titleName is not null)
                WriteString(nacp.AsSpan(i * TitleEntrySize, NameSize), titleName);
            if (publisher is not null)
                WriteString(nacp.AsSpan(i * TitleEntrySize + NameSize, PublisherSize), publisher);
        }

        if (patchLogo)
            nacp[LogoHandling] = 0;

        if (titleIdOverride != 0)
        {
            BinaryPrimitives.WriteUInt64LittleEndian(nacp.AsSpan(PresenceGroupId), titleIdOverride);
            BinaryPrimitives.WriteUInt64LittleEndian(nacp.AsSpan(SaveDataOwnerId), titleIdOverride);
            BinaryPrimitives.WriteUInt64LittleEndian(nacp.AsSpan(AddOnContentBaseId), titleIdOverride + 0x1000);
            for (int i = 0; i < 8; i++)
                BinaryPrimitives.WriteUInt64LittleEndian(nacp.AsSpan(LocalCommunicationId + i * 8), titleIdOverride);
        }
        return nacp;
    }

    private static void WriteString(Span<byte> field, string value)
    {
        field.Clear();
        var bytes = Encoding.UTF8.GetBytes(value);
        if (bytes.Length >= field.Length)
            throw new ToolException($"'{value}' is longer than {field.Length - 1} bytes");
        bytes.CopyTo(field);
    }
}

internal sealed record ContentRecord(NcaInfo Nca, byte Type, byte IdOffset = 0);

internal sealed record ContentMetaRecord(string Id, long Size, byte Type, byte IdOffset);

/// <summary>A parsed content meta file from an existing NSP.</summary>
internal sealed class ContentMeta
{
    private const int HeaderSize = 0x20;
    private const int RecordSize = 0x38;

    public required byte[] Raw { get; init; }
    public required ulong TitleId { get; init; }
    public required uint Version { get; init; }
    public required byte Type { get; init; }
    public required int ExtendedHeaderSize { get; init; }
    public required List<ContentMetaRecord> Records { get; init; }

    /// <summary>For an update or a DLC, the title it belongs to. For a base game, its update's title ID.</summary>
    public ulong LinkedId => ExtendedHeaderSize >= 8 ? BinaryPrimitives.ReadUInt64LittleEndian(Raw.AsSpan(HeaderSize)) : 0;

    /// <summary>Only base games and updates have this field.</summary>
    public uint RequiredSystemVersion =>
        Type is Cnmt.MetaApplication or Cnmt.MetaPatch && ExtendedHeaderSize >= 0xC
            ? BinaryPrimitives.ReadUInt32LittleEndian(Raw.AsSpan(HeaderSize + 8))
            : 0;

    public static ContentMeta Parse(byte[] raw, string what)
    {
        if (raw.Length < HeaderSize)
            throw new ToolException($"{what} has a damaged content meta");
        var r = raw.AsSpan();
        int extended = BinaryPrimitives.ReadUInt16LittleEndian(r[0x0E..]);
        int count = BinaryPrimitives.ReadUInt16LittleEndian(r[0x10..]);
        if (HeaderSize + extended + count * (long)RecordSize > raw.Length)
            throw new ToolException($"{what} has a damaged content meta");

        var records = new List<ContentMetaRecord>(count);
        for (int i = 0; i < count; i++)
        {
            var e = r.Slice(HeaderSize + extended + i * RecordSize, RecordSize);
            long size = 0;
            for (int b = 4; b >= 0; b--)
                size = (size << 8) | e[0x30 + b];
            records.Add(new ContentMetaRecord(Convert.ToHexString(e.Slice(0x20, 0x10)).ToLowerInvariant(), size, e[0x36], e[0x37]));
        }

        return new ContentMeta
        {
            Raw = raw,
            TitleId = BinaryPrimitives.ReadUInt64LittleEndian(r),
            Version = BinaryPrimitives.ReadUInt32LittleEndian(r[8..]),
            Type = r[0x0C],
            ExtendedHeaderSize = extended,
            Records = records,
        };
    }

    public int RebuiltSize(int recordCount) => HeaderSize + ExtendedHeaderSize + recordCount * RecordSize + 0x20;

    /// <summary>A copy of this content meta with a new version and new content records.</summary>
    public byte[] Rebuild(uint version, uint? requiredSystemVersion, IReadOnlyList<ContentRecord> records)
    {
        int prefix = HeaderSize + ExtendedHeaderSize;
        var cnmt = new byte[RebuiltSize(records.Count)];
        var c = cnmt.AsSpan();
        Raw.AsSpan(0, prefix).CopyTo(c);
        BinaryPrimitives.WriteUInt32LittleEndian(c[8..], version);
        BinaryPrimitives.WriteUInt16LittleEndian(c[0x10..], (ushort)records.Count);
        BinaryPrimitives.WriteUInt16LittleEndian(c[0x12..], 0);
        if (requiredSystemVersion is { } required && Type is Cnmt.MetaApplication or Cnmt.MetaPatch && ExtendedHeaderSize >= 0xC)
            BinaryPrimitives.WriteUInt32LittleEndian(c[(HeaderSize + 8)..], required);
        // A game asking for a newer version of itself than the one that's in here would only have the console say to update it before it starts.
        if (Type == Cnmt.MetaApplication && ExtendedHeaderSize >= 0x10)
            BinaryPrimitives.WriteUInt32LittleEndian(c[(HeaderSize + 0xC)..], 0);
        Cnmt.WriteRecords(c[prefix..], records);
        return cnmt;
    }
}

internal static class Cnmt
{
    public const byte ContentTypeMeta = 0;
    public const byte ContentTypeProgram = 1;
    public const byte ContentTypeData = 2;
    public const byte ContentTypeControl = 3;
    public const byte ContentTypeHtmlDocument = 4;
    public const byte ContentTypeLegalInformation = 5;
    public const byte ContentTypeDeltaFragment = 6;

    public const byte MetaApplication = 0x80;
    public const byte MetaPatch = 0x81;
    public const byte MetaAddOnContent = 0x82;

    public static string FileName(ulong titleId) => $"Application_{titleId:x16}.cnmt";

    public static string FileName(ulong titleId, byte metaType) => metaType switch
    {
        MetaAddOnContent => $"AddOnContent_{titleId:x16}.cnmt",
        MetaPatch => $"Patch_{titleId:x16}.cnmt",
        _ => FileName(titleId),
    };

    public static void WriteRecords(Span<byte> destination, IReadOnlyList<ContentRecord> records)
    {
        for (int i = 0; i < records.Count; i++)
        {
            var r = destination[(i * 0x38)..];
            records[i].Nca.Hash.CopyTo(r);
            records[i].Nca.Hash.AsSpan(0, 16).CopyTo(r[0x20..]);
            long size = records[i].Nca.Size;
            for (int b = 0; b < 6; b++)
                r[0x30 + b] = (byte)(size >> (8 * b));
            r[0x36] = records[i].Type;
            r[0x37] = records[i].IdOffset;
        }
    }

    public static int BuiltSize(int recordCount) => 0x20 + 0x10 + recordCount * 0x38 + 0x20;

    public static byte[] Build(ulong titleId, uint titleVersion, uint requiredSystemVersion, IReadOnlyList<ContentRecord> records)
    {
        var cnmt = new byte[BuiltSize(records.Count)];
        var c = cnmt.AsSpan();
        BinaryPrimitives.WriteUInt64LittleEndian(c, titleId);
        BinaryPrimitives.WriteUInt32LittleEndian(c[8..], titleVersion);
        c[0x0C] = 0x80;
        BinaryPrimitives.WriteUInt16LittleEndian(c[0x0E..], 0x10);
        BinaryPrimitives.WriteUInt16LittleEndian(c[0x10..], (ushort)records.Count);

        BinaryPrimitives.WriteUInt64LittleEndian(c[0x20..], titleId + 0x800);
        BinaryPrimitives.WriteUInt32LittleEndian(c[0x28..], requiredSystemVersion);

        WriteRecords(c[0x30..], records);
        return cnmt;
    }
}
