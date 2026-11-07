using System.Buffers.Binary;
using Microsoft.Win32.SafeHandles;

namespace HacBrewPackLz;

/// <summary>Reading on would mean decompressing a solid NCZ from its start again, which isn't allowed just now.</summary>
internal sealed class NczRewindException() : Exception("固态 NCZ 必须从头重新读取");

/// <summary>
/// An NCA stored as an NCZ, the way NSZ and XCZ files hold them, seen as the NCA it came from: the header as it was,
/// followed by its sections, which here are already decrypted. There are two kinds. Block mode compresses each block on
/// its own, so any part can be read at any time. Solid mode is one stream that can only be decoded from its start, so
/// this works hard not to go back: small sections are kept in memory as they go past, the latest data can be read again,
/// and if something keeps needing earlier data anyway, everything decoded from then on goes to a temporary file.
/// </summary>
internal sealed class NczSource : DataSource
{
    private const int HeaderSize = 0x4000;
    private const int SectionEntrySize = 0x40;
    private const int MaxSections = 0x100000;
    private const int MinBlockExponent = 14;
    private const int MaxBlockExponent = 30;
    private const byte BlockTypeZstd = 1;
    private const long MaxKeptSection = 256L << 20;
    private const int RewindsBeforeSpooling = 2;

    private sealed class KeptRange(long start, long size)
    {
        public long Start { get; } = start;
        public long Size { get; } = size;
        public byte[]? Data { get; set; }
        public long Filled { get; set; }
    }

    private readonly DataSource _file;
    private readonly string _name;
    private readonly byte[] _header;
    private readonly int _headerEnd;
    private readonly long[] _ncaStarts;
    private readonly long[] _sizes;
    private readonly long[] _streamStarts;
    private readonly long _streamSize;
    private readonly long _dataStart;

    private readonly long[]? _blockOffsets;
    private readonly int[]? _blockSizes;
    private readonly int _blockExponent;
    private readonly ZstdDecoder? _blockDecoder;
    private byte[]? _block;
    private byte[]? _compressedBlock;
    private int _blockIndex = -1;

    private readonly List<KeptRange> _kept = [];
    private ZstdStream? _stream;
    private SafeFileHandle? _spool;
    private long _spooled;
    private int _rewinds;
    private bool _warned;
    private int _sweepSeen;

    private static int s_lastSweep;
    [ThreadStatic] private static int t_sweep;

    /// <summary>Where a solid stream that can't be read in order is kept while it's in use. Set this before a job.</summary>
    public static string? SpoolFolder { get; set; }

    public override long Length { get; }

    public bool IsSolid => _blockOffsets is null;

    /// <summary>
    /// From now on this thread reads each solid stream in one pass: it can start over before its first read if it has to,
    /// and after that, a read of anything earlier gets a <see cref="NczRewindException"/>. This is for estimates, which
    /// must not end up decoding a whole game several times over.
    /// </summary>
    public static void BeginSinglePass() => t_sweep = Interlocked.Increment(ref s_lastSweep);

    public static void EndSinglePass() => t_sweep = 0;

