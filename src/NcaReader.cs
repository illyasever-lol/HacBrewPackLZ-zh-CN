using System.Buffers.Binary;
using System.Security.Cryptography;

namespace HacBrewPackLz;

/// <summary>Title keys from the tickets inside the given NSPs, and from title.keys beside the keyset.</summary>
internal sealed class TicketStore(Keyset keyset)
{
    private const int MaxTicketSize = 0x4000;

    private readonly Dictionary<string, byte[]> _encryptedKeys = new(StringComparer.OrdinalIgnoreCase);
    private readonly HashSet<string> _personalized = new(StringComparer.OrdinalIgnoreCase);

    /// <summary>Files get opened several at a time.</summary>
    private readonly object _gate = new();

    public void AddFrom(GameArchive nsp)
    {
        foreach (var entry in nsp.Entries)
        {
            if (!entry.Name.EndsWith(".tik", StringComparison.OrdinalIgnoreCase) || entry.Size > MaxTicketSize)
                continue;
            var ticket = new byte[entry.Size];
            nsp.Open(entry).Read(0, ticket, 0, ticket.Length);
            lock (_gate)
                AddTicket(ticket);
        }
    }

    public byte[] GetTitleKey(ReadOnlySpan<byte> rightsId, int generationIndex, string ncaName)
    {
        string id = Convert.ToHexString(rightsId);
        byte[]? encrypted;
        bool personalized;
        lock (_gate)
        {
            _encryptedKeys.TryGetValue(id, out encrypted);
            personalized = _personalized.Contains(id);
        }
        if (encrypted is not null)
        {
            var kek = keyset.TryTitleKek(generationIndex)
                ?? throw new ToolException($"titlekek_{generationIndex:x2} is not in the keyset, it is needed for {ncaName}. Your keys may be older than this game");
            return NcaCrypto.EcbDecrypt(kek, encrypted);
        }
        if (keyset.TryTitleKey(id) is { } plain)
            return plain;
        if (personalized)
            throw new ToolException($"{ncaName} uses a personalized ticket tied to one console, which isn't supported. Add its key to title.keys beside your keyset");
        throw new ToolException($"{ncaName} needs a title key, but the NSP has no ticket for rights ID {id}. Add its key to title.keys beside your keyset");
    }

    private void AddTicket(byte[] ticket)
    {
        if (ticket.Length < 4)
            return;
        int bodyOffset = BinaryPrimitives.ReadUInt32LittleEndian(ticket) switch
        {
            0x10000 or 0x10003 => 4 + 0x200 + 0x3C,
            0x10001 or 0x10004 => 4 + 0x100 + 0x3C,
            0x10002 or 0x10005 => 4 + 0x3C + 0x40,
            _ => -1,
        };
        if (bodyOffset < 0 || ticket.Length < bodyOffset + 0x170)
            return;

        var body = ticket.AsSpan(bodyOffset);
        string rightsId = Convert.ToHexString(body.Slice(0x160, 0x10));
        if (body[0x141] == 0)
            _encryptedKeys[rightsId] = body.Slice(0x40, 0x10).ToArray();
        else
            _personalized.Add(rightsId);
    }
}

internal sealed class NcaFsSection
{
    public const byte FsRomFs = 0;
    public const byte FsPartition = 1;
    public const byte HashSha256 = 2;
    public const byte HashIntegrity = 3;
    public const byte HashSha3 = 5;
    public const byte HashIntegritySha3 = 6;
    public const byte EncryptNone = 1;
    public const byte EncryptCtr = 3;
    public const byte EncryptCtrEx = 4;

    public required int Index { get; init; }
    public required long Offset { get; init; }
    public required long Size { get; init; }
    public required byte[] Header { get; init; }

    public byte FsType => Header[2];
    public byte HashType => Header[3];
    public byte Encryption => Header[4];
    public ulong UpperIv => BinaryPrimitives.ReadUInt64LittleEndian(Header.AsSpan(0x140));
    public uint SecureValue => BinaryPrimitives.ReadUInt32LittleEndian(Header.AsSpan(0x144));

