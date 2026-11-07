using System.Security.Cryptography;

namespace HacBrewPackLz;

internal sealed record IvfcResult(byte[] HashLevels, long[] LevelOffsets, long[] LevelSizes, byte[] MasterHash);

/// <summary>
/// Builds the six level IVFC tree of a RomFS style section. Data is fed in stream order, and the hash levels are kept in
/// memory (about 1/512 of the data) until the data has gone by.
/// </summary>
internal sealed class IvfcBuilder
{
    public const int BlockSize = 0x4000;
    public const int BlockOrder = 14;

    private const int HashSize = 0x20;
    private const int HashLevelCount = 5;
    private const int ParallelThreshold = 64;

    private readonly byte[] _partial = new byte[BlockSize];
    private int _partialLength;
    private byte[] _hashes = new byte[0x10000];
    private long _hashBytes;

    public void Add(byte[] buffer, int offset, int count)
    {
        if (_partialLength > 0)
        {
            int n = Math.Min(count, BlockSize - _partialLength);
            Buffer.BlockCopy(buffer, offset, _partial, _partialLength, n);
            _partialLength += n;
            offset += n;
            count -= n;
            if (_partialLength < BlockSize)
                return;
            HashBlocks(_partial, 0, 1);
            _partialLength = 0;
        }

        int blocks = count / BlockSize;
        if (blocks > 0)
        {
            HashBlocks(buffer, offset, blocks);
            offset += blocks * BlockSize;
            count -= blocks * BlockSize;
        }

        if (count > 0)
        {
            Buffer.BlockCopy(buffer, offset, _partial, 0, count);
            _partialLength = count;
        }
    }

    /// <param name="dataSize">Size recorded for the data level, which may exclude trailing padding that was hashed.</param>
    /// <param name="dataFirst">The data comes before the levels, which is how something of a size that isn't known in advance can be written as it's made.</param>
    public IvfcResult Finish(long dataSize, bool dataFirst = false)
    {
        if (_partialLength != 0)
            throw new InvalidOperationException("IVFC 数据必须按整块输入");

        var levels = new byte[HashLevelCount][];
        levels[HashLevelCount - 1] = PadLevel(_hashes.AsSpan(0, checked((int)_hashBytes)));
        for (int i = HashLevelCount - 2; i >= 0; i--)
            levels[i] = PadLevel(HashAll(levels[i + 1]));

        var offsets = new long[HashLevelCount + 1];
        var sizes = new long[HashLevelCount + 1];
        long start = dataFirst ? dataSize : 0;
        long position = 0;
        for (int i = 0; i < HashLevelCount; i++)
        {
            offsets[i] = start + position;
            sizes[i] = levels[i].Length;
            position += levels[i].Length;
        }
        offsets[HashLevelCount] = dataFirst ? 0 : position;
        sizes[HashLevelCount] = dataSize;

        var joined = new byte[position];
        for (int i = 0; i < HashLevelCount; i++)
            levels[i].CopyTo(joined, offsets[i] - start);

        return new IvfcResult(joined, offsets, sizes, SHA256.HashData(levels[0]));
    }

    /// <summary>hacBrewPack always appends padding, even a whole block when the size is already aligned.</summary>
    public static long PadSize(long size) => BlockSize - size % BlockSize;

    /// <summary>The total size of the hash levels over a data level of this many bytes, as <see cref="完成"/> lays them out.</summary>
    public static long HashLevelsSize(long dataSize)
    {
        long total = 0, level = dataSize;
        for (int i = 0; i < HashLevelCount; i++)
        {
            long hashes = (level + BlockSize - 1) / BlockSize * HashSize;
            level = hashes + PadSize(hashes);
            total += level;
        }
        return total;
    }

    private static byte[] PadLevel(ReadOnlySpan<byte> hashes)
    {
        var level = new byte[hashes.Length + PadSize(hashes.Length)];
        hashes.CopyTo(level);
        return level;
    }

    private static byte[] HashAll(byte[] level)
    {
        int blocks = level.Length / BlockSize;
        var hashes = new byte[blocks * HashSize];
        for (int i = 0; i < blocks; i++)
            SHA256.HashData(level.AsSpan(i * BlockSize, BlockSize), hashes.AsSpan(i * HashSize));
        return hashes;
    }

    private void HashBlocks(byte[] buffer, int offset, int blocks)
    {
        long needed = _hashBytes + blocks * (long)HashSize;
        if (needed > _hashes.Length)
            Array.Resize(ref _hashes, checked((int)Math.Min(Math.Max(needed, _hashes.Length * 2L), Array.MaxLength)));

        var output = _hashes;
        int start = checked((int)_hashBytes);
        if (blocks >= ParallelThreshold)
        {
            Parallel.For(0, blocks, i =>
                SHA256.HashData(buffer.AsSpan(offset + i * BlockSize, BlockSize), output.AsSpan(start + i * HashSize)));
        }
        else
        {
            for (int i = 0; i < blocks; i++)
                SHA256.HashData(buffer.AsSpan(offset + i * BlockSize, BlockSize), output.AsSpan(start + i * HashSize));
        }
        _hashBytes = needed;
    }
}
