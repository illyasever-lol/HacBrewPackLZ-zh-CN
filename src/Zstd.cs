using System.Buffers.Binary;
using System.Numerics;
using System.Runtime.CompilerServices;
using System.Runtime.Intrinsics.X86;

namespace HacBrewPackLz;

internal readonly record struct ZstdFrameHeader(int Size, long WindowSize, long ContentSize, bool HasChecksum);

/// <summary>
/// Zstandard decoding (RFC 8878), which is what NSZ and XCZ files are compressed with. Only decoding is needed, so
/// this is all there is and no native library comes along. An instance keeps tables that carry over from block to block
/// within a frame, so use one per thread. Files can come from anywhere: every read and write is checked against its
/// buffer, and anything malformed stops with a ToolException.
/// </summary>
internal sealed unsafe class ZstdDecoder
{
    public const int MaxBlockSize = 1 << 17;
    public const int MaxFrameHeaderSize = 18;

    private const uint FrameMagic = 0xFD2FB528;
    private const uint SkippableMagic = 0x184D2A50;
    private const uint SkippableMask = 0xFFFFFFF0;

    private const int MaxHuffmanBits = 11;
    private const int MaxWeightLog = 6;
    private const int LiteralLengthMaxLog = 9;
    private const int OffsetMaxLog = 8;
    private const int MatchLengthMaxLog = 9;
    private const int LiteralLengthMaxSymbol = 35;
    private const int OffsetMaxSymbol = 31;
    private const int MatchLengthMaxSymbol = 52;
    private const int WildCopySize = 16;
    private const int LookAhead = 8;
    private const int CacheLine = 64;

    private struct Sequence
    {
        public int LiteralLength;
        public int MatchLength;
        public ulong Offset;
    }

    private struct FseEntry
    {
        public ushort NextState;
        public byte Symbol;
        public byte Bits;
    }

    /// <summary>
    /// Reads a bitstream backwards from its end, the way Huffman and sequence streams are written. Everything the hot
    /// loops call is inlined, so its fields can stay in registers, which makes all the difference to how fast this goes.
    /// </summary>
    private struct BitReader
    {
        private byte* _start;
        private byte* _position;
        private ulong _bits;
        private uint _consumed;

        public static BitReader Create(byte* source, int size)
        {
            if (size < 1 || source[size - 1] == 0)
                throw Corrupt();
            var reader = new BitReader { _start = source };
            uint marker = (uint)BitOperations.Log2(source[size - 1]);
            if (size >= sizeof(ulong))
            {
                reader._position = source + size - sizeof(ulong);
                reader._bits = Unsafe.ReadUnaligned<ulong>(reader._position);
                reader._consumed = 8 - marker;
            }
            else
            {
                reader._position = source;
                for (int i = 0; i < size; i++)
                    reader._bits |= (ulong)source[i] << (8 * i);
                reader._consumed = 8 - marker + (uint)(sizeof(ulong) - size) * 8;
            }
            return reader;
        }

        public readonly bool IsFinished => _position == _start && _consumed == 64;

        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        public readonly ulong Peek(int count) => _bits << (int)(_consumed & 63) >> 1 >> ((63 - count) & 63);

        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        public void Skip(int count) => _consumed += (uint)count;

        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        public ulong Read(int count)
        {
            ulong value = Peek(count);
            _consumed += (uint)count;
            return value;
        }

        /// <summary>Tops the container up to at least 56 unread bits, as long as there are that many left.</summary>
        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        public void Reload()
        {
            if (_position - _start >= sizeof(ulong))
            {
                _position -= _consumed >> 3;
                _consumed &= 7;
                _bits = Unsafe.ReadUnaligned<ulong>(_position);
            }
            else if (_position != _start && _consumed <= 64)
            {
                uint bytes = Math.Min(_consumed >> 3, (uint)(_position - _start));
                _position -= bytes;
                _consumed -= bytes * 8;
                _bits = Unsafe.ReadUnaligned<ulong>(_position);
            }
        }

        /// <summary>
        /// Decodes Huffman symbols five at a time until they, or the bits that can be loaded in one go, run low. Five
        /// symbols of at most 11 bits fit in what a load guarantees, whatever was left over from the last one.
        /// </summary>
        public byte* DecodeSymbols(byte* destination, byte* end, ushort* table, int tableBits)
        {
            byte* position = _position;
            byte* limit = _start + sizeof(ulong);
            uint consumed = _consumed;
            int shift = 64 - tableBits;
            if (consumed >= 64)
                return destination;

            while (end - destination >= 5 && position >= limit)
            {
                position -= consumed >> 3;
                consumed &= 7;
                ulong bits = Unsafe.ReadUnaligned<ulong>(position) << (int)consumed;

                uint entry = table[bits >> shift];
                destination[0] = (byte)entry;
                bits <<= (int)(entry >> 8);
                consumed += entry >> 8;

                entry = table[bits >> shift];
                destination[1] = (byte)entry;
                bits <<= (int)(entry >> 8);
                consumed += entry >> 8;

                entry = table[bits >> shift];
                destination[2] = (byte)entry;
                bits <<= (int)(entry >> 8);
                consumed += entry >> 8;

                entry = table[bits >> shift];
                destination[3] = (byte)entry;
                bits <<= (int)(entry >> 8);
                consumed += entry >> 8;

                entry = table[bits >> shift];
                destination[4] = (byte)entry;
                consumed += entry >> 8;
                destination += 5;
            }

            _position = position;
            _consumed = consumed;
            _bits = Unsafe.ReadUnaligned<ulong>(position);
            return destination;
        }
    }