    public long IndirectOffset => BinaryPrimitives.ReadInt64LittleEndian(Header.AsSpan(0x100));
    public long IndirectSize => BinaryPrimitives.ReadInt64LittleEndian(Header.AsSpan(0x108));
    public ReadOnlySpan<byte> IndirectHeader => Header.AsSpan(0x110, 0x10);
    public long CtrExOffset => BinaryPrimitives.ReadInt64LittleEndian(Header.AsSpan(0x120));
    public long CtrExSize => BinaryPrimitives.ReadInt64LittleEndian(Header.AsSpan(0x128));
    public ReadOnlySpan<byte> CtrExHeader => Header.AsSpan(0x130, 0x10);
    public bool HasIndirect => IndirectSize != 0;

    public long SparseTableOffset => BinaryPrimitives.ReadInt64LittleEndian(Header.AsSpan(0x148));
    public long SparseTableSize => BinaryPrimitives.ReadInt64LittleEndian(Header.AsSpan(0x150));
    public ReadOnlySpan<byte> SparseTableHeader => Header.AsSpan(0x158, 0x10);
    public long SparseDataOffset => BinaryPrimitives.ReadInt64LittleEndian(Header.AsSpan(0x168));
    public ushort SparseGeneration => BinaryPrimitives.ReadUInt16LittleEndian(Header.AsSpan(0x170));
    /// <summary>Most of it isn't in the file, as this base game was cut down to what its update still reads from it.</summary>
    public bool HasSparse => SparseGeneration != 0;

    public long CompressionOffset => BinaryPrimitives.ReadInt64LittleEndian(Header.AsSpan(0x178));
    public long CompressionSize => BinaryPrimitives.ReadInt64LittleEndian(Header.AsSpan(0x180));
    public ReadOnlySpan<byte> CompressionHeader => Header.AsSpan(0x188, 0x10);
    public bool HasCompression => CompressionOffset != 0 && CompressionSize != 0;

    /// <summary>Where the data level of its IVFC hash tree starts, past the hash levels, and how long it is.</summary>
    public long DataLevelOffset => DataLevelField(0);
    public long DataLevelSize => DataLevelField(8);

    private long DataLevelField(int at)
    {
        int levels = BinaryPrimitives.ReadInt32LittleEndian(Header.AsSpan(0x14));
        return levels >= 2 && levels <= 7 ? BinaryPrimitives.ReadInt64LittleEndian(Header.AsSpan(0x18 + (levels - 2) * 0x18 + at)) : 0;
    }
}

/// <summary>
/// Reads an existing NCA in the same layer order as the firmware: decryption (with AES-CTR-Ex and the update's
/// indirect table when there is one), then the hash tree, then decompression. Every block is checked against the
/// hash tree on the way through, so damaged input or an update that doesn't belong to its base stops the job.
/// </summary>
internal sealed class NcaFile
{
    private const int KeyIndexCtr = 2;

    private readonly DataSource _raw;
    private readonly NczSource? _ncz;
    private readonly Keyset _keyset;
    private readonly TicketStore _tickets;
    private byte[]? _contentKey;

    public string Name { get; }
    public byte[] Header { get; }
    public NcaFsSection?[] Sections { get; } = new NcaFsSection?[4];

    public NcaContentType ContentType => (NcaContentType)Header[0x205];
    public ulong ProgramId => BinaryPrimitives.ReadUInt64LittleEndian(Header.AsSpan(0x210));
    public int ContentIndex => BinaryPrimitives.ReadInt32LittleEndian(Header.AsSpan(0x218));
    public uint SdkVersion => BinaryPrimitives.ReadUInt32LittleEndian(Header.AsSpan(0x21C));
    public int GenerationIndex => Math.Max(Math.Max(Header[0x206], Header[0x220]) - 1, 0);
    public bool HasRightsId => Header.AsSpan(0x230, 0x10).IndexOfAnyExcept((byte)0) >= 0;
    /// <summary>Whether any of it is only there in part, and needs an update to be whole.</summary>
    public bool HoldsOnlyPart => Sections.Any(s => s is { HasSparse: true });

    /// <summary>Data that can only be read efficiently from front to back, which is the case for a solid NCZ.</summary>
    public bool ReadsInOrder => _ncz is { IsSolid: true };

    /// <summary>Packed with Zstandard, as in an NSZ or XCZ, which the console can't read, so every installer unpacks it.</summary>
    public bool Packed => _ncz is not null;

