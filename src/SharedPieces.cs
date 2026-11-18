using System.Numerics;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace HacBrewPackLz;

/// <summary>
/// A game can hold the same data more than once. An update often reads the same part of the original game from more than one place, so a
/// game made part of its update holds that part more than once, and games repeat their own data too. Each piece held whole again is stored
/// once, and every other place it's held points at what was stored. The console's reader doesn't mind: a compression entry can start
/// anywhere, and several can point at the same stored bytes.
/// </summary>
internal static class SharedPieces
{
    /// <summary>
    /// One block of the image as it's stored. A shared one is piece <see cref="Piece"/>, -1 for any other, and <see cref="First"/> is where
    /// that piece is held first, where it's stored. <see cref="Found"/> is a place found to hold a piece again while compressing, rather
    /// than through the update.
    /// </summary>
    public readonly record struct Block(long Offset, int Length, int Piece, bool First, bool Found = false);

    public sealed class Plan
    {
        public required List<Block> Blocks { get; init; }
        /// <summary>How many places point at a piece stored somewhere else.</summary>
        public required int References { get; init; }
        /// <summary>The bytes those places hold, which aren't stored again.</summary>
        public long SharedBytes { get; init; }
    }

    /// <summary>Where the update's map has a piece of the original that's held more than once.</summary>
    private readonly record struct Place(long Image, int Piece, bool First);

    /// <summary>
    /// The blocks with only what the update's map gives shared, as the estimate takes them. Fixed blocks from the start, as without an
    /// update, wherever nothing is shared. Each shared piece is a block of its own, and what's between them is cut into blocks from where it
    /// starts.
    /// </summary>
    public static Plan Make(long imageLength, int blockSize, IReadOnlyList<UpdateMap.Range>? kept)
    {
        var shared = Places(imageLength, blockSize, kept, out _);
        var blocks = new List<Block>((int)Math.Min(imageLength / blockSize + shared.Count * 2 + 1, int.MaxValue));
        int next = 0;
        int references = 0;
        for (long position = 0; position < imageLength;)
        {
            if (next < shared.Count && shared[next].Image == position)
            {
                blocks.Add(new Block(position, blockSize, shared[next].Piece, shared[next].First));
                references += shared[next].First ? 0 : 1;
                position += blockSize;
                next++;
                continue;
            }
            long limit = next < shared.Count ? shared[next].Image : imageLength;
            int length = (int)Math.Min(blockSize, limit - position);
            blocks.Add(new Block(position, length, -1, false));
            position += length;
        }
        return new Plan { Blocks = blocks, References = references, SharedBytes = (long)references * blockSize };
    }

    /// <summary>Every place the update's map has a piece of the original that's held more than once, in order, and how many pieces that is.</summary>
    private static List<Place> Places(long imageLength, int blockSize, IReadOnlyList<UpdateMap.Range>? kept, out int pieces)
    {
        // Every whole piece of the original that the image holds, and where. They can't overlap, as the image only holds one thing in
        // each place, but anything that would is left out rather than trusted.
        var places = new List<(long image, long original)>();
        foreach (var range in kept ?? [])
        {
            long first = (range.Original + blockSize - 1) / blockSize * blockSize;
            for (long piece = first; piece + blockSize <= range.Original + range.Length; piece += blockSize)
            {
                long image = range.Image + (piece - range.Original);
                if (image >= 0 && image + blockSize <= imageLength)
                    places.Add((image, piece));
            }
        }
        places.Sort((a, b) => a.image != b.image ? a.image.CompareTo(b.image) : a.original.CompareTo(b.original));
        var apart = new List<(long image, long original)>();
        foreach (var place in places)
        {
            if (apart.Count == 0 || place.image >= apart[^1].image + blockSize)
                apart.Add(place);
        }
        var held = new Dictionary<long, int>();
        foreach (var place in apart)
            held[place.original] = held.GetValueOrDefault(place.original) + 1;

        // Pieces are numbered as they're first come to.
        var numbers = new Dictionary<long, int>();
        var shared = new List<Place>();
        foreach (var place in apart)
        {
            if (held[place.original] < 2)
                continue;
            bool first = !numbers.TryGetValue(place.original, out int number);
            if (first)
                numbers[place.original] = number = numbers.Count;
            shared.Add(new Place(place.image, number, first));
        }
        pieces = numbers.Count;
        return shared;
    }

