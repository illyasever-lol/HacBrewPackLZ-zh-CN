using System.Buffers.Binary;
using System.Security.Cryptography;
using System.Text;

namespace HacBrewPackLz;

internal enum NcaContentType : byte
{
    Program = 0,
    Meta = 1,
    Control = 2,
    Manual = 3,
    Data = 4,
    PublicData = 5,
}

internal sealed class NcaSection
{
    public required byte[] FsHeader { get; init; }
    public required DataSource Data { get; init; }
    public required bool Encrypted { get; init; }
    public ulong UpperIv { get; init; }
}

internal sealed record NcaInfo(byte[] Hash, long Size)
{
    public string Id => Convert.ToHexString(Hash, 0, 16).ToLowerInvariant();
}

/// <summary>Section contents and FS headers for the two filesystem layouts an NCA can hold.</summary>
internal static class NcaSections
{
    private const byte EncryptionNone = 1;
    private const byte EncryptionAesCtr = 3;

    public const int ExefsHashBlockSize = 0x10000;
    public const int LogoHashBlockSize = 0x1000;
    public const int MetaHashBlockSize = 0x1000;

    private const int Pfs0Alignment = 0x200;

    public static long Pfs0SectionSize(long pfs0Size, int hashBlockSize)
    {
        long table = (pfs0Size + hashBlockSize - 1) / hashBlockSize * 0x20;
        return Pfs0DataOffset(table) + pfs0Size;
    }

    public static long RomFsSectionSize(long imageSize)
    {
        long data = imageSize + IvfcBuilder.PadSize(imageSize);
        return IvfcBuilder.HashLevelsSize(data) + data;
    }

    /// <param name="storedSize">The compressed blocks and raw runs, before the table.</param>
    public static long CompressedRomFsSectionSize(long storedSize, int tableEntries)
    {
        long aligned = (storedSize + IvfcBuilder.BlockSize - 1) & ~(IvfcBuilder.BlockSize - 1L);
        long data = aligned + BucketTree.NodeStorageSize(tableEntries) + BucketTree.EntryStorageSize(tableEntries);
        return IvfcBuilder.HashLevelsSize(data) + data;
    }

    /// <summary>hacBrewPack always pads the table, by a whole 0x200 when it is already aligned.</summary>
    private static long Pfs0DataOffset(long tableSize) => tableSize + (Pfs0Alignment - tableSize % Pfs0Alignment);

    /// <summary>PFS0 section: a SHA-256 table over the partition, then the partition itself.</summary>
    public static NcaSection Pfs0(DataSource pfs0, int hashBlockSize, bool encrypted)
    {
        long blocks = (pfs0.Length + hashBlockSize - 1) / hashBlockSize;
        var table = new byte[blocks * 0x20];
        var buffer = new byte[hashBlockSize];
        for (long i = 0; i < blocks; i++)
        {
            int n = (int)Math.Min(hashBlockSize, pfs0.Length - i * hashBlockSize);
            pfs0.Read(i * hashBlockSize, buffer, 0, n);
            SHA256.HashData(buffer.AsSpan(0, n), table.AsSpan((int)(i * 0x20)));
        }
        pfs0.Release();

        long pfs0Offset = Pfs0DataOffset(table.Length);

        var header = NewFsHeader(fsType: 1, hashType: 2, encrypted);
        var h = header.AsSpan();
        SHA256.HashData(table, h[0x08..]);
        BinaryPrimitives.WriteInt32LittleEndian(h[0x28..], hashBlockSize);
        BinaryPrimitives.WriteInt32LittleEndian(h[0x2C..], 2);
        BinaryPrimitives.WriteInt64LittleEndian(h[0x38..], table.Length);
        BinaryPrimitives.WriteInt64LittleEndian(h[0x40..], pfs0Offset);
        BinaryPrimitives.WriteInt64LittleEndian(h[0x48..], pfs0.Length);

        var data = new ConcatSource()
            .Add(new BytesSource(table))
            .Add(new ZeroSource(pfs0Offset - table.Length))
            .Add(pfs0);
        return new NcaSection { FsHeader = header, Data = data, Encrypted = encrypted };
    }