    public NczSource(DataSource file, string name)
    {
        _file = file;
        _name = name;
        if (file.Length < HeaderSize + 16)
            throw Damaged();

        _header = new byte[HeaderSize];
        file.Read(0, _header, 0, HeaderSize);

        var head = new byte[16];
        file.Read(HeaderSize, head, 0, head.Length);
        if (!head.AsSpan(0, 8).SequenceEqual("NCZSECTN"u8))
            throw Damaged();
        ulong count = BinaryPrimitives.ReadUInt64LittleEndian(head.AsSpan(8));
        if (count == 0 || count > MaxSections || HeaderSize + 16 + (long)count * SectionEntrySize > file.Length)
            throw Damaged();

        var table = new byte[(int)count * SectionEntrySize];
        file.Read(HeaderSize + 16, table, 0, table.Length);
        long position = HeaderSize + 16 + table.Length;

        // The stream holds the sections one after another, starting with any gap before the first. Normally the header
        // is everything before the first section. An NCA laid out like hacBrewPack's has a section start inside the part
        // that's kept as it is, and then the decrypted copy in the stream is what counts.
        var starts = new List<long>((int)count + 1);
        var sizes = new List<long>((int)count + 1);
        for (int i = 0; i < (int)count; i++)
        {
            var e = table.AsSpan(i * SectionEntrySize);
            long start = BinaryPrimitives.ReadInt64LittleEndian(e);
            long size = BinaryPrimitives.ReadInt64LittleEndian(e[8..]);
            if (start < NcaWriter.HeaderRegionSize || size < 0 || start > long.MaxValue - size)
                throw Damaged();
            if (i == 0 && start > HeaderSize)
            {
                starts.Add(HeaderSize);
                sizes.Add(start - HeaderSize);
            }
            starts.Add(start);
            sizes.Add(size);
        }

        _headerEnd = (int)Math.Min(starts[0], HeaderSize);
        _ncaStarts = starts.ToArray();
        _sizes = sizes.ToArray();
        _streamStarts = new long[_ncaStarts.Length];
        long streamSize = 0;
        long end = HeaderSize;
        for (int i = 0; i < _ncaStarts.Length; i++)
        {
            if (i > 0 && _ncaStarts[i] < _ncaStarts[i - 1] + _sizes[i - 1])
                throw Damaged();
            _streamStarts[i] = streamSize;
            if (_sizes[i] > long.MaxValue - streamSize)
                throw Damaged();
            streamSize += _sizes[i];
            end = _ncaStarts[i] + _sizes[i];
        }
        _streamSize = streamSize;
        Length = end;

        var blockMagic = new byte[8];
        if (position + 8 <= file.Length)
            file.Read(position, blockMagic, 0, 8);
        if (blockMagic.AsSpan().SequenceEqual("NCZBLOCK"u8))
        {
            var blockHead = new byte[8 + 16];
            if (position + blockHead.Length > file.Length)
                throw Damaged();
            file.Read(position, blockHead, 0, blockHead.Length);
            if (blockHead[8 + 1] != BlockTypeZstd)
                throw new ToolException($"{_name} uses compression type {blockHead[8 + 1]}, which isn't supported");
            _blockExponent = blockHead[8 + 3];
            int blocks = BinaryPrimitives.ReadInt32LittleEndian(blockHead.AsSpan(8 + 4));
            long decompressed = BinaryPrimitives.ReadInt64LittleEndian(blockHead.AsSpan(8 + 8));
            position += blockHead.Length;

            // The blocks may hold a little more than the sections do, if the NCA had anything after its last section.
            long expected = _blockExponent is >= MinBlockExponent and <= MaxBlockExponent && decompressed >= streamSize
                ? (decompressed + (1L << _blockExponent) - 1) >> _blockExponent
                : -1;
            if (blocks != expected || position + blocks * 4L > file.Length)
                throw Damaged();
            _streamSize = decompressed;

            var list = new byte[blocks * 4];
            file.Read(position, list, 0, list.Length);
            position += list.Length;

            _blockOffsets = new long[blocks];
            _blockSizes = new int[blocks];
            long offset = position;
            for (int i = 0; i < blocks; i++)
            {
                int size = BinaryPrimitives.ReadInt32LittleEndian(list.AsSpan(i * 4));
                if (size <= 0 || size > 1L << _blockExponent || offset + size > file.Length)
                    throw Damaged();
                _blockOffsets[i] = offset;
                _blockSizes[i] = size;
                offset += size;
            }
            _blockDecoder = new ZstdDecoder();
        }
        _dataStart = position;
    }

    private ToolException Damaged() => new($"{_name} is damaged, or isn't an NCZ");

    /// <summary>
    /// Says a section is small enough to hold on to. A solid stream keeps its bytes as they go past, so that the section
    /// can be read again, in any order, without starting the stream over.
    /// </summary>
    public void KeepIfSmall(long ncaOffset, long size)
    {
        if (!IsSolid || size <= 0 || size > MaxKeptSection)
            return;
        int segment = FindSegment(ncaOffset);
        if (segment < 0 || _ncaStarts[segment] > ncaOffset)
            return;
        long start = _streamStarts[segment] + (ncaOffset - _ncaStarts[segment]);

        // The whole section has to sit in the stream in one piece.
        int last = FindSegment(ncaOffset + size - 1);
        if (last < 0 || _streamStarts[last] + (ncaOffset + size - 1 - _ncaStarts[last]) != start + size - 1)
            return;
        _kept.Add(new KeptRange(start, size));
    }

    /// <summary>Lets go of the memory and any temporary file. Reading again later simply starts over.</summary>
    public void ReleaseMemory()
    {
        _stream = null;
        _spool?.Dispose();
        _spool = null;
        _spooled = 0;
        _rewinds = 0;
        _block = null;
        _compressedBlock = null;
        _blockIndex = -1;
        foreach (var range in _kept)
        {
            range.Data = null;
            range.Filled = 0;
        }
    }

    public override void Read(long offset, byte[] buffer, int bufferOffset, int count)
    {
        if (offset < 0 || offset + count > Length)
            throw new ArgumentOutOfRangeException(nameof(offset));

        while (count > 0)
        {
            int n;
            if (offset < _headerEnd)
            {
                n = (int)Math.Min(count, _headerEnd - offset);
                Buffer.BlockCopy(_header, (int)offset, buffer, bufferOffset, n);
            }
            else
            {
                int segment = FindSegment(offset);
                long inSegment = offset - _ncaStarts[segment];
                if (inSegment >= _sizes[segment])
                {
                    // Nothing is stored for a gap between sections, and nothing belongs in one either.
                    long gapEnd = segment + 1 < _ncaStarts.Length ? _ncaStarts[segment + 1] : Length;
                    n = (int)Math.Min(count, gapEnd - offset);
                    Array.Clear(buffer, bufferOffset, n);
                }
                else
                {
                    n = (int)Math.Min(count, _sizes[segment] - inSegment);
                    long position = _streamStarts[segment] + inSegment;
                    if (IsSolid)
                        ReadSolid(position, buffer, bufferOffset, n);
                    else
                        ReadBlocks(position, buffer, bufferOffset, n);
                }
            }
            offset += n;
            bufferOffset += n;
            count -= n;
        }
    }