    private readonly byte[] _literals = new byte[MaxBlockSize + WildCopySize * 2];
    private readonly ushort[] _huffman = new ushort[1 << MaxHuffmanBits];
    private readonly FseEntry[] _weightTable = new FseEntry[1 << MaxWeightLog];
    private readonly FseEntry[] _literalLengthTable = new FseEntry[1 << LiteralLengthMaxLog];
    private readonly FseEntry[] _offsetTable = new FseEntry[1 << OffsetMaxLog];
    private readonly FseEntry[] _matchLengthTable = new FseEntry[1 << MatchLengthMaxLog];
    private readonly FseEntry[] _literalLengthRle = new FseEntry[1];
    private readonly FseEntry[] _offsetRle = new FseEntry[1];
    private readonly FseEntry[] _matchLengthRle = new FseEntry[1];
    private readonly short[] _counts = new short[256];
    private readonly ushort[] _nextStates = new ushort[256];

    private FseEntry[]? _literalLengths;
    private FseEntry[]? _offsets;
    private FseEntry[]? _matchLengths;
    private int _literalLengthLog;
    private int _offsetLog;
    private int _matchLengthLog;
    private int _huffmanBits;
    private ulong _repeat0;
    private ulong _repeat1;
    private ulong _repeat2;

    private static readonly FseEntry[] DefaultLiteralLengths = BuildDefault(
    [
        4, 3, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 3, 2, 1, 1, 1, 1, 1, -1, -1, -1, -1,
    ], 6);

    private static readonly FseEntry[] DefaultOffsets = BuildDefault(
    [
        1, 1, 1, 1, 1, 1, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, -1, -1, -1, -1, -1,
    ], 5);

    private static readonly FseEntry[] DefaultMatchLengths = BuildDefault(
    [
        1, 4, 3, 2, 2, 2, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
        1, 1, 1, 1, 1, 1, 1, 1, -1, -1, -1, -1, -1, -1, -1,
    ], 6);

    private static ReadOnlySpan<uint> LiteralLengthBase =>
    [
        0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 18, 20, 22, 24, 28, 32, 40, 48, 64, 128, 256, 512, 1024,
        2048, 4096, 8192, 16384, 32768, 65536,
    ];

    private static ReadOnlySpan<byte> LiteralLengthExtra =>
    [
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 3, 3, 4, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16,
    ];

    private static ReadOnlySpan<uint> MatchLengthBase =>
    [
        3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33,
        34, 35, 37, 39, 41, 43, 47, 51, 59, 67, 83, 99, 131, 259, 515, 1027, 2051, 4099, 8195, 16387, 32771, 65539,
    ];

    private static ReadOnlySpan<byte> MatchLengthExtra =>
    [
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 3, 3,
        4, 4, 5, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16,
    ];

    private static ToolException Corrupt() => new("Zstandard 流已损坏，或不是 Zstandard 数据");

    /// <summary>What's at the start of a frame, or false if more bytes are needed to tell. Not for skippable frames.</summary>
    public static bool TryReadFrameHeader(ReadOnlySpan<byte> source, out ZstdFrameHeader header)
    {
        header = default;
        if (source.Length < 5)
            return false;
        if (BinaryPrimitives.ReadUInt32LittleEndian(source) != FrameMagic)
            throw Corrupt();

        byte descriptor = source[4];
        int contentFlag = descriptor >> 6;
        bool singleSegment = (descriptor & 0x20) != 0;
        bool hasChecksum = (descriptor & 0x04) != 0;
        int dictionaryFlag = descriptor & 3;
        if ((descriptor & 0x08) != 0)
            throw Corrupt();

        int dictionarySize = dictionaryFlag == 3 ? 4 : dictionaryFlag;
        int contentSize = contentFlag switch
        {
            0 => singleSegment ? 1 : 0,
            1 => 2,
            2 => 4,
            _ => 8,
        };
        int size = 5 + (singleSegment ? 0 : 1) + dictionarySize + contentSize;
        if (source.Length < size)
            return false;

        int position = 5;
        long windowSize = 0;
        if (!singleSegment)
        {
            byte window = source[position++];
            long windowBase = 1L << (10 + (window >> 3));
            windowSize = windowBase + windowBase / 8 * (window & 7);
        }

        ulong dictionary = 0;
        for (int i = 0; i < dictionarySize; i++)
            dictionary |= (ulong)source[position++] << (8 * i);
        if (dictionary != 0)
            throw new ToolException("该 Zstandard 流需要字典，暂不支持");

        ulong content = 0;
        for (int i = 0; i < contentSize; i++)
            content |= (ulong)source[position + i] << (8 * i);
        if (contentFlag == 1)
            content += 256;
        if (content > long.MaxValue)
            throw Corrupt();

        long known = contentSize > 0 ? (long)content : -1;
        header = new ZstdFrameHeader(size, singleSegment ? known : windowSize, known, hasChecksum);
        return true;
    }

