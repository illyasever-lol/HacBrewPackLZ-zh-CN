using System.Buffers.Binary;
using System.Security.Cryptography;

namespace HacBrewPackLz;

internal sealed class CtrDecryptor(DataSource raw, byte[] key, long sectionFileOffset)
{
    // One for each thread that reads, as they can't be shared.
    private readonly ThreadLocal<AesCtrTransform> _ctr = new(() => new AesCtrTransform(key, 0));
    [ThreadStatic] private static byte[]? t_scratch;

    public bool ReadsAtOnce => raw.ReadsAtOnce;

    public void Read(long offset, byte[] buffer, int bufferOffset, int count, ulong upperIv)
    {
        long aligned = offset & ~15L;
        int head = (int)(offset - aligned);
        int total = (head + count + 15) & ~15;
        if (head == 0 && total == count)
        {
            raw.Read(offset, buffer, bufferOffset, count);
            _ctr.Value!.Transform(buffer, bufferOffset, count, sectionFileOffset + offset, upperIv);
            return;
        }

        var scratch = t_scratch;
        if (scratch is null || scratch.Length < total)
            t_scratch = scratch = new byte[Math.Max(total, 0x10000)];
        raw.Read(aligned, scratch, 0, total);
        _ctr.Value!.Transform(scratch, 0, total, sectionFileOffset + aligned, upperIv);
        Buffer.BlockCopy(scratch, head, buffer, bufferOffset, count);
    }
}

internal sealed class AesCtrSource(DataSource raw, byte[] key, long sectionFileOffset, ulong upperIv) : DataSource
{
    private readonly CtrDecryptor _decryptor = new(raw, key, sectionFileOffset);

    public override long Length => raw.Length;
    public override bool ReadsAtOnce => _decryptor.ReadsAtOnce;

    public override void Read(long offset, byte[] buffer, int bufferOffset, int count) =>
        _decryptor.Read(offset, buffer, bufferOffset, count, upperIv);
}

/// <summary>An update's own data: each region has its own counter value, listed in the AES-CTR-Ex table.</summary>
internal sealed class AesCtrExSource : DataSource
{
    private const int EntrySize = 0x10;

    private readonly DataSource _raw;
    private readonly CtrDecryptor _decryptor;
    private readonly uint _secureValue;
    private readonly long[] _offsets;
    private readonly int[] _generations;
    private readonly bool[] _encrypted;
    private readonly long _end;
    private readonly string _name;
    private int _cursor;

    public AesCtrExSource(DataSource raw, byte[] key, long sectionFileOffset, uint secureValue, byte[] table, ReadOnlySpan<byte> treeHeader, string name)
    {
        _raw = raw;
        _decryptor = new CtrDecryptor(raw, key, sectionFileOffset);
        _secureValue = secureValue;
        _name = name;

        int count = BucketTree.ReadHeader(treeHeader, name);
        _offsets = new long[count];
        _generations = new int[count];
        _encrypted = new bool[count];
        var offsets = _offsets;
        var generations = _generations;
        var encrypted = _encrypted;
        _end = BucketTree.EnumerateEntries(table, EntrySize, count, name, (i, e) =>
        {
            offsets[i] = BinaryPrimitives.ReadInt64LittleEndian(e);
            encrypted[i] = e[8] == 0;
            generations[i] = BinaryPrimitives.ReadInt32LittleEndian(e[0xC..]);
        });
    }

    public override long Length => _raw.Length;
    public override bool ReadsAtOnce => _raw.ReadsAtOnce;

    public override void Read(long offset, byte[] buffer, int bufferOffset, int count)
    {
        while (count > 0)
        {
            int entry = Locate(offset);
            long entryEnd = entry + 1 < _offsets.Length ? _offsets[entry + 1] : _end;
            int n = (int)Math.Min(count, entryEnd - offset);
            if (_encrypted[entry])
                _decryptor.Read(offset, buffer, bufferOffset, n, ((ulong)_secureValue << 32) | (uint)_generations[entry]);
            else
                _raw.Read(offset, buffer, bufferOffset, n);
            offset += n;
            bufferOffset += n;
            count -= n;
        }
    }