    /// <summary>The last segment starting at or before an NCA offset.</summary>
    private int FindSegment(long offset)
    {
        int found = Array.BinarySearch(_ncaStarts, offset);
        return found >= 0 ? found : Math.Max(~found - 1, 0);
    }

    private void ReadBlocks(long position, byte[] buffer, int offset, int count)
    {
        while (count > 0)
        {
            int index = (int)(position >> _blockExponent);
            LoadBlock(index);
            int inBlock = (int)(position - ((long)index << _blockExponent));
            int n = Math.Min(count, BlockLength(index) - inBlock);
            Buffer.BlockCopy(_block!, inBlock, buffer, offset, n);
            position += n;
            offset += n;
            count -= n;
        }
    }

    private int BlockLength(int index) => (int)Math.Min(1L << _blockExponent, _streamSize - ((long)index << _blockExponent));

    private void LoadBlock(int index)
    {
        if (_blockIndex == index)
            return;
        Ui.Token.ThrowIfCancellationRequested();

        _block ??= GC.AllocateUninitializedArray<byte>(1 << _blockExponent);
        _blockIndex = -1;
        int length = BlockLength(index);
        int size = _blockSizes![index];

        // A block that didn't shrink is stored as it is.
        if (size >= length)
        {
            if (size != length)
                throw Damaged();
            _file.Read(_blockOffsets![index], _block, 0, length);
        }
        else
        {
            _compressedBlock ??= GC.AllocateUninitializedArray<byte>(1 << _blockExponent);
            _file.Read(_blockOffsets![index], _compressedBlock, 0, size);
            if (_blockDecoder!.Decompress(_compressedBlock, 0, size, _block, 0, length) != length)
                throw Damaged();
        }
        _blockIndex = index;
    }

    private void ReadSolid(long position, byte[] buffer, int offset, int count)
    {
        bool mayRewind = t_sweep == 0 || _sweepSeen != t_sweep;
        while (count > 0)
        {
            int n = ReadKept(position, buffer, offset, count);
            if (n == 0 && position < _spooled)
            {
                n = RandomAccess.Read(_spool!, buffer.AsSpan(offset, (int)Math.Min(count, _spooled - position)), position);
                if (n <= 0)
                    throw Damaged();
            }
            if (n == 0)
            {
                var stream = _stream ??= NewStream();
                if (position < stream.Oldest)
                {
                    if (!mayRewind)
                        throw new NczRewindException();
                    Rewind(stream);
                }
                while (position >= stream.Position)
                {
                    if (!stream.Advance())
                        throw Damaged();
                }
                n = stream.Copy(position, buffer, offset, count);
            }
            position += n;
            offset += n;
            count -= n;
        }
        _sweepSeen = t_sweep;
    }

    private int ReadKept(long position, byte[] buffer, int offset, int count)
    {
        foreach (var range in _kept)
        {
            long inRange = position - range.Start;
            if (inRange >= 0 && inRange < range.Filled)
            {
                int n = (int)Math.Min(count, range.Filled - inRange);
                Buffer.BlockCopy(range.Data!, (int)inRange, buffer, offset, n);
                return n;
            }
        }
        return 0;
    }

    private ZstdStream NewStream() => new(_file, _dataStart, _file.Length - _dataStart, _streamSize) { Decoded = OnDecoded };

    private void Rewind(ZstdStream stream)
    {
        if (t_sweep == 0 && ++_rewinds > RewindsBeforeSpooling && _spool is null)
        {
            string folder = SpoolFolder ?? Path.GetTempPath();
            Directory.CreateDirectory(folder);
            string path = Path.Combine(folder, $".hacbrewpacklz_{Environment.ProcessId}_{Guid.NewGuid():N}.tmp");
            _spool = File.OpenHandle(path, FileMode.Create, FileAccess.ReadWrite, FileShare.None, FileOptions.DeleteOnClose);
            _spooled = 0;
            Ui.Detail($"  {_name} 需要乱序读取，正在 {folder} 中保留最多 {ProgressReporter.Format(_streamSize)} 的临时副本");
        }

        if (!_warned)
        {
            _warned = true;
            Ui.Detail($"  {_name} 采用固实压缩，必须从头重新解码，这需要一些时间");
        }
        stream.Rewind();
    }

    private void OnDecoded(long position, byte[] data, int offset, int count)
    {
        foreach (var range in _kept)
        {
            long next = range.Start + range.Filled;
            if (range.Filled >= range.Size || next < position || next >= position + count)
                continue;
            range.Data ??= GC.AllocateUninitializedArray<byte>((int)range.Size);
            int n = (int)Math.Min(position + count - next, range.Size - range.Filled);
            Buffer.BlockCopy(data, offset + (int)(next - position), range.Data, (int)range.Filled, n);
            range.Filled += n;
        }

        if (_spool is not null && position == _spooled)
        {
            RandomAccess.Write(_spool, data.AsSpan(offset, count), position);
            _spooled += count;
        }
    }
}