    /// <summary>The parts of the original an update keeps that end up in the game data compressed here, if it's made part of its update.</summary>
    public static IReadOnlyList<UpdateMap.Range>? KeptBy(NcaFile owner, NcaFsSection section, NcaFile? original) =>
        original is not null && section.HasIndirect ? owner.KeptInImage(section, original) : null;

    /// <summary>
    /// Plans the blocks as the image is read, in order, and finds on the way what the game holds again by itself. Every place the update's
    /// map gives is a block of its own, as <see cref="Make"/> has it. Between them, a block's worth that's the same as a whole block stored
    /// before points at that block instead of being stored again, and the blocks go on from after it. Where nothing is held again, the blocks
    /// are the same as <see cref="Make"/>'s.
    ///
    /// Finding them takes spots that come with the content wherever it is: where a rolling hash of the 64 bytes up to a byte has its top bits
    /// clear, about 16 to a block. A block stored whole is remembered by its first spot (its key), its second, 16 words from across it, and a
    /// hash of all of it. A later spot with a block's key is where that block could be held again, which its second spot, the 16 words and
    /// then the hash of all of it there have to bear out. Before anything points at a block, compressing compares two more hashes of all of it.
    /// Many blocks can start alike, as a game's files often do, so a key is kept for up to 256 blocks.
    ///
    /// Only what comes after a block can point at it, so it all happens in the one read compressing does, 2 blocks behind the reading. A place
    /// that holds a block again can be anywhere, and every block after it then starts wherever that one ends: a Switch on 22.5.0 played a
    /// Minecraft copy with 13,559 of its 14,223 entries starting at places that aren't a multiple of 16.
    /// </summary>
    public sealed class Planner
    {
        /// <summary>The bytes the rolling hash takes in: a spot depends on these alone, wherever they are.</summary>
        private const int Span = 64;
        /// <summary>A key many blocks have is kept for the first ones. Hollow Knight finds 37 MB with 4, 74 MB with 256 and 75 MB with 1024.</summary>
        private const int BlocksPerKey = 256;
        /// <summary>Words from across a block that a candidate has to have the same before all of it is hashed.</summary>
        private const int SampleWords = 16;
        private const int ReadSize = 1 << 22;

        private static readonly ulong[] Gear = MakeGear();

        private sealed class Target
        {
            public long Offset;
            public int KeyOffset;
            /// <summary>-1 if the block has only the one spot.</summary>
            public int SecondOffset;
            public ulong SecondValue;
            public ulong Sample;
            public ulong Hash;
            public int Piece;
            /// <summary>Where it was last looked for. Within a block of that, it isn't looked for again, which keeps data that repeats every few bytes from being hashed at every one.</summary>
            public long LastTried = long.MinValue / 2;
            /// <summary>The next block with the same key, or -1.</summary>
            public int Next = -1;
        }

        private readonly DataSource _image;
        private readonly long _length;
        private readonly int _blockSize;
        private readonly int _shift;
        private readonly List<Place> _map;
        private int _nextMap;
        private int _pieces;
        private long _position;

        private readonly byte[] _buffer;
        private long _bufferStart;
        private int _bufferFill;
        private ulong _rolling;

        private readonly List<long> _spotAt = [];
        private readonly List<ulong> _spotValue = [];
        private int _firstSpot;

        private readonly Dictionary<ulong, int> _keys = [];
        private readonly List<Target> _targets = [];
        private readonly Queue<Block> _planned = new();
        private readonly List<(long At, int Target)> _candidates = [];

        public Planner(DataSource image, int blockSize, IReadOnlyList<UpdateMap.Range>? kept)
        {
            _image = image;
            _length = image.Length;
            _blockSize = blockSize;
            // A spot every 1/16 of a block on average.
            _shift = 64 - (BitOperations.Log2((uint)blockSize) - 4);
            _map = Places(_length, blockSize, kept, out _pieces);
            // The two blocks looked through, and a read: it only reads while it has less than those.
            _buffer = new byte[2 * blockSize + ReadSize];
        }