    public NcaFile(DataSource raw, string name, Keyset keyset, TicketStore tickets)
    {
        _raw = raw;
        _ncz = raw as NczSource;
        _keyset = keyset;
        _tickets = tickets;
        Name = name;
        if (raw.Length < 0xC00)
            throw new ToolException($"{name} is only {raw.Length} bytes, where an NCA takes at least 3 KB");

        Header = new byte[0xC00];
        raw.Read(0, Header, 0, Header.Length);
        NcaCrypto.XtsTransform(keyset.HeaderKey, Header, Header.Length, encrypt: false);
        if (!Header.AsSpan(0x200, 4).SequenceEqual("NCA3"u8))
            throw new ToolException($"{name} isn't a readable NCA3. It may be damaged, or your header_key may be wrong");

        for (int i = 0; i < Sections.Length; i++)
        {
            long start = BinaryPrimitives.ReadUInt32LittleEndian(Header.AsSpan(0x240 + i * 0x10)) * 0x200L;
            long end = BinaryPrimitives.ReadUInt32LittleEndian(Header.AsSpan(0x244 + i * 0x10)) * 0x200L;
            if (start == 0 && end == 0)
                continue;

            var fsHeader = Header.AsSpan(0x400 + i * 0x200, 0x200).ToArray();
            if (!SHA256.HashData(fsHeader).AsSpan().SequenceEqual(Header.AsSpan(0x280 + i * 0x20, 0x20)))
                throw new ToolException($"{name} section {i} has a damaged FS header");
            var section = new NcaFsSection { Index = i, Offset = start, Size = end - start, Header = fsHeader };
            // A section that's been cut down still says where it used to be, which can be well past the end of the file.
            if (start < 0xC00 || end < start || (end > raw.Length && !section.HasSparse))
                throw new ToolException($"{name} section {i} lies outside the file");
            Sections[i] = section;
            _ncz?.KeepIfSmall(start, end - start);
        }
    }

    /// <summary>Lets go of what a compressed source holds on to. It's picked up again if the NCA is read again.</summary>
    public void ReleaseMemory() => _ncz?.ReleaseMemory();

    /// <summary>The section's bytes after decryption, with no other layers applied.</summary>
    public DataSource OpenPhysical(NcaFsSection section)
    {
        // An NCZ holds its sections decrypted, so neither keys nor tickets come into it.
        DataSource raw = section.HasSparse ? OpenSparse(section) : new SubSource(_raw, section.Offset, section.Size);
        if (_ncz is not null)
            return raw;

        switch (section.Encryption)
        {
            case NcaFsSection.EncryptNone:
                return raw;
            case NcaFsSection.EncryptCtr:
                return new AesCtrSource(raw, ContentKey, section.Offset, section.UpperIv);
            case NcaFsSection.EncryptCtrEx:
                // An update that changes nothing in this section has none of its own data in it, and so nothing for the table to say.
                if (section.CtrExSize == 0)
                    return new AesCtrSource(raw, ContentKey, section.Offset, section.UpperIv);
                var table = ReadMetaTable(section, section.CtrExOffset, section.CtrExSize);
                return new AesCtrExSource(raw, ContentKey, section.Offset, section.SecureValue, table, section.CtrExHeader, Name);
            default:
                throw new ToolException($"{Name} section {section.Index} uses encryption type {section.Encryption}, which isn't supported");
        }
    }

    private DataSource OpenSparse(NcaFsSection section)
    {
        // With nothing listed, none of it is here at all, and the update has all of it.
        if (BinaryPrimitives.ReadInt32LittleEndian(section.SparseTableHeader[8..]) == 0)
            return new ZeroSource(section.Size);

        long size = section.SparseTableSize;
        if (!Ranges.Fits(section.SparseDataOffset, section.SparseTableOffset, _raw.Length) || size <= 0 || size > int.MaxValue ||
            !Ranges.Fits(section.SparseDataOffset + section.SparseTableOffset, size, _raw.Length))
            throw new ToolException($"{Name} section {section.Index} has a sparse table outside the file");
        long start = section.SparseDataOffset + section.SparseTableOffset;

        // Its table is encrypted where it now is, with a counter that says which time this section was cut down. The console decrypts it
        // whatever the section's own data is, so a copy that keeps its data unencrypted still has it encrypted, and it's read the same way.
        // An NCZ, as NXPress writes one of a game cut down (NCZSECT2), holds the table decrypted, as it does each piece.
        DataSource raw = new SubSource(_raw, start, size);
        if (_ncz is null)
            raw = new AesCtrSource(raw, ContentKey, start, ((ulong)section.SecureValue << 32) | ((uint)section.SparseGeneration << 16));
        var table = new byte[size];
        raw.Read(0, table, 0, table.Length);
        return new SparseSource(_raw, section.SparseDataOffset, section.Size, table, section.SparseTableHeader, Name);
    }