    /// <summary>Plain RomFS section, the layout hacBrewPack writes.</summary>
    public static NcaSection RomFs(DataSource image, bool encrypted, ProgressReporter? progress)
    {
        var ivfc = new IvfcBuilder();
        var buffer = new byte[IvfcBuilder.BlockSize * 512];
        long padding = IvfcBuilder.PadSize(image.Length);
        long total = image.Length + padding;
        for (long position = 0; position < total;)
        {
            int n = (int)Math.Min(total - position, buffer.Length);
            int fromImage = (int)Math.Min(n, Math.Max(image.Length - position, 0));
            image.Read(position, buffer, 0, fromImage);
            Array.Clear(buffer, fromImage, n - fromImage);
            ivfc.Add(buffer, 0, n);
            position += n;
            progress?.Report(Math.Min(position, image.Length), position);
        }
        image.Release();

        var result = ivfc.Finish(image.Length);
        var data = new ConcatSource()
            .Add(new BytesSource(result.HashLevels))
            .Add(image)
            .Add(new ZeroSource(padding));
        return new NcaSection { FsHeader = IntegrityHeader(result, encrypted, null), Data = data, Encrypted = encrypted };
    }

    /// <summary>
    /// RomFS behind the compression layer. Layer order on read is AES-CTR, then IVFC, then decompression, so the
    /// hash tree covers the compressed bytes and the BKTR table, which sit together in the IVFC data level.
    /// </summary>
    public static byte[] CompressedRomFsHeader(CompressedData compressed, bool encrypted) => IntegrityHeader(compressed.Ivfc, encrypted, compressed);

    private static byte[] IntegrityHeader(IvfcResult ivfc, bool encrypted, CompressedData? compressed)
    {
        var header = NewFsHeader(fsType: 0, hashType: 3, encrypted);
        var h = header.AsSpan();
        "IVFC"u8.CopyTo(h[0x08..]);
        BinaryPrimitives.WriteUInt32LittleEndian(h[0x0C..], 0x20000);
        BinaryPrimitives.WriteUInt32LittleEndian(h[0x10..], 0x20);
        BinaryPrimitives.WriteUInt32LittleEndian(h[0x14..], 7);
        for (int i = 0; i < 6; i++)
        {
            var level = h[(0x18 + i * 0x18)..];
            BinaryPrimitives.WriteInt64LittleEndian(level, ivfc.LevelOffsets[i]);
            BinaryPrimitives.WriteInt64LittleEndian(level[8..], ivfc.LevelSizes[i]);
            BinaryPrimitives.WriteInt32LittleEndian(level[0x10..], IvfcBuilder.BlockOrder);
        }
        ivfc.MasterHash.CopyTo(h[0xC8..]);

        if (compressed is not null)
        {
            var info = h[0x178..];
            BinaryPrimitives.WriteInt64LittleEndian(info, compressed.TableOffset);
            BinaryPrimitives.WriteInt64LittleEndian(info[8..], compressed.TableSize);
            BucketTree.WriteHeader(info[0x10..], compressed.EntryCount);
        }
        return header;
    }

    private static byte[] NewFsHeader(byte fsType, byte hashType, bool encrypted)
    {
        var header = new byte[0x200];
        header[0] = 2;
        header[2] = fsType;
        header[3] = hashType;
        header[4] = encrypted ? EncryptionAesCtr : EncryptionNone;
        return header;
    }
}

internal static class Pfs0Builder
{
    public static ConcatSource Build(IEnumerable<(string name, DataSource data)> files)
    {
        var list = files.ToList();
        using var strings = new MemoryStream();
        var offsets = new int[list.Count];
        for (int i = 0; i < list.Count; i++)
        {
            offsets[i] = (int)strings.Position;
            strings.Write(Encoding.UTF8.GetBytes(list[i].name));
            strings.WriteByte(0);
        }
        int tableSize = (int)((strings.Length + 0x1F) & ~0x1FL);

        var header = new byte[0x10 + list.Count * 0x18 + tableSize];
        var h = header.AsSpan();
        "PFS0"u8.CopyTo(h);
        BinaryPrimitives.WriteInt32LittleEndian(h[4..], list.Count);
        BinaryPrimitives.WriteInt32LittleEndian(h[8..], tableSize);
        long offset = 0;
        for (int i = 0; i < list.Count; i++)
        {
            var e = h[(0x10 + i * 0x18)..];
            BinaryPrimitives.WriteInt64LittleEndian(e, offset);
            BinaryPrimitives.WriteInt64LittleEndian(e[8..], list[i].data.Length);
            BinaryPrimitives.WriteInt32LittleEndian(e[0x10..], offsets[i]);
            offset += list[i].data.Length;
        }
        strings.ToArray().CopyTo(h[(0x10 + list.Count * 0x18)..]);

        var result = new ConcatSource().Add(new BytesSource(header));
        foreach (var (_, data) in list)
            result.Add(data);
        return result;
    }
}

/// <summary>Where a section ended up, and the FS header that describes it.</summary>
internal sealed record NcaPlace(long Start, long End, byte[] FsHeader);

internal sealed class NcaWriter(byte[] headerKey, byte[] keyAreaEncryptionKey, byte[] sectionKey, int keyGeneration, uint sdkVersion)
{
    public const int HeaderRegionSize = 0xC00;