        /// <summary>
        /// The next blocks, up to <paramref name="most"/>, with their bytes copied to the start of <paramref name="input"/>. False once
        /// there are none left.
        /// </summary>
        public bool Next(byte[] input, int most, List<Block> blocks, out long start, out int bytes)
        {
            blocks.Clear();
            start = _planned.Count > 0 ? _planned.Peek().Offset : _position;
            bytes = 0;
            while (blocks.Count < most && (_planned.Count > 0 || _position < _length))
            {
                if (_planned.Count == 0)
                    Step();
                // Copied as it's planned, so the buffer only has to hold what's looked through.
                var block = _planned.Dequeue();
                Buffer.BlockCopy(_buffer, (int)(block.Offset - _bufferStart), input, bytes, block.Length);
                bytes += block.Length;
                blocks.Add(block);
            }
            return blocks.Count > 0;
        }

        /// <summary>Plans the block at the position, or a block held again and what's before it.</summary>
        private void Step()
        {
            Fill(_position, Math.Min(_position + 2L * _blockSize, _length));
            while (_firstSpot < _spotAt.Count && _spotAt[_firstSpot] < _position)
                _firstSpot++;
            if (_firstSpot >= 0x10000)
            {
                _spotAt.RemoveRange(0, _firstSpot);
                _spotValue.RemoveRange(0, _firstSpot);
                _firstSpot = 0;
            }

            long limit = _length;
            if (_nextMap < _map.Count)
            {
                var place = _map[_nextMap];
                if (place.Image == _position)
                {
                    _planned.Enqueue(new Block(_position, _blockSize, place.Piece, place.First));
                    if (place.First)
                        Remember(_position, place.Piece);
                    _position += _blockSize;
                    _nextMap++;
                    return;
                }
                limit = place.Image;
            }

            if (limit - _position >= _blockSize && Find(limit, out long at, out int piece))
            {
                if (at > _position)
                    _planned.Enqueue(new Block(_position, (int)(at - _position), -1, false));
                _planned.Enqueue(new Block(at, _blockSize, piece, false, Found: true));
                _position = at + _blockSize;
                return;
            }

            int length = (int)Math.Min(_blockSize, limit - _position);
            bool remembered = length == _blockSize && Remember(_position, _pieces);
            _planned.Enqueue(new Block(_position, length, remembered ? _pieces : -1, remembered));
            if (remembered)
                _pieces++;
            _position += length;
        }

        /// <summary>The earliest place in the next block's worth that holds a whole block stored before.</summary>
        private bool Find(long limit, out long at, out int piece)
        {
            long x = _position;
            long w = _blockSize;
            _candidates.Clear();
            // A block's key is at least 63 bytes into it, and less than a block.
            for (int i = FirstSpotFrom(x + Span - 1); i < _spotAt.Count && _spotAt[i] < x + 2 * w - 1; i++)
            {
                if (!_keys.TryGetValue(_spotValue[i], out int t))
                    continue;
                for (; t >= 0; t = _targets[t].Next)
                {
                    var target = _targets[t];
                    long q = _spotAt[i] - target.KeyOffset;
                    if (q >= x && q < x + w && q >= target.Offset + w && q + w <= limit)
                        _candidates.Add((q, t));
                }
            }
            _candidates.Sort();
            foreach (var (q, t) in _candidates)
            {
                var target = _targets[t];
                if (target.SecondOffset >= 0 && !HasSpot(q + target.SecondOffset, target.SecondValue))
                    continue;
                if (Sample(q) != target.Sample)
                    continue;
                if (q < target.LastTried + w)
                    continue;
                target.LastTried = q;
                if (Hash(Window(q), 0) != target.Hash)
                    continue;
                at = q;
                piece = target.Piece;
                return true;
            }
            at = 0;
            piece = -1;
            return false;
        }