    /// <summary>The section as the hash tree sees it: an update's section is rebuilt from its base and its own data.</summary>
    public DataSource OpenLogical(NcaFsSection section, NcaFile? baseNca)
    {
        if (section.HasSparse)
            throw new ToolException($"{Name} only holds part of the game, and the rest of it is in its update");
        var physical = OpenPhysical(section);
        if (!section.HasIndirect)
            return physical;

        var table = ReadMetaTable(section, section.IndirectOffset, section.IndirectSize);
        DataSource? original = null;
        if (baseNca?.Sections[section.Index] is { } baseSection)
        {
            original = baseNca.OpenAsOriginal(baseSection);
            // A copy this compressed is read as the original was, which is what an update made for the original reads. An update made
            // for the copy itself, as stored, reads it as it is: whichever its own hashes say it is, worked out once.
            if (baseNca.IsRebuiltCompressed(baseSection) && baseNca.KeptOriginal.Count == 0)
            {
                if (!_readsStored.TryGetValue(section.Index, out bool stored))
                {
                    // An update this app made isn't encrypted, as an official one always is, and it was made for a copy as it's stored, which is
                    // quick to see. Seeing whether an update reads a copy as its original was can take working out what the original had before
                    // its data, over all of it, so that's only looked at first for an official update, which always reads the original.
                    var asStored = new IndirectSource(baseNca.OpenPhysical(baseSection), physical, table, section.IndirectHeader, Name);
                    stored = section.Encryption == NcaFsSection.EncryptNone
                        ? Fits(section, asStored)
                        : !Fits(section, new IndirectSource(original, physical, table, section.IndirectHeader, Name)) && Fits(section, asStored);
                    _readsStored[section.Index] = stored;
                }
                if (stored)
                    original = baseNca.OpenPhysical(baseSection);
            }
        }
        return new IndirectSource(original, physical, table, section.IndirectHeader, Name);
    }

    /// <summary>
    /// What an update reads from this as its base game. An update is made against the original's stored bytes, which compressing
    /// changes, but a compressed copy this tool made holds the very same game data. So the section is given as the original had it,
    /// uncompressed, rebuilt from that, and the update fits it. The update's own hash tree says whether it really does.
    /// </summary>
    public DataSource OpenAsOriginal(NcaFsSection section)
    {
        if (KeptOriginal.FirstOrDefault(s => s.Index == section.Index) is { } kept)
            return new KeptOriginalSource(OpenRomFsImage(section, null), kept, Name, IsRebuiltCompressed(section) ? "compressed" : "repacked");
        return IsRebuiltCompressed(section) ? new UncompressedRomFsSource(OpenRomFsImage(section, null)) : OpenPhysical(section);
    }

    /// <summary>For a copy with an update in it already, where that update kept each part of the original, which a newer one reads.</summary>
    public List<UpdateMap.Section> KeptOriginal { get; } = [];