    /// <summary>How far to skip for a skippable frame at the start of this data, or -1 if it isn't one.</summary>
    public static long SkippableFrameSize(ReadOnlySpan<byte> source)
    {
        if (source.Length < 8 || (BinaryPrimitives.ReadUInt32LittleEndian(source) & SkippableMask) != SkippableMagic)
            return -1;
        return 8L + BinaryPrimitives.ReadUInt32LittleEndian(source[4..]);
    }

    /// <summary>Forgets everything that carries over between blocks, as every new frame has to.</summary>
    public void BeginFrame()
    {
        _repeat0 = 1;
        _repeat1 = 4;
        _repeat2 = 8;
        _literalLengths = _offsets = _matchLengths = null;
        _huffmanBits = 0;
    }

    /// <summary>Decompresses whole frames held in memory, as in a block of an NSZ. Returns how many bytes they made.</summary>
    public int Decompress(byte[] source, int sourceOffset, int sourceLength, byte[] destination, int destinationOffset, int capacity)
    {
        if (sourceOffset < 0 || sourceLength < 0 || sourceOffset + sourceLength > source.Length
            || destinationOffset < 0 || capacity < 0 || destinationOffset + capacity > destination.Length)
            throw new ArgumentOutOfRangeException(nameof(sourceOffset));
        if (capacity == 0)
            destination = new byte[1];

        fixed (byte* output = destination)
        fixed (byte* input = source.Length > 0 ? source : new byte[1])
        {
            byte* ip = input + sourceOffset;
            byte* iend = ip + sourceLength;
            byte* outputStart = output + destinationOffset;
            byte* op = outputStart;
            byte* oend = op + capacity;
            while (ip < iend)
            {
                var head = new ReadOnlySpan<byte>(ip, (int)Math.Min(iend - ip, MaxFrameHeaderSize));
                long skip = SkippableFrameSize(head);
                if (skip >= 0)
                {
                    if (skip > iend - ip)
                        throw Corrupt();
                    ip += skip;
                    continue;
                }

                if (!TryReadFrameHeader(head, out var header))
                    throw Corrupt();
                ip += header.Size;
                BeginFrame();
                byte* history = op;

                bool last;
                do
                {
                    if (iend - ip < 3)
                        throw Corrupt();
                    int blockHeader = ip[0] | (ip[1] << 8) | (ip[2] << 16);
                    ip += 3;
                    last = (blockHeader & 1) != 0;
                    int type = (blockHeader >> 1) & 3;
                    int size = blockHeader >> 3;
                    if (size > MaxBlockSize)
                        throw Corrupt();
                    switch (type)
                    {
                        case 0:
                            if (size > iend - ip || size > oend - op)
                                throw Corrupt();
                            Buffer.MemoryCopy(ip, op, size, size);
                            ip += size;
                            op += size;
                            break;
                        case 1:
                            if (iend - ip < 1 || size > oend - op)
                                throw Corrupt();
                            Unsafe.InitBlockUnaligned(op, *ip, (uint)size);
                            ip++;
                            op += size;
                            break;
                        case 2:
                            if (size > iend - ip)
                                throw Corrupt();
                            op += DecodeBlock(ip, size, history, op, oend);
                            ip += size;
                            break;
                        default:
                            throw Corrupt();
                    }
                }
                while (!last);

                if (header.HasChecksum)
                {
                    if (iend - ip < 4)
                        throw Corrupt();
                    ip += 4;
                }
            }
            return (int)(op - outputStart);
        }
    }

    /// <summary>
    /// Decodes one compressed block to <paramref name="目标"/>, with matches reaching back no further than
    /// <paramref name="历史"/>, and returns how many bytes it made.
    /// </summary>
    public int DecodeBlock(byte* source, int size, byte* history, byte* destination, byte* destinationEnd)
    {
        int consumed = ReadLiterals(source, size, out int literalCount);
        source += consumed;
        size -= consumed;
        if (size < 1)
            throw Corrupt();

        int sequenceCount = source[0];
        int header = 1;
        if (sequenceCount >= 255)
        {
            if (size < 3)
                throw Corrupt();
            sequenceCount = source[1] + (source[2] << 8) + 0x7F00;
            header = 3;
        }
        else if (sequenceCount >= 128)
        {
            if (size < 2)
                throw Corrupt();
            sequenceCount = ((sequenceCount - 128) << 8) + source[1];
            header = 2;
        }
        source += header;
        size -= header;

        fixed (byte* literals = _literals)
        {
            if (sequenceCount == 0)
            {
                if (size != 0 || literalCount > destinationEnd - destination)
                    throw Corrupt();
                Buffer.MemoryCopy(literals, destination, literalCount, literalCount);
                return literalCount;
            }

            if (size < 1)
                throw Corrupt();
            int modes = *source++;
            size--;
            if ((modes & 3) != 0)
                throw Corrupt();

            int used = ReadSequenceTable(modes >> 6, source, size, ref _literalLengths, _literalLengthTable, _literalLengthRle, DefaultLiteralLengths, 6, LiteralLengthMaxLog, LiteralLengthMaxSymbol, ref _literalLengthLog);
            source += used;
            size -= used;
            used = ReadSequenceTable((modes >> 4) & 3, source, size, ref _offsets, _offsetTable, _offsetRle, DefaultOffsets, 5, OffsetMaxLog, OffsetMaxSymbol, ref _offsetLog);
            source += used;
            size -= used;
            used = ReadSequenceTable((modes >> 2) & 3, source, size, ref _matchLengths, _matchLengthTable, _matchLengthRle, DefaultMatchLengths, 6, MatchLengthMaxLog, MatchLengthMaxSymbol, ref _matchLengthLog);
            source += used;
            size -= used;

            return ExecuteSequences(source, size, sequenceCount, literals, literalCount, history, destination, destinationEnd);
        }
    }