    private const int SectorSize = 0x200;
    private const int ChunkSize = 1 << 20;

    public byte[] SectionKey => sectionKey;

    public static long AlignToSector(long size) => Align(size, SectorSize);

    public byte[] Header(NcaContentType contentType, ulong titleId, NcaPlace?[] places, long total, RSA? signer, int contentIndex, uint? sdkOverride)
    {
        var header = new byte[HeaderRegionSize];
        var h = header.AsSpan();
        "NCA3"u8.CopyTo(h[0x200..]);
        h[0x205] = (byte)contentType;
        int generation = Math.Clamp(keyGeneration, 0, 0xFF);
        h[0x206] = (byte)Math.Min(generation, 2);
        h[0x220] = (byte)generation;
        BinaryPrimitives.WriteUInt64LittleEndian(h[0x210..], titleId);
        BinaryPrimitives.WriteInt32LittleEndian(h[0x218..], contentIndex);
        BinaryPrimitives.WriteUInt32LittleEndian(h[0x21C..], sdkOverride ?? sdkVersion);

        for (int i = 0; i < places.Length; i++)
        {
            if (places[i] is not { } place)
                continue;
            var entry = h[(0x240 + i * 0x10)..];
            BinaryPrimitives.WriteUInt32LittleEndian(entry, checked((uint)(place.Start / SectorSize)));
            BinaryPrimitives.WriteUInt32LittleEndian(entry[4..], checked((uint)(place.End / SectorSize)));
            BinaryPrimitives.WriteUInt32LittleEndian(entry[8..], 1);
            SHA256.HashData(place.FsHeader, h[(0x280 + i * 0x20)..]);
            place.FsHeader.CopyTo(h[(0x400 + i * SectorSize)..]);
        }
        BinaryPrimitives.WriteInt64LittleEndian(h[0x208..], total);

        var keyArea = new byte[0x40];
        sectionKey.CopyTo(keyArea, 0x20);
        NcaCrypto.EcbEncrypt(keyAreaEncryptionKey, keyArea).CopyTo(h[0x300..]);

        if (signer is not null)
        {
            var signature = signer.SignData(header, 0x200, 0x200, HashAlgorithmName.SHA256, RSASignaturePadding.Pss);
            signature.CopyTo(h[0x100..]);
        }

        NcaCrypto.XtsTransform(headerKey, header, HeaderRegionSize, encrypt: true);
        return header;
    }

    /// <summary>
    /// Encrypts and streams one NCA to <paramref name="输出"/> and returns its SHA-256 and size. Everything has to be
    /// known by now, as the header goes first.
    /// </summary>
    public NcaInfo Write(Stream output, NcaContentType contentType, ulong titleId, NcaSection?[] sections, RSA? signer, ProgressReporter? progress,
        int contentIndex = 0, uint? sdkOverride = null)
    {
        long position = HeaderRegionSize;
        var sizes = new long[sections.Length];
        var places = new NcaPlace?[sections.Length];
        for (int i = 0; i < sections.Length; i++)
        {
            if (sections[i] is not { } section)
                continue;
            sizes[i] = Align(section.Data.Length, SectorSize);
            places[i] = new NcaPlace(position, position + sizes[i], section.FsHeader);
            position += sizes[i];
        }
        var header = Header(contentType, titleId, places, position, signer, contentIndex, sdkOverride);

        using var sha = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
        output.Write(header);
        sha.AppendData(header);

        var buffer = new byte[ChunkSize];
        long fileOffset = HeaderRegionSize;
        long written = 0;
        for (int i = 0; i < sections.Length; i++)
        {
            if (sections[i] is not { } section)
                continue;
            using var ctr = section.Encrypted ? new AesCtrTransform(sectionKey, section.UpperIv) : null;
            for (long done = 0; done < sizes[i];)
            {
                int n = (int)Math.Min(ChunkSize, sizes[i] - done);
                int fromData = (int)Math.Min(n, Math.Max(section.Data.Length - done, 0));
                section.Data.Read(done, buffer, 0, fromData);
                Array.Clear(buffer, fromData, n - fromData);
                ctr?.Transform(buffer, 0, n, fileOffset);
                sha.AppendData(buffer, 0, n);
                output.Write(buffer, 0, n);
                done += n;
                fileOffset += n;
                written += n;
                progress?.Report(written, written);
            }
            section.Data.Release();
        }

        return new NcaInfo(sha.GetHashAndReset(), position);
    }

    private static long Align(long value, long alignment) => (value + alignment - 1) & ~(alignment - 1);
}