        /// <summary>Takes note of a whole block stored here, so later data can point at it. False if it has no spot, or its key has enough blocks.</summary>
        private bool Remember(long offset, int piece)
        {
            long end = offset + _blockSize;
            int i = FirstSpotFrom(offset + Span - 1);
            if (i >= _spotAt.Count || _spotAt[i] >= end)
                return false;
            ulong key = _spotValue[i];
            int last = -1, count = 0;
            if (_keys.TryGetValue(key, out int t))
            {
                for (; t >= 0; t = _targets[t].Next)
                {
                    last = t;
                    count++;
                }
            }
            if (count >= BlocksPerKey)
                return false;
            bool second = i + 1 < _spotAt.Count && _spotAt[i + 1] < end;
            _targets.Add(new Target
            {
                Offset = offset,
                KeyOffset = (int)(_spotAt[i] - offset),
                SecondOffset = second ? (int)(_spotAt[i + 1] - offset) : -1,
                SecondValue = second ? _spotValue[i + 1] : 0,
                Sample = Sample(offset),
                Hash = Hash(Window(offset), 0),
                Piece = piece,
            });
            if (last >= 0)
                _targets[last].Next = _targets.Count - 1;
            else
                _keys[key] = _targets.Count - 1;
            return true;
        }

        private ReadOnlySpan<byte> Window(long offset) => _buffer.AsSpan((int)(offset - _bufferStart), _blockSize);

        /// <summary>A hash of a word from the middle of each 1/16 of the block here: far quicker than all of it, and blocks that only start alike differ.</summary>
        private ulong Sample(long offset)
        {
            Span<ulong> words = stackalloc ulong[SampleWords];
            ref byte start = ref _buffer[(int)(offset - _bufferStart)];
            int step = _blockSize / SampleWords;
            for (int k = 0; k < SampleWords; k++)
                words[k] = Unsafe.ReadUnaligned<ulong>(ref Unsafe.Add(ref start, k * step + step / 2));
            return Hash(MemoryMarshal.AsBytes(words), 3);
        }

        private bool HasSpot(long at, ulong value)
        {
            int i = FirstSpotFrom(at);
            return i < _spotAt.Count && _spotAt[i] == at && _spotValue[i] == value;
        }

        private int FirstSpotFrom(long at)
        {
            int low = _firstSpot, high = _spotAt.Count;
            while (low < high)
            {
                int middle = (low + high) >>> 1;
                if (_spotAt[middle] < at)
                    low = middle + 1;
                else
                    high = middle;
            }
            return low;
        }

        /// <summary>Reads on until the buffer holds the image up to need, keeping what's from keep on.</summary>
        private void Fill(long keep, long need)
        {
            while (_bufferStart + _bufferFill < need)
            {
                long end = _bufferStart + _bufferFill;
                int wanted = (int)Math.Min(ReadSize, _length - end);
                if (_buffer.Length - _bufferFill < wanted)
                {
                    int drop = (int)(keep - _bufferStart);
                    Buffer.BlockCopy(_buffer, drop, _buffer, 0, _bufferFill - drop);
                    _bufferStart = keep;
                    _bufferFill -= drop;
                }
                _image.Read(end, _buffer, _bufferFill, wanted);
                Roll(_bufferFill, wanted, end);
                _bufferFill += wanted;
            }
        }

        /// <summary>
        /// Every spot in the bytes, as the planner finds them for blocks this size. The first 63 bytes are left out, as a spot there depends
        /// on what's before them. Spots say what two places hold in common without either being read whole: see <see cref="FileCopies"/>.
        /// </summary>
        public static void SpotsIn(ReadOnlySpan<byte> data, int blockSize, HashSet<ulong> into)
        {
            int shift = 64 - (BitOperations.Log2((uint)blockSize) - 4);
            ulong h = 0;
            for (int i = 0; i < data.Length; i++)
            {
                h = (h << 1) + Gear[data[i]];
                if (i >= Span - 1 && h >> shift == 0)
                    into.Add(h);
            }
        }