    /// <summary>
    /// Where the parts of the original an update's section keeps as they were end up in the game data it makes. Through its table to
    /// where it keeps them, and if it's compressed, through its compression table to where a stretch stored as it is ends up. What's
    /// kept in its hash levels, or inside what it compresses, isn't there as it was, and is left out.
    /// </summary>
    public UpdateMap.Range[] KeptInImage(NcaFsSection section, NcaFile? baseNca)
    {
        long data = section.DataLevelOffset;
        var kept = new List<(long virt, long original, long length)>();
        foreach (var r in KeptFromOriginal(section))
        {
            long skip = Math.Max(data - r.Virtual, 0);
            if (skip < r.Length)
                kept.Add((r.Virtual + skip - data, r.Original + skip, r.Length - skip));
        }
        if (!section.HasCompression)
            return [.. kept.Select(k => new UpdateMap.Range(k.original, k.virt, k.length))];

        // The compression table is new data of the update's own, at the end of its data level.
        var stream = new SubSource(OpenLogical(section, baseNca), data, section.DataLevelSize);
        long offset = section.CompressionOffset, size = section.CompressionSize;
        if (size <= 0 || size > int.MaxValue || !Ranges.Fits(offset, size, stream.Length))
            throw new ToolException($"{Name} section {section.Index} has a compression table outside its data");
        var table = new byte[size];
        stream.Read(offset, table, 0, table.Length);
        int count = BucketTree.ReadHeader(section.CompressionHeader, Name);
        var virt = new long[count];
        var phys = new long[count];
        var types = new byte[count];
        long end = BucketTree.EnumerateEntries(table, 0x18, count, Name, (i, e) =>
        {
            virt[i] = BinaryPrimitives.ReadInt64LittleEndian(e);
            phys[i] = BinaryPrimitives.ReadInt64LittleEndian(e[8..]);
            types[i] = e[0x10];
        });
        var stored = new List<(long phys, long length, long virt)>();
        for (int i = 0; i < count; i++)
        {
            if (types[i] == 0)
                stored.Add((phys[i], (i + 1 < count ? virt[i + 1] : end) - virt[i], virt[i]));
        }
        stored.Sort((a, b) => a.phys.CompareTo(b.phys));
        var starts = stored.Select(s => s.phys).ToArray();

        var ranges = new List<UpdateMap.Range>();
        foreach (var (p, original, length) in kept)
        {
            int i = Array.BinarySearch(starts, p);
            if (i < 0)
                i = Math.Max(~i - 1, 0);
            for (; i < stored.Count && stored[i].phys < p + length; i++)
            {
                long s = Math.Max(p, stored[i].phys), e = Math.Min(p + length, stored[i].phys + stored[i].length);
                if (e > s)
                    ranges.Add(new UpdateMap.Range(original + (s - p), stored[i].virt + (s - stored[i].phys), e - s));
            }
        }
        return [.. ranges];
    }

    /// <summary>The parts of the original an update's section reads as they were, from its table, by where they are in the section.</summary>
    private (long Virtual, long Original, long Length)[] KeptFromOriginal(NcaFsSection section)
    {
        const int EntrySize = 0x14;
        var table = ReadMetaTable(section, section.IndirectOffset, section.IndirectSize);
        int count = BucketTree.ReadHeader(section.IndirectHeader, Name);
        var virt = new long[count];
        var phys = new long[count];
        var fromPatch = new bool[count];
        long end = BucketTree.EnumerateEntries(table, EntrySize, count, Name, (i, e) =>
        {
            virt[i] = BinaryPrimitives.ReadInt64LittleEndian(e);
            phys[i] = BinaryPrimitives.ReadInt64LittleEndian(e[8..]);
            fromPatch[i] = BinaryPrimitives.ReadInt32LittleEndian(e[0x10..]) != 0;
        });
        var ranges = new List<(long, long, long)>();
        for (int i = 0; i < count; i++)
        {
            if (!fromPatch[i])
                ranges.Add((virt[i], phys[i], (i + 1 < count ? virt[i + 1] : end) - virt[i]));
        }
        return [.. ranges];
    }

    /// <summary>
    /// Compressed by this tool, which only ever writes key generation 0. Its makers can't have compressed a game with that: compression came
    /// with firmware 12, and games made for it have a far newer key generation. How the section is laid out doesn't say: versions before 1.4
    /// put the hash levels first, as games their makers compressed do, and the ones since put the game data first, as it's written as it's made.
    /// </summary>
    public bool IsRebuiltCompressed(NcaFsSection section) =>
        section.FsType == NcaFsSection.FsRomFs && section.HasCompression && !section.HasSparse && GenerationIndex == 0;

    private readonly Dictionary<int, bool> _readsStored = [];

    /// <summary>Whether a few blocks of an update's section that come from its base check out against its own hashes, read this way.</summary>
    private bool Fits(NcaFsSection section, IndirectSource logical)
    {
        if (section.FsType != NcaFsSection.FsRomFs)
            return true;
        try
        {
            var data = OpenIntegrityData(section, logical, $"{Name} section {section.Index}", "", true);
            long start = section.DataLevelOffset;
            var fromBase = logical.FromOriginal().Where(v => v >= start && v - start < data.Length).ToList();
            var one = new byte[1];
            for (int i = 0; i < 8 && fromBase.Count > 0; i++)
                data.Read(fromBase[fromBase.Count * i / 8] - start, one, 0, 1);
            return true;
        }
        catch (ToolException)
        {
            return false;
        }
    }