/// <summary>
/// Writes an NCA with a section compressed on its way there, so the compressed data never has to be anywhere but where
/// it's going. As the header says how it all came out, it goes in last, and the hash of the whole NCA can only be worked
/// out after that, so it all gets read back, and checked against what was written while it's at it.
/// </summary>
internal sealed class NcaBuilder : IByteSink
{
    private const int ChunkSize = 4 << 20;
    private const int PieceSize = 1 << 20;

    private readonly NcaWriter _writer;
    private readonly FileStream _output;
    private readonly byte[] _chunk = new byte[ChunkSize];
    private readonly NcaPlace?[] _places = new NcaPlace?[4];
    private readonly List<(int size, byte[] digest)> _written = [];
    private AesCtrTransform? _ctr;
    private int _chunkUsed;
    private long _position = NcaWriter.HeaderRegionSize;
    private long _sectionStart;

    public long Start { get; }

    public NcaBuilder(NcaWriter writer, FileStream output)
    {
        _writer = writer;
        _output = output;
        Start = output.Position;
        output.Write(new byte[NcaWriter.HeaderRegionSize]);
    }

    public void Add(int index, NcaSection section)
    {
        StartSection(section.Encrypted, section.UpperIv);
        var buffer = new byte[PieceSize];
        for (long done = 0; done < section.Data.Length;)
        {
            int n = (int)Math.Min(PieceSize, section.Data.Length - done);
            section.Data.Read(done, buffer, 0, n);
            Write(buffer, 0, n);
            done += n;
        }
        section.Data.Release();
        EndSection(index, section.FsHeader);
    }

    public CompressedData AddCompressed(int index, DataSource image, int blockSize, int level, int threads, bool encrypted, ProgressReporter progress)
    {
        StartSection(encrypted, 0);
        var data = RomFsCompressor.Run(image, this, blockSize, level, threads, progress);
        Write(data.Ivfc.HashLevels, 0, data.Ivfc.HashLevels.Length);
        EndSection(index, NcaSections.CompressedRomFsHeader(data, encrypted));
        return data;
    }

    public void Write(byte[] buffer, int offset, int count)
    {
        while (count > 0)
        {
            int n = Math.Min(count, ChunkSize - _chunkUsed);
            Buffer.BlockCopy(buffer, offset, _chunk, _chunkUsed, n);
            _chunkUsed += n;
            offset += n;
            count -= n;
            if (_chunkUsed == ChunkSize)
                Flush();
        }
    }

    public NcaInfo Finish(NcaContentType contentType, ulong titleId, RSA? signer, int contentIndex, uint? sdkOverride)
    {
        var header = _writer.Header(contentType, titleId, _places, _position, signer, contentIndex, sdkOverride);
        _output.Flush();
        _output.Position = Start;
        _output.Write(header);
        _output.Flush();

        var progress = new ProgressReporter("正在校验写入内容", _position);
        using var sha = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
        _output.Position = Start;
        var back = new byte[ChunkSize];
        _output.ReadExactly(back, 0, header.Length);
        if (!back.AsSpan(0, header.Length).SequenceEqual(header))
            throw new ToolException("写回读取的内容与写入的不一致，磁盘可能有问题");
        sha.AppendData(header);

        long position = header.Length;
        foreach (var (size, digest) in _written)
        {
            _output.ReadExactly(back, 0, size);
            if (!SHA256.HashData(back.AsSpan(0, size)).AsSpan().SequenceEqual(digest))
                throw new ToolException("写回读取的内容与写入的不一致，磁盘可能有问题");
            sha.AppendData(back, 0, size);
            position += size;
            progress.Report(position, position);
        }
        progress.Done();
        _output.Position = Start + _position;
        _ctr?.Dispose();
        return new NcaInfo(sha.GetHashAndReset(), _position);
    }

    private void StartSection(bool encrypted, ulong upperIv)
    {
        _sectionStart = _position;
        _ctr?.Dispose();
        _ctr = encrypted ? new AesCtrTransform(_writer.SectionKey, upperIv) : null;
    }

    private void EndSection(int index, byte[] fsHeader)
    {
        long length = _position + _chunkUsed - _sectionStart;
        int padding = (int)(NcaWriter.AlignToSector(length) - length);
        if (padding > 0)
            Write(new byte[padding], 0, padding);
        Flush();
        _places[index] = new NcaPlace(_sectionStart, _position, fsHeader);
    }

    private void Flush()
    {
        if (_chunkUsed == 0)
            return;
        _ctr?.Transform(_chunk, 0, _chunkUsed, _position);
        _written.Add((_chunkUsed, SHA256.HashData(_chunk.AsSpan(0, _chunkUsed))));
        _output.Write(_chunk, 0, _chunkUsed);
        _position += _chunkUsed;
        _chunkUsed = 0;
    }
}
