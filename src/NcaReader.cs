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
        if (_ncz is not null)
            throw new ToolException($"{Name} section {section.Index} is only there in part, which can't be read from an NCZ");
        // With nothing listed, none of it is here at all, and the update has all of it.
        if (BinaryPrimitives.ReadInt32LittleEndian(section.SparseTableHeader[8..]) == 0)
            return new ZeroSource(section.Size);

        long start = section.SparseDataOffset + section.SparseTableOffset, size = section.SparseTableSize;
        if (section.SparseDataOffset < 0 || section.SparseTableOffset < 0 || size <= 0 || start + size > _raw.Length || size > int.MaxValue)
            throw new ToolException($"{Name} section {section.Index} has a sparse table outside the file");

        // Its table is encrypted where it now is, with a counter that says which time this section was cut down.
        DataSource raw = new SubSource(_raw, start, size);
        if (section.Encryption != NcaFsSection.EncryptNone)
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

        DataSource? original = baseNca?.Sections[section.Index] is { } baseSection ? baseNca.OpenPhysical(baseSection) : null;
        var table = ReadMetaTable(section, section.IndirectOffset, section.IndirectSize);
        return new IndirectSource(original, physical, table, section.IndirectHeader, Name);
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
        if (offset < 0 || size <= 0 || offset + size > section.Size || size > int.MaxValue)
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
            Ui.Warn($"{what} uses a SHA-3 hash tree, so its data can't be checked");

        byte[] hashes = h.Slice(0xC8, 0x20).ToArray();
        int levels = layers - 1;
        for (int level = 0; level < levels; level++)
        {
            var info = h[(0x18 + level * 0x18)..];
            long offset = BinaryPrimitives.ReadInt64LittleEndian(info);
            long size = BinaryPrimitives.ReadInt64LittleEndian(info[8..]);
            int order = BinaryPrimitives.ReadInt32LittleEndian(info[0x10..]);
            if (offset < 0 || size < 0 || offset + size > logical.Length || order < 9 || order > 24)
                throw new ToolException($"{what} has an invalid IVFC level {level}");

            if (level == levels - 1)
            {
                var data = new SubSource(logical, offset, size);
                return verify ? new VerifiedSource(data, hashes, 1 << order, what, hint) : data;
            }
            if (!verify)
                continue;

            var bytes = new byte[size];
            logical.Read(offset, bytes, 0, bytes.Length);
            VerifiedSource.CheckBlocks(new BytesSource(bytes), 0, size, hashes, 1 << order, what, hint);
            hashes = bytes;
        }
        throw new InvalidOperationException();
    }

    private DataSource OpenCompressed(NcaFsSection section, DataSource data)
    {
        long offset = section.CompressionOffset, size = section.CompressionSize;
        if (offset < 0 || size <= 0 || offset + size > data.Length || size > int.MaxValue)
            throw new ToolException($"{Name} section {section.Index} has a compression table outside its data");

        var table = new byte[size];
        data.Read(offset, table, 0, table.Length);
        return new CompressedImageSource(new SubSource(data, 0, offset), table, section.CompressionHeader, Name);
    }
}