        /// <summary>The rolling hash over bytes just read, noting every spot.</summary>
        private void Roll(int offset, int count, long at)
        {
            ulong h = _rolling;
            int shift = _shift;
            ref byte bytes = ref MemoryMarshal.GetArrayDataReference(_buffer);
            ref ulong gear = ref MemoryMarshal.GetArrayDataReference(Gear);
            for (int i = 0; i < count; i++)
            {
                h = (h << 1) + Unsafe.Add(ref gear, (nint)Unsafe.Add(ref bytes, (nint)(offset + i)));
                if (h >> shift == 0)
                {
                    _spotAt.Add(at + i);
                    _spotValue.Add(h);
                }
            }
            _rolling = h;
        }

        /// <summary>
        /// A fixed value for each byte, the same in every copy of the program. 64 bytes of one value come to that value's negative, which is
        /// never let be a spot, however small the blocks, so zeros and other padding have none.
        /// </summary>
        private static ulong[] MakeGear()
        {
            var gear = new ulong[256];
            ulong state = 0x2545F4914F6CDD1DUL;
            for (int b = 0; b < gear.Length; b++)
            {
                do
                {
                    gear[b] = SplitMix(ref state);
                }
                while (unchecked(0 - gear[b]) >> 56 == 0);
            }
            return gear;
        }

        private static ulong SplitMix(ref ulong state)
        {
            ulong z = state += 0x9E3779B97F4A7C15UL;
            z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9UL;
            z = (z ^ (z >> 27)) * 0x94D049BB133111EBUL;
            return z ^ (z >> 31);
        }
    }

    private const ulong Prime1 = 0x9E3779B185EBCA87UL;
    private const ulong Prime2 = 0xC2B2AE3D27D4EB4FUL;
    private const ulong Prime3 = 0x165667B19E3779F9UL;
    private const ulong Prime4 = 0x85EBCA77C2B2AE63UL;
    private const ulong Prime5 = 0x27D4EB2F165667C5UL;

    /// <summary>XXH64: quick, and two different blocks come to the same about once in 2^64, differently for each seed.</summary>
    public static ulong Hash(ReadOnlySpan<byte> data, ulong seed)
    {
        unchecked
        {
            int position = 0;
            ulong h;
            if (data.Length >= 32)
            {
                ulong v1 = seed + Prime1 + Prime2, v2 = seed + Prime2, v3 = seed, v4 = seed - Prime1;
                for (; position + 32 <= data.Length; position += 32)
                {
                    v1 = Round(v1, Word(data, position));
                    v2 = Round(v2, Word(data, position + 8));
                    v3 = Round(v3, Word(data, position + 16));
                    v4 = Round(v4, Word(data, position + 24));
                }
                h = BitOperations.RotateLeft(v1, 1) + BitOperations.RotateLeft(v2, 7) + BitOperations.RotateLeft(v3, 12) + BitOperations.RotateLeft(v4, 18);
                h = Merge(h, v1);
                h = Merge(h, v2);
                h = Merge(h, v3);
                h = Merge(h, v4);
            }
            else
            {
                h = seed + Prime5;
            }
            h += (ulong)data.Length;
            for (; position + 8 <= data.Length; position += 8)
                h = BitOperations.RotateLeft(h ^ Round(0, Word(data, position)), 27) * Prime1 + Prime4;
            if (position + 4 <= data.Length)
            {
                h = BitOperations.RotateLeft(h ^ (MemoryMarshal.Read<uint>(data[position..]) * Prime1), 23) * Prime2 + Prime3;
                position += 4;
            }
            for (; position < data.Length; position++)
                h = BitOperations.RotateLeft(h ^ (data[position] * Prime5), 11) * Prime1;
            h ^= h >> 33;
            h *= Prime2;
            h ^= h >> 29;
            h *= Prime3;
            h ^= h >> 32;
            return h;
        }
    }

    /// <summary>Only ever asked for 8 bytes that are there.</summary>
    private static ulong Word(ReadOnlySpan<byte> data, int position) => Unsafe.ReadUnaligned<ulong>(ref Unsafe.Add(ref MemoryMarshal.GetReference(data), position));

    private static ulong Round(ulong accumulator, ulong input) => unchecked(BitOperations.RotateLeft(accumulator + input * Prime2, 31) * Prime1);

    private static ulong Merge(ulong h, ulong v) => unchecked((h ^ Round(0, v)) * Prime1 + Prime4);
}