    /// <summary>
    /// Strongly compressed games are full of short matches far back in a large window, and each of those would miss the
    /// CPU's cache. So sequences are decoded a little ahead of being carried out, and the memory each will read is
    /// fetched in the meantime, as the reference decoder does. That alone more than doubles the speed on such data.
    /// </summary>
    private int ExecuteSequences(byte* source, int size, int sequenceCount, byte* literals, int literalCount, byte* history, byte* destination, byte* destinationEnd)
    {
        var reader = BitReader.Create(source, size);

        byte* op = destination;
        byte* literal = literals;
        byte* literalEnd = literals + literalCount;
        byte* upcoming = destination;
        Sequence* queue = stackalloc Sequence[LookAhead];
        ulong repeat0 = _repeat0, repeat1 = _repeat1, repeat2 = _repeat2;

        fixed (FseEntry* literalLengths = _literalLengths!, offsets = _offsets!, matchLengths = _matchLengths!)
        fixed (uint* literalBase = LiteralLengthBase, matchBase = MatchLengthBase)
        fixed (byte* literalExtra = LiteralLengthExtra, matchExtra = MatchLengthExtra)
        {
            reader.Reload();
            int literalState = (int)reader.Read(_literalLengthLog);
            reader.Reload();
            int offsetState = (int)reader.Read(_offsetLog);
            reader.Reload();
            int matchState = (int)reader.Read(_matchLengthLog);

            for (int index = 0; index < sequenceCount; index++)
            {
                FseEntry literalEntry = literalLengths[literalState];
                FseEntry offsetEntry = offsets[offsetState];
                FseEntry matchEntry = matchLengths[matchState];

                // A reload always leaves at least 56 bits: enough for an offset and a match length, then for the rest.
                reader.Reload();
                ulong offset = (1UL << offsetEntry.Symbol) + reader.Read(offsetEntry.Symbol);
                int matchLength = (int)(matchBase[matchEntry.Symbol] + (uint)reader.Read(matchExtra[matchEntry.Symbol]));
                reader.Reload();
                int literalLength = (int)(literalBase[literalEntry.Symbol] + (uint)reader.Read(literalExtra[literalEntry.Symbol]));

                if (offset > 3)
                {
                    offset -= 3;
                    repeat2 = repeat1;
                    repeat1 = repeat0;
                    repeat0 = offset;
                }
                else
                {
                    switch ((int)offset - 1 + (literalLength == 0 ? 1 : 0))
                    {
                        case 0:
                            offset = repeat0;
                            break;
                        case 1:
                            offset = repeat1;
                            repeat1 = repeat0;
                            repeat0 = offset;
                            break;
                        case 2:
                            offset = repeat2;
                            repeat2 = repeat1;
                            repeat1 = repeat0;
                            repeat0 = offset;
                            break;
                        default:
                            offset = repeat0 - 1;
                            repeat2 = repeat1;
                            repeat1 = repeat0;
                            repeat0 = offset;
                            break;
                    }
                }

                if (index + 1 < sequenceCount)
                {
                    literalState = literalEntry.NextState + (int)reader.Read(literalEntry.Bits);
                    matchState = matchEntry.NextState + (int)reader.Read(matchEntry.Bits);
                    offsetState = offsetEntry.NextState + (int)reader.Read(offsetEntry.Bits);
                }

                // Asking for an address is harmless even if it turns out to be invalid: this reads nothing itself.
                upcoming += literalLength;
                if (Sse.IsSupported)
                {
                    byte* from = upcoming - (long)offset;
                    Sse.Prefetch0(from);
                    Sse.Prefetch0(from + CacheLine);
                }
                upcoming += matchLength;

                if (index >= LookAhead)
                {
                    Sequence* next = queue + ((index - LookAhead) & (LookAhead - 1));
                    Execute(*next, ref op, ref literal, literalEnd, history, destinationEnd);
                }
                queue[index & (LookAhead - 1)] = new Sequence { LiteralLength = literalLength, MatchLength = matchLength, Offset = offset };
            }

            for (int index = Math.Max(sequenceCount - LookAhead, 0); index < sequenceCount; index++)
                Execute(queue[index & (LookAhead - 1)], ref op, ref literal, literalEnd, history, destinationEnd);
        }

        reader.Reload();
        if (!reader.IsFinished)
            throw Corrupt();
        _repeat0 = repeat0;
        _repeat1 = repeat1;
        _repeat2 = repeat2;

        long left = literalEnd - literal;
        if (left > destinationEnd - op)
            throw Corrupt();
        Buffer.MemoryCopy(literal, op, left, left);
        op += left;
        return (int)(op - destination);
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    private static void Execute(in Sequence sequence, ref byte* output, ref byte* literal, byte* literalEnd, byte* history, byte* outputEnd)
    {
        byte* op = output;
        int literalLength = sequence.LiteralLength;
        int matchLength = sequence.MatchLength;
        ulong offset = sequence.Offset;
        if (literalLength > literalEnd - literal || (long)literalLength + matchLength > outputEnd - op)
            throw Corrupt();

        if (outputEnd - op - literalLength >= WildCopySize)
            WildCopy(op, literal, literalLength);
        else
            Buffer.MemoryCopy(literal, op, literalLength, literalLength);
        op += literalLength;
        literal += literalLength;

        if (offset == 0 || offset > (ulong)(op - history))
            throw Corrupt();
        byte* match = op - (long)offset;
        if (offset >= WildCopySize && outputEnd - op - matchLength >= WildCopySize)
        {
            WildCopy(op, match, matchLength);
        }
        else if (offset == 1)
        {
            Unsafe.InitBlockUnaligned(op, *match, (uint)matchLength);
        }
        else
        {
            for (int i = 0; i < matchLength; i++)
                op[i] = match[i];
        }
        output = op + matchLength;
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    private static void WildCopy(byte* destination, byte* source, int count)
    {
        byte* end = destination + count;
        do
        {
            Unsafe.CopyBlockUnaligned(destination, source, WildCopySize);
            destination += WildCopySize;
            source += WildCopySize;
        }
        while (destination < end);
    }

    private int ReadSequenceTable(int mode, byte* source, int size, ref FseEntry[]? current, FseEntry[] own, FseEntry[] rle, FseEntry[] defaults, int defaultLog, int maxLog, int maxSymbol, ref int log)
    {
        switch (mode)
        {
            case 0:
                current = defaults;
                log = defaultLog;
                return 0;
            case 1:
                if (size < 1 || source[0] > maxSymbol)
                    throw Corrupt();
                rle[0] = new FseEntry { Symbol = source[0] };
                current = rle;
                log = 0;
                return 1;
            case 2:
                int used = ReadCounts(source, size, maxLog, maxSymbol, out int symbols, out log);
                BuildTable(_counts, symbols, log, own, _nextStates);
                current = own;
                return used;
            default:
                if (current is null)
                    throw Corrupt();
                return 0;
        }
    }

    /// <summary>Reads a table's symbol counts into _counts, and returns how many bytes that took.</summary>
    private int ReadCounts(byte* source, int size, int maxLog, int maxSymbol, out int symbols, out int log)
    {
        long position = 0;
        log = 5 + (int)ReadForward(source, size, ref position, 4);
        if (log > maxLog)
            throw Corrupt();

        int remaining = 1 << log;
        int symbol = 0;
        while (remaining > 0 && symbol <= maxSymbol)
        {
            int bits = BitOperations.Log2((uint)remaining + 1) + 1;
            int value = (int)ReadForward(source, size, ref position, bits);
            int lowerMask = (1 << (bits - 1)) - 1;
            int threshold = (1 << bits) - 1 - (remaining + 1);
            if ((value & lowerMask) < threshold)
            {
                position--;
                value &= lowerMask;
            }
            else if (value > lowerMask)
            {
                value -= threshold;
            }

            int count = value - 1;
            remaining -= count < 0 ? -count : count;
            _counts[symbol++] = (short)count;

            if (count == 0)
            {
                int repeat = (int)ReadForward(source, size, ref position, 2);
                while (true)
                {
                    for (int i = 0; i < repeat; i++)
                    {
                        if (symbol > maxSymbol)
                            throw Corrupt();
                        _counts[symbol++] = 0;
                    }
                    if (repeat != 3)
                        break;
                    repeat = (int)ReadForward(source, size, ref position, 2);
                }
            }
        }

        long bytes = (position + 7) >> 3;
        if (remaining != 0 || bytes > size)
            throw Corrupt();
        symbols = symbol;
        return (int)bytes;
    }

    private static uint ReadForward(byte* source, int size, ref long position, int count)
    {
        ulong value = 0;
        long index = position >> 3;
        for (int i = 0; i < sizeof(uint) + 1 && index + i < size; i++)
            value |= (ulong)source[index + i] << (8 * i);
        uint result = (uint)((value >> (int)(position & 7)) & ((1UL << count) - 1));
        position += count;
        return result;
    }

    private static FseEntry[] BuildDefault(short[] counts, int log)
    {
        var table = new FseEntry[1 << log];
        BuildTable(counts, counts.Length, log, table, new ushort[256]);
        return table;
    }

    private static void BuildTable(short[] counts, int symbols, int log, FseEntry[] table, ushort[] nextStates)
    {
        int size = 1 << log;
        int mask = size - 1;
        int highThreshold = size;
        for (int s = 0; s < symbols; s++)
        {
            if (counts[s] == -1)
            {
                table[--highThreshold].Symbol = (byte)s;
                nextStates[s] = 1;
            }
            else
            {
                nextStates[s] = (ushort)Math.Max((int)counts[s], 0);
            }
        }

        int step = (size >> 1) + (size >> 3) + 3;
        int position = 0;
        for (int s = 0; s < symbols; s++)
        {
            for (int i = 0; i < counts[s]; i++)
            {
                table[position].Symbol = (byte)s;
                do
                {
                    position = (position + step) & mask;
                }
                while (position >= highThreshold);
            }
        }
        if (position != 0)
            throw Corrupt();

        for (int i = 0; i < size; i++)
        {
            int state = nextStates[table[i].Symbol]++;
            int bits = log - BitOperations.Log2((uint)state);
            table[i].Bits = (byte)bits;
            table[i].NextState = (ushort)((state << bits) - size);
        }
    }

    /// <summary>Reads the literals section into _literals. Returns the bytes it took up.</summary>
    private int ReadLiterals(byte* source, int size, out int literalCount)
    {
        if (size < 1)
            throw Corrupt();

        int type = source[0] & 3;
        int format = (source[0] >> 2) & 3;
        int header, regenerated;
        if (type < 2)
        {
            switch (format)
            {
                case 1:
                    if (size < 2)
                        throw Corrupt();
                    regenerated = (source[0] >> 4) | (source[1] << 4);
                    header = 2;
                    break;
                case 3:
                    if (size < 3)
                        throw Corrupt();
                    regenerated = (source[0] >> 4) | (source[1] << 4) | (source[2] << 12);
                    header = 3;
                    break;
                default:
                    regenerated = source[0] >> 3;
                    header = 1;
                    break;
            }
            if (regenerated > MaxBlockSize)
                throw Corrupt();
            literalCount = regenerated;

            fixed (byte* literals = _literals)
            {
                if (type == 0)
                {
                    if (regenerated > size - header)
                        throw Corrupt();
                    Buffer.MemoryCopy(source + header, literals, regenerated, regenerated);
                    return header + regenerated;
                }
                if (size - header < 1)
                    throw Corrupt();
                Unsafe.InitBlockUnaligned(literals, source[header], (uint)regenerated);
                return header + 1;
            }
        }

        int compressed;
        switch (format)
        {
            case 0 or 1:
                if (size < 3)
                    throw Corrupt();
                int small = source[0] | (source[1] << 8) | (source[2] << 16);
                regenerated = (small >> 4) & 0x3FF;
                compressed = (small >> 14) & 0x3FF;
                header = 3;
                break;
            case 2:
                if (size < 4)
                    throw Corrupt();
                uint medium = Unsafe.ReadUnaligned<uint>(source);
                regenerated = (int)((medium >> 4) & 0x3FFF);
                compressed = (int)((medium >> 18) & 0x3FFF);
                header = 4;
                break;
            default:
                if (size < 5)
                    throw Corrupt();
                ulong large = Unsafe.ReadUnaligned<uint>(source) | ((ulong)source[4] << 32);
                regenerated = (int)((large >> 4) & 0x3FFFF);
                compressed = (int)((large >> 22) & 0x3FFFF);
                header = 5;
                break;
        }
        if (regenerated > MaxBlockSize || compressed > size - header || compressed < 1)
            throw Corrupt();
        literalCount = regenerated;

        byte* stream = source + header;
        int left = compressed;
        if (type == 2)
        {
            int used = ReadHuffmanTable(stream, left);
            stream += used;
            left -= used;
        }
        else if (_huffmanBits == 0)
        {
            throw Corrupt();
        }

        fixed (byte* literals = _literals)
        fixed (ushort* table = _huffman)
        {
            if (format == 0)
            {
                DecodeHuffman(stream, left, literals, regenerated, table);
            }
            else
            {
                if (left < 6)
                    throw Corrupt();
                int size1 = stream[0] | (stream[1] << 8);
                int size2 = stream[2] | (stream[3] << 8);
                int size3 = stream[4] | (stream[5] << 8);
                stream += 6;
                left -= 6;
                int size4 = left - size1 - size2 - size3;
                int segment = (regenerated + 3) / 4;
                int lastSegment = regenerated - segment * 3;
                if (size4 < 1 || lastSegment < 0)
                    throw Corrupt();
                DecodeHuffman(stream, size1, literals, segment, table);
                DecodeHuffman(stream + size1, size2, literals + segment, segment, table);
                DecodeHuffman(stream + size1 + size2, size3, literals + segment * 2, segment, table);
                DecodeHuffman(stream + size1 + size2 + size3, size4, literals + segment * 3, lastSegment, table);
            }
        }
        return header + compressed;
    }

    private void DecodeHuffman(byte* source, int size, byte* destination, int count, ushort* table)
    {
        var reader = BitReader.Create(source, size);
        int bits = _huffmanBits;
        byte* end = destination + count;

        if (size >= sizeof(ulong))
            destination = reader.DecodeSymbols(destination, end, table, bits);

        // What's left is the last few symbols, or the last few bytes of the stream, where fewer bits can be loaded.
        while (destination < end)
        {
            reader.Reload();
            ushort entry = table[reader.Peek(bits)];
            reader.Skip(entry >> 8);
            *destination++ = (byte)entry;
        }

        reader.Reload();
        if (!reader.IsFinished)
            throw Corrupt();
    }

    private int ReadHuffmanTable(byte* source, int size)
    {
        if (size < 1)
            throw Corrupt();

        byte* weights = stackalloc byte[256];
        int first = source[0];
        int count, used;
        if (first >= 128)
        {
            count = first - 127;
            used = 1 + (count + 1) / 2;
            if (used > size)
                throw Corrupt();
            for (int i = 0; i < count; i++)
            {
                byte packed = source[1 + i / 2];
                weights[i] = (byte)((i & 1) == 0 ? packed >> 4 : packed & 15);
            }
        }
        else
        {
            used = 1 + first;
            if (used > size)
                throw Corrupt();
            count = ReadCompressedWeights(source + 1, first, weights);
        }

        uint total = 0;
        for (int i = 0; i < count; i++)
        {
            if (weights[i] > MaxHuffmanBits)
                throw Corrupt();
            if (weights[i] > 0)
                total += 1u << (weights[i] - 1);
        }
        if (total == 0)
            throw Corrupt();

        int bits = BitOperations.Log2(total) + 1;
        uint left = (1u << bits) - total;
        if (bits > MaxHuffmanBits || !BitOperations.IsPow2(left))
            throw Corrupt();
        weights[count++] = (byte)(BitOperations.Log2(left) + 1);

        Span<int> ranks = stackalloc int[MaxHuffmanBits + 2];
        ranks.Clear();
        for (int i = 0; i < count; i++)
        {
            if (weights[i] > 0)
                ranks[bits + 1 - weights[i]]++;
        }

        // The longest codes take the first table entries, then each shorter length follows in symbol order.
        Span<int> next = stackalloc int[MaxHuffmanBits + 2];
        next[bits] = 0;
        for (int i = bits; i >= 1; i--)
            next[i - 1] = next[i] + ranks[i] * (1 << (bits - i));

        for (int symbol = 0; symbol < count; symbol++)
        {
            if (weights[symbol] == 0)
                continue;
            int length = bits + 1 - weights[symbol];
            int span = 1 << (bits - length);
            int start = next[length];
            if (start + span > 1 << bits)
                throw Corrupt();
            ushort entry = (ushort)(symbol | (length << 8));
            for (int i = 0; i < span; i++)
                _huffman[start + i] = entry;
            next[length] = start + span;
        }
        _huffmanBits = bits;
        return used;
    }

    private int ReadCompressedWeights(byte* source, int size, byte* weights)
    {
        int used = ReadCounts(source, size, MaxWeightLog, 255, out int symbols, out int log);
        BuildTable(_counts, symbols, log, _weightTable, _nextStates);
        source += used;
        size -= used;
        if (size < 1 || source[size - 1] == 0)
            throw Corrupt();

        long position = size * 8L - (8 - BitOperations.Log2(source[size - 1]));
        int state1 = ReadBackward(source, ref position, log);
        int state2 = ReadBackward(source, ref position, log);
        int count = 0;
        while (true)
        {
            if (count >= 254)
                throw Corrupt();
            FseEntry entry = _weightTable[state1];
            weights[count++] = entry.Symbol;
            state1 = entry.NextState + ReadBackward(source, ref position, entry.Bits);
            if (position < 0)
            {
                weights[count++] = _weightTable[state2].Symbol;
                break;
            }

            entry = _weightTable[state2];
            weights[count++] = entry.Symbol;
            state2 = entry.NextState + ReadBackward(source, ref position, entry.Bits);
            if (position < 0)
            {
                weights[count++] = _weightTable[state1].Symbol;
                break;
            }
        }
        return count;
    }

    private static int ReadBackward(byte* source, ref long position, int count)
    {
        position -= count;
        long start = position;
        int bits = count;
        if (start < 0)
        {
            bits += (int)start;
            start = 0;
        }
        if (bits <= 0)
            return 0;

        int value = 0;
        for (int i = 0; i < bits; i++)
        {
            long bit = start + i;
            value |= ((source[bit >> 3] >> (int)(bit & 7)) & 1) << i;
        }
        return position < 0 ? value << (int)-position : value;
    }
}

/// <summary>
/// A Zstandard stream decoded front to back, as a solid NSZ has to be. What it decoded most recently stays in memory,
/// because later blocks refer back to it, and that much can also be read again without starting over.
/// </summary>
/// <param name="expectedSize">How much the stream should decode to, if known. It saves memory on small streams.</param>
internal sealed unsafe class ZstdStream(DataSource source, long start, long length, long expectedSize = long.MaxValue)
{
    private const int InputSize = 1 << 20;
    private const int MinimumOutputSize = 1 << 24;
    private const long MaxWindowSize = 1L << 30;

    private readonly ZstdDecoder _decoder = new();
    private readonly byte[] _input = new byte[InputSize];
    private byte[] _output = [];
    private int _inputPosition;
    private int _inputEnd;
    private long _sourcePosition;
    private int _outputEnd;
    private int _frameStart;
    private long _outputBase;
    private long _windowSize;
    private bool _inFrame;
    private bool _hasChecksum;

    /// <summary>Told about everything as it's decoded: its position in the stream, then where it is.</summary>
    public Action<long, byte[], int, int>? Decoded { get; set; }

    /// <summary>The stream position just after what has been decoded.</summary>
    public long Position => _outputBase + _outputEnd;

    /// <summary>The earliest stream position that can still be read without starting over.</summary>
    public long Oldest => _outputBase;

    public void Rewind()
    {
        _inputPosition = _inputEnd = 0;
        _sourcePosition = 0;
        _outputEnd = 0;
        _frameStart = 0;
        _outputBase = 0;
        _inFrame = false;
    }

    public int Copy(long position, byte[] buffer, int offset, int count)
    {
        long inOutput = position - _outputBase;
        if (inOutput < 0 || inOutput >= _outputEnd)
            throw new ArgumentOutOfRangeException(nameof(position));
        int n = (int)Math.Min(count, _outputEnd - inOutput);
        Buffer.BlockCopy(_output, (int)inOutput, buffer, offset, n);
        return n;
    }

    /// <summary>Decodes the next block. False means the stream has ended.</summary>
    public bool Advance()
    {
        Ui.Token.ThrowIfCancellationRequested();
        if (!_inFrame && !BeginFrame())
            return false;

        Require(3);
        int blockHeader = _input[_inputPosition] | (_input[_inputPosition + 1] << 8) | (_input[_inputPosition + 2] << 16);
        _inputPosition += 3;
        bool last = (blockHeader & 1) != 0;
        int type = (blockHeader >> 1) & 3;
        int size = blockHeader >> 3;
        if (size > ZstdDecoder.MaxBlockSize)
            throw Damaged();

        MakeRoom();
        int produced;
        switch (type)
        {
            case 0:
                Require(size);
                Buffer.BlockCopy(_input, _inputPosition, _output, _outputEnd, size);
                _inputPosition += size;
                produced = size;
                break;
            case 1:
                Require(1);
                _output.AsSpan(_outputEnd, size).Fill(_input[_inputPosition]);
                _inputPosition++;
                produced = size;
                break;
            case 2:
                Require(size);
                fixed (byte* input = _input)
                fixed (byte* output = _output)
                {
                    produced = _decoder.DecodeBlock(input + _inputPosition, size, output + _frameStart, output + _outputEnd, output + _output.Length);
                }
                _inputPosition += size;
                break;
            default:
                throw Damaged();
        }

        long position = Position;
        _outputEnd += produced;
        Decoded?.Invoke(position, _output, _outputEnd - produced, produced);

        if (last)
        {
            if (_hasChecksum)
            {
                Require(4);
                _inputPosition += 4;
            }
            _inFrame = false;
        }
        return true;
    }

    private static ToolException Damaged() => new("Zstandard 流已损坏，或提前结束");

    private bool BeginFrame()
    {
        while (true)
        {
            Fill(ZstdDecoder.MaxFrameHeaderSize);
            int available = _inputEnd - _inputPosition;
            if (available == 0)
                return false;

            var head = _input.AsSpan(_inputPosition, available);
            long skip = ZstdDecoder.SkippableFrameSize(head);
            if (skip >= 0)
            {
                Skip(skip);
                continue;
            }

            if (!ZstdDecoder.TryReadFrameHeader(head, out var header))
                throw Damaged();
            _inputPosition += header.Size;
            _hasChecksum = header.HasChecksum;
            _windowSize = Math.Max(Math.Min(header.WindowSize, expectedSize), ZstdDecoder.MaxBlockSize);
            if (_windowSize > MaxWindowSize)
                throw new ToolException("该 Zstandard 流解码需要超过 1 GiB 内存，暂不支持");
            GrowOutput(_windowSize);

            _decoder.BeginFrame();
            MakeRoom();
            _frameStart = _outputEnd;
            _inFrame = true;
            return true;
        }
    }

    private void GrowOutput(long windowSize)
    {
        long wanted = Math.Max(windowSize * 2, Math.Min(MinimumOutputSize, expectedSize)) + ZstdDecoder.MaxBlockSize * 2;
        wanted = Math.Min(wanted, Array.MaxLength);
        if (_output.Length >= wanted)
            return;
        try
        {
            var larger = GC.AllocateUninitializedArray<byte>((int)wanted);
            Buffer.BlockCopy(_output, 0, larger, 0, _outputEnd);
            _output = larger;
        }
        catch (OutOfMemoryException)
        {
            throw new ToolException($"There isn't enough free memory to decode this file: it needs {ProgressReporter.Format(wanted)}");
        }
    }

    /// <summary>Keeps a window's worth of what was decoded, and moves it down once the rest of the buffer is used up.</summary>
    private void MakeRoom()
    {
        if (_outputEnd + ZstdDecoder.MaxBlockSize * 2 <= _output.Length)
            return;
        int keep = (int)Math.Min(_windowSize, _outputEnd);
        int removed = _outputEnd - keep;
        Buffer.BlockCopy(_output, removed, _output, 0, keep);
        _outputEnd = keep;
        _outputBase += removed;
        _frameStart = Math.Max(_frameStart - removed, 0);
    }

    private void Require(int count)
    {
        Fill(count);
        if (_inputEnd - _inputPosition < count)
            throw Damaged();
    }

    private void Fill(int count)
    {
        if (_inputEnd - _inputPosition >= count)
            return;
        int left = _inputEnd - _inputPosition;
        Buffer.BlockCopy(_input, _inputPosition, _input, 0, left);
        _inputPosition = 0;
        _inputEnd = left;
        int n = (int)Math.Min(_input.Length - left, length - _sourcePosition);
        if (n > 0)
        {
            source.Read(start + _sourcePosition, _input, left, n);
            _sourcePosition += n;
            _inputEnd += n;
        }
    }

    private void Skip(long count)
    {
        long buffered = _inputEnd - _inputPosition;
        if (count <= buffered)
        {
            _inputPosition += (int)count;
            return;
        }
        long beyond = count - buffered;
        if (beyond > length - _sourcePosition)
            throw Damaged();
        _sourcePosition += beyond;
        _inputPosition = _inputEnd = 0;
    }
}