    /// <summary>The plain RomFS image of a section, with decompression and normally hash checking applied.</summary>
    /// <param name="verify">False only for estimates: hash blocks are large, so checking them defeats reading a small sample.</param>
    public DataSource OpenRomFsImage(NcaFsSection section, NcaFile? baseNca, bool verify = true)
    {
        if (section.FsType != NcaFsSection.FsRomFs)
            throw new ToolException($"{Name} section {section.Index} is not a RomFS");

        var logical = OpenLogical(section, baseNca);
        var hint = section.HasIndirect ? "，或者该升级补丁不属于此本体游戏" : "";
        var what = $"{Name} section {section.Index}";
        DataSource data = section.HashType switch
        {
            NcaFsSection.HashIntegrity or NcaFsSection.HashIntegritySha3 => OpenIntegrityData(section, logical, what, hint, verify),
            _ => throw new ToolException($"{what} has a RomFS with hash type {section.HashType}, which isn't supported"),
        };
        return section.HasCompression ? OpenCompressed(section, data) : data;
    }

    public Pfs0Archive OpenPfs0(NcaFsSection section, NcaFile? baseNca, bool verify = true) =>
        Pfs0Archive.Open(OpenLogical(section, baseNca), section, $"{Name} section {section.Index}", verify);

    private byte[] ContentKey => _contentKey ??= ResolveContentKey();

    private byte[] ResolveContentKey()
    {
        if (HasRightsId)
            return _tickets.GetTitleKey(Header.AsSpan(0x230, 0x10), GenerationIndex, Name);

        int keyAreaIndex = Header[0x207];
        var kaek = _keyset.TryKeyAreaKey(keyAreaIndex, GenerationIndex)
            ?? throw new ToolException($"The key area key for generation {GenerationIndex:x2} is not in the keyset, it is needed for {Name}. Your keys may be older than this game");
        var keyArea = NcaCrypto.EcbDecrypt(kaek, Header.AsSpan(0x300, 0x40).ToArray());
        return keyArea[(KeyIndexCtr * 0x10)..((KeyIndexCtr + 1) * 0x10)];
    }

    /// <summary>The update tables always use plain AES-CTR with the section's own counter value.</summary>
    private byte[] ReadMetaTable(NcaFsSection section, long offset, long size)
    {
        if (size <= 0 || size > int.MaxValue || !Ranges.Fits(offset, size, section.Size))
            throw new ToolException($"{Name} section {section.Index} has an update table outside the section");

        DataSource raw = new SubSource(_raw, section.Offset, section.Size);
        if (section.Encryption != NcaFsSection.EncryptNone && _ncz is null)
            raw = new AesCtrSource(raw, ContentKey, section.Offset, section.UpperIv);
        var table = new byte[size];
        raw.Read(offset, table, 0, table.Length);
        return table;
    }

    private DataSource OpenIntegrityData(NcaFsSection section, DataSource logical, string what, string hint, bool wantVerify)
    {
        var h = section.Header.AsSpan();
        if (!h.Slice(0x08, 4).SequenceEqual("IVFC"u8))
            throw new ToolException($"{what} has no IVFC header");
        int layers = BinaryPrimitives.ReadInt32LittleEndian(h[0x14..]);
        if (layers < 2 || layers > 7)
            throw new ToolException($"{what} has {layers} IVFC layers");

        // SHA-3 trees exist in the format but SHA-3 isn't available on every OS this runs on: read them unchecked.
        bool verify = wantVerify && section.HashType == NcaFsSection.HashIntegrity;
        if (wantVerify && !verify)
            Ui.Warn($"{what} 使用 SHA-3 哈希树，无法校验其数据");

        int levels = layers - 1;
        var offsets = new long[levels];
        var sizes = new long[levels];
        var orders = new int[levels];
        for (int level = 0; level < levels; level++)
        {
            var info = h[(0x18 + level * 0x18)..];
            offsets[level] = BinaryPrimitives.ReadInt64LittleEndian(info);
            sizes[level] = BinaryPrimitives.ReadInt64LittleEndian(info[8..]);
            orders[level] = BinaryPrimitives.ReadInt32LittleEndian(info[0x10..]);
            // A hash level is read whole, and a real one is at most a few hundred MB, even for the largest game.
            if (!Ranges.Fits(offsets[level], sizes[level], logical.Length) || (level < levels - 1 && sizes[level] > MostHashLevel) || orders[level] < 9 || orders[level] > 24)
                throw new ToolException($"{what} has an invalid IVFC level {level}");
        }

        var data = new SubSource(logical, offsets[levels - 1], sizes[levels - 1]);
        if (!verify)
            return data;

        // Of each hash level, only the blocks the data under it uses are checked, as those are all the console ever reads: a section its
        // developer compressed has hash levels made for its game data uncompressed, which can end in blocks nothing uses, and those don't hold
        // what their hashes say. Monument Valley's did, and it was taken as damaged.
        var used = new long[levels];
        long BlocksOf(long bytes, int level) => (bytes + (1L << orders[level]) - 1) >> orders[level];
        used[levels - 1] = BlocksOf(sizes[levels - 1], levels - 1);
        for (int level = levels - 2; level >= 0; level--)
            used[level] = Math.Min(BlocksOf(sizes[level], level), BlocksOf(used[level + 1] * 0x20, level));

        byte[] hashes = h.Slice(0xC8, 0x20).ToArray();
        for (int level = 0; level < levels - 1; level++)
        {
            var bytes = new byte[sizes[level]];
            logical.Read(offsets[level], bytes, 0, bytes.Length);
            VerifiedSource.CheckBlocks(new BytesSource(bytes), 0, Math.Min(sizes[level], used[level] << orders[level]), hashes, 1 << orders[level], what, hint);
            hashes = bytes;
        }
        return new VerifiedSource(data, hashes, 1 << orders[levels - 1], what, hint);
    }