    private int Locate(long offset)
    {
        int entry = SortedLookup.Find(_offsets, _end, offset, _cursor);
        if (entry < 0)
            throw new ToolException($"{_name}: offset 0x{offset:x} isn't covered by the AES-CTR-Ex table");
        _cursor = entry;
        return entry;
    }
}

/// <summary>An update's view of a section: every range comes either from the base game's section or from the update.</summary>
internal sealed class IndirectSource : DataSource
{
    private const int EntrySize = 0x14;

    private readonly DataSource? _original;
    private readonly DataSource _patch;
    private readonly long[] _virtual;
    private readonly long[] _physical;
    private readonly bool[] _fromPatch;
    private readonly long _end;
    private readonly string _name;
    private int _cursor;

    public IndirectSource(DataSource? original, DataSource patch, byte[] table, ReadOnlySpan<byte> treeHeader, string name)
    {
        _original = original;
        _patch = patch;
        _name = name;

        int count = BucketTree.ReadHeader(treeHeader, name);
        _virtual = new long[count];
        _physical = new long[count];
        _fromPatch = new bool[count];
        var virt = _virtual;
        var phys = _physical;
        var fromPatch = _fromPatch;
        _end = BucketTree.EnumerateEntries(table, EntrySize, count, name, (i, e) =>
        {
            virt[i] = BinaryPrimitives.ReadInt64LittleEndian(e);
            phys[i] = BinaryPrimitives.ReadInt64LittleEndian(e[8..]);
            fromPatch[i] = BinaryPrimitives.ReadInt32LittleEndian(e[0x10..]) != 0;
        });
    }

    public override long Length => _end;
    public override bool ReadsAtOnce => _patch.ReadsAtOnce && _original?.ReadsAtOnce != false;

    public override void Read(long offset, byte[] buffer, int bufferOffset, int count)
    {
        while (count > 0)
        {
            int entry = SortedLookup.Find(_virtual, _end, offset, _cursor);
            if (entry < 0)
                throw new ToolException($"{_name}: offset 0x{offset:x} isn't covered by the update's table");
            _cursor = entry;

            long entryEnd = entry + 1 < _virtual.Length ? _virtual[entry + 1] : _end;
            int n = (int)Math.Min(count, entryEnd - offset);
            long source = _physical[entry] + (offset - _virtual[entry]);
            if (_fromPatch[entry])
                _patch.Read(source, buffer, bufferOffset, n);
            else if (_original is not null)
                _original.Read(source, buffer, bufferOffset, n);
            else
                throw new ToolException($"{_name} needs its base game, which wasn't provided");

            offset += n;
            bufferOffset += n;
            count -= n;
        }
    }
}

/// <summary>
/// A section of a base game that's been cut down to what its update still reads from it. What's left is kept together, encrypted as it was where
/// it used to be, so to anything reading it this looks like the whole section, with nothing where the rest used to be.
/// </summary>
internal sealed class SparseSource : DataSource
{
    private const int EntrySize = 0x14;

    private readonly DataSource _raw;
    private readonly long _dataStart;
    private readonly long _length;
    private readonly long[] _virtual;
    private readonly long[] _physical;
    private readonly bool[] _present;
    private readonly long _end;
    private int _cursor;

    public SparseSource(DataSource raw, long dataStart, long length, byte[] table, ReadOnlySpan<byte> treeHeader, string name)
    {
        _raw = raw;
        _dataStart = dataStart;
        _length = length;

        int count = BucketTree.ReadHeader(treeHeader, name);
        _virtual = new long[count];
        _physical = new long[count];
        _present = new bool[count];
        var virt = _virtual;
        var phys = _physical;
        var present = _present;
        _end = BucketTree.EnumerateEntries(table, EntrySize, count, name, (i, e) =>
        {
            virt[i] = BinaryPrimitives.ReadInt64LittleEndian(e);
            phys[i] = BinaryPrimitives.ReadInt64LittleEndian(e[8..]);
            present[i] = BinaryPrimitives.ReadInt32LittleEndian(e[0x10..]) == 0;
        });
    }

    public override long Length => _length;
    public override bool ReadsAtOnce => _raw.ReadsAtOnce;