    private const long MostHashLevel = 1L << 30;

    private DataSource OpenCompressed(NcaFsSection section, DataSource data)
    {
        long offset = section.CompressionOffset, size = section.CompressionSize;
        if (size <= 0 || size > int.MaxValue || !Ranges.Fits(offset, size, data.Length))
            throw new ToolException($"{Name} section {section.Index} has a compression table outside its data");

        var table = new byte[size];
        data.Read(offset, table, 0, table.Length);
        return new CompressedImageSource(new SubSource(data, 0, offset), table, section.CompressionHeader, Name);
    }
}

/// <summary>
/// A RomFS section as an uncompressed game has it, laid out as <see cref="Nca.RomFs"/> writes it and as the original was: the hash
/// levels, then the image and its padding. The hash levels take a pass over the whole image, so they're only worked out if
/// something reads them.
/// </summary>
internal sealed class UncompressedRomFsSource : DataSource
{
    private readonly DataSource _image;
    private readonly long _levelsSize;
    private readonly long _dataSize;
    private readonly object _gate = new();
    private byte[]? _levels;

    public UncompressedRomFsSource(DataSource image)
    {
        _image = image;
        _dataSize = image.Length + IvfcBuilder.PadSize(image.Length);
        _levelsSize = IvfcBuilder.HashLevelsSize(_dataSize);
    }

    public override long Length => _levelsSize + _dataSize;
    public override bool ReadsAtOnce => _image.ReadsAtOnce;

    public override void Read(long offset, byte[] buffer, int bufferOffset, int count)
    {
        while (count > 0)
        {
            int n;
            if (offset < _levelsSize)
            {
                n = (int)Math.Min(count, _levelsSize - offset);
                Buffer.BlockCopy(Levels(), (int)offset, buffer, bufferOffset, n);
            }
            else if (offset - _levelsSize < _image.Length)
            {
                n = (int)Math.Min(count, _image.Length - (offset - _levelsSize));
                _image.Read(offset - _levelsSize, buffer, bufferOffset, n);
            }
            else
            {
                n = count;
                Array.Clear(buffer, bufferOffset, n);
            }
            offset += n;
            bufferOffset += n;
            count -= n;
        }
    }

    public override void Release() => _image.Release();

    private byte[] Levels()
    {
        lock (_gate)
        {
            if (_levels is not null)
                return _levels;
            var ivfc = new IvfcBuilder();
            var buffer = new byte[IvfcBuilder.BlockSize * 512];
            for (long position = 0; position < _dataSize;)
            {
                int n = (int)Math.Min(_dataSize - position, buffer.Length);
                int fromImage = (int)Math.Min(n, Math.Max(_image.Length - position, 0));
                _image.Read(position, buffer, 0, fromImage);
                Array.Clear(buffer, fromImage, n - fromImage);
                ivfc.Add(buffer, 0, n);
                position += n;
            }
            return _levels = ivfc.Finish(_image.Length).HashLevels;
        }
    }
}