    public override void Read(long offset, byte[] buffer, int bufferOffset, int count)
    {
        while (count > 0)
        {
            int entry = SortedLookup.Find(_virtual, _end, offset, _cursor);
            if (entry < 0)
            {
                Array.Clear(buffer, bufferOffset, count);
                return;
            }
            _cursor = entry;

            long entryEnd = entry + 1 < _virtual.Length ? _virtual[entry + 1] : _end;
            int n = (int)Math.Min(count, entryEnd - offset);
            if (_present[entry])
                _raw.Read(_dataStart + _physical[entry] + (offset - _virtual[entry]), buffer, bufferOffset, n);
            else
                Array.Clear(buffer, bufferOffset, n);

            offset += n;
            bufferOffset += n;
            count -= n;
        }
    }
}

internal static class SortedLookup
{
    /// <summary>Finds the last start at or before value. Sequential reads usually hit the hint or the entry after it.</summary>
    public static int Find(long[] starts, long end, long value, int hint)
    {
        if (value < 0 || value >= end || starts.Length == 0 || value < starts[0])
            return -1;
        if (hint < starts.Length && starts[hint] <= value)
        {
            if (hint + 1 == starts.Length || value < starts[hint + 1])
                return hint;
            if (hint + 2 == starts.Length || value < starts[hint + 2])
                return hint + 1;
        }
        int index = Array.BinarySearch(starts, value);
        return index >= 0 ? index : ~index - 1;
    }
}

/// <summary>The last level of a hash tree, checking whole blocks against their hashes as they are read.</summary>
internal sealed class VerifiedSource(DataSource data, byte[] hashes, int blockSize, string what, string hint) : DataSource
{
    private const int WindowSize = 1 << 22;
    private const int HashSize = 0x20;
    private const int ParallelThreshold = 16;

    private readonly byte[] _window = new byte[Math.Max(WindowSize, blockSize)];
    private long _windowStart = -1;
    private int _windowLength;

    public override long Length => data.Length;

    public override void Read(long offset, byte[] buffer, int bufferOffset, int count)
    {
        if (offset < 0 || offset + count > data.Length)
            throw new ArgumentOutOfRangeException(nameof(offset));

        while (count > 0)
        {
            if (_windowStart < 0 || offset < _windowStart || offset >= _windowStart + _windowLength)
                LoadWindow(offset);
            int inWindow = (int)(offset - _windowStart);
            int n = Math.Min(count, _windowLength - inWindow);
            Buffer.BlockCopy(_window, inWindow, buffer, bufferOffset, n);
            offset += n;
            bufferOffset += n;
            count -= n;
        }
    }

    private void LoadWindow(long offset)
    {
        long start = offset / blockSize * blockSize;
        int length = (int)Math.Min(_window.Length / blockSize * blockSize, data.Length - start);
        data.Read(start, _window, 0, length);
        Check(_window, length, start, hashes, blockSize, what, hint);
        _windowStart = start;
        _windowLength = length;
    }

    public static void CheckBlocks(DataSource source, long start, long size, byte[] hashes, int blockSize, string what, string hint)
    {
        var buffer = new byte[Math.Max(WindowSize / blockSize, 1) * blockSize];
        for (long done = 0; done < size;)
        {
            int n = (int)Math.Min(buffer.Length, size - done);
            source.Read(start + done, buffer, 0, n);
            Check(buffer, n, done, hashes, blockSize, what, hint);
            done += n;
        }
    }

    /// <summary>A short last block is hashed with zeros up to the block size, as the firmware does.</summary>
    private static void Check(byte[] buffer, int length, long position, byte[] hashes, int blockSize, string what, string hint)
    {
        int blocks = (length + blockSize - 1) / blockSize;
        long firstBlock = position / blockSize;
        if ((firstBlock + blocks) * HashSize > hashes.Length)
            throw new ToolException($"{what}: the hash table is too short for its data, the file is damaged{hint}");

        if (length % blockSize != 0)
        {
            if (length + (blockSize - length % blockSize) > buffer.Length)
                throw new InvalidOperationException("哈希缓冲区过小");
            Array.Clear(buffer, length, blockSize - length % blockSize);
        }

        long failed = -1;
        void CheckOne(int i)
        {
            Span<byte> hash = stackalloc byte[HashSize];
            SHA256.HashData(buffer.AsSpan(i * blockSize, blockSize), hash);
            if (!hash.SequenceEqual(hashes.AsSpan((int)((firstBlock + i) * HashSize), HashSize)))
                Volatile.Write(ref failed, firstBlock + i);
        }

        if (blocks >= ParallelThreshold)
            Parallel.For(0, blocks, CheckOne);
        else
            for (int i = 0; i < blocks; i++)
                CheckOne(i);

        long bad = Volatile.Read(ref failed);
        if (bad >= 0)
            throw new ToolException($"{what}: hash check failed at offset 0x{bad * blockSize:x}. The file is damaged{hint}");
    }
}

/// <summary>A compressed RomFS seen as its plain image. Accepts everything the firmware's reader does, not only this tool's output.</summary>
internal sealed class CompressedImageSource : DataSource
{
    private const int EntrySize = 0x18;
    private const byte TypeNone = 0;
    private const byte TypeZeros = 1;
    private const byte TypeLz4 = 3;

    private readonly DataSource _data;
    private readonly long[] _virtual;
    private readonly long[] _physical;
    private readonly int[] _physicalSize;
    private readonly byte[] _types;
    private readonly long _end;
    private readonly string _name;
    private byte[] _compressed = new byte[0x10000];
    private byte[] _block = new byte[0x10000];
    private int _blockEntry = -1;
    private int _cursor;

    public CompressedImageSource(DataSource data, byte[] table, ReadOnlySpan<byte> treeHeader, string name)
    {
        _data = data;
        _name = name;
        int count = BucketTree.ReadHeader(treeHeader, name);
        _virtual = new long[count];
        _physical = new long[count];
        _physicalSize = new int[count];
        _types = new byte[count];
        var virt = _virtual;
        var phys = _physical;
        var sizes = _physicalSize;
        var types = _types;
        _end = BucketTree.EnumerateEntries(table, EntrySize, count, name, (i, e) =>
        {
            virt[i] = BinaryPrimitives.ReadInt64LittleEndian(e);
            phys[i] = BinaryPrimitives.ReadInt64LittleEndian(e[8..]);
            types[i] = e[0x10];
            sizes[i] = BinaryPrimitives.ReadInt32LittleEndian(e[0x14..]);
        });
    }

    public override long Length => _end;

    public override void Read(long offset, byte[] buffer, int bufferOffset, int count)
    {
        while (count > 0)
        {
            _cursor = SortedLookup.Find(_virtual, _end, offset, _cursor);
            if (_cursor < 0)
                throw new ToolException($"{_name}: offset 0x{offset:x} isn't covered by the compression table");

            long entryEnd = _cursor + 1 < _virtual.Length ? _virtual[_cursor + 1] : _end;
            long inEntry = offset - _virtual[_cursor];
            int n = (int)Math.Min(count, entryEnd - offset);
            switch (_types[_cursor])
            {
                case TypeNone:
                    _data.Read(_physical[_cursor] + inEntry, buffer, bufferOffset, n);
                    break;
                case TypeZeros:
                    Array.Clear(buffer, bufferOffset, n);
                    break;
                case TypeLz4:
                    LoadBlock(_cursor, checked((int)(entryEnd - _virtual[_cursor])));
                    Buffer.BlockCopy(_block, (int)inEntry, buffer, bufferOffset, n);
                    break;
                default:
                    throw new ToolException($"{_name} uses compression type {_types[_cursor]}, which isn't supported");
            }
            offset += n;
            bufferOffset += n;
            count -= n;
        }
    }

    private void LoadBlock(int entry, int virtualSize)
    {
        if (_blockEntry == entry)
            return;
        int physicalSize = _physicalSize[entry];
        if (physicalSize <= 0)
            throw new ToolException($"{_name}: compressed block at 0x{_virtual[entry]:x} has an invalid size");
        if (_compressed.Length < physicalSize)
            _compressed = new byte[physicalSize];
        if (_block.Length < virtualSize)
            _block = new byte[virtualSize];

        _blockEntry = -1;
        _data.Read(_physical[entry], _compressed, 0, physicalSize);
        if (Lz4Decoder.Decode(_compressed, 0, physicalSize, _block, 0, virtualSize) != virtualSize)
            throw new ToolException($"{_name}: compressed block at 0x{_virtual[entry]:x} failed to decode");
        _blockEntry = entry;
    }
}
