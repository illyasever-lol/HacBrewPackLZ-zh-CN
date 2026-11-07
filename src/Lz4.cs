using System.Numerics;
using System.Runtime.CompilerServices;

namespace HacBrewPackLz;

/// <summary>
/// LZ4 raw block encoder for blocks of at most 64 KiB, which is all the NCA compressed storage layer accepts.
/// Levels 1-2 are greedy, 3-9 lazy hash chain, 10-12 optimal parsing. Output obeys the LZ4 end of block rules:
/// the last 5 bytes are literals and the last match starts at least 12 bytes before the end.
/// Instances keep scratch tables, so use one per thread.
/// </summary>
internal sealed unsafe class Lz4Encoder
{
    public const int MaxBlockSize = 0x10000;
    public const int MinLevel = 1;
    public const int MaxLevel = 12;

    private const int MinMatch = 4;
    private const int MfLimit = 12;
    private const int LastLiterals = 5;
    private const int HashBits = 16;

    private readonly ushort[] _head = new ushort[1 << HashBits];
    private readonly ushort[] _prev = new ushort[MaxBlockSize];

    private int[]? _price;
    private int[]? _run;
    private int[]? _length;
    private ushort[]? _distance;
    private int[]? _path;

    /// <summary>Returns the compressed size, or -1 when the result would not fit in <paramref name="dstCapacity"/>.</summary>
    public int Compress(byte[] src, int srcOffset, int srcLength, byte[] dst, int dstOffset, int dstCapacity, int level)
    {
        ArgumentOutOfRangeException.ThrowIfGreaterThan(srcLength, MaxBlockSize);
        if (srcLength <= MfLimit)
            return -1;

        Array.Clear(_head);
        fixed (byte* s = &src[srcOffset], d = &dst[dstOffset])
        fixed (ushort* head = _head, prev = _prev)
        {
            return level switch
            {
                <= 2 => CompressGreedy(s, srcLength, d, dstCapacity, head, level == 1 ? 5 : 8),
                <= 9 => CompressLazy(s, srcLength, d, dstCapacity, head, prev, 1 << (level - 1)),
                10 => CompressOptimal(s, srcLength, d, dstCapacity, head, prev, 96, 64),
                11 => CompressOptimal(s, srcLength, d, dstCapacity, head, prev, 512, 128),
                _ => CompressOptimal(s, srcLength, d, dstCapacity, head, prev, 16384, 4096),
            };
        }
    }

    /// <summary>One probe per position and no chains, skipping ahead faster the longer it goes without finding a match.</summary>
    private static int CompressGreedy(byte* s, int n, byte* d, int capacity, ushort* head, int skipShift)
    {
        byte* op = d;
        byte* opEnd = d + capacity;
        byte* limit = s + (n - LastLiterals);
        int ip = 0, anchor = 0;
        int mfLimit = n - MfLimit;

        while (ip <= mfLimit)
        {
            uint sequence = *(uint*)(s + ip);
            uint h = Hash(sequence);
            int candidate = head[h] - 1;
            head[h] = (ushort)(ip + 1);
            if (candidate < 0 || *(uint*)(s + candidate) != sequence)
            {
                ip += 1 + ((ip - anchor) >> skipShift);
                continue;
            }

            int length = MinMatch + CommonLength(s + ip + MinMatch, s + candidate + MinMatch, limit);
            if (!EmitSequence(ref op, opEnd, s + anchor, ip - anchor, ip - candidate, length))
                return -1;
            ip += length;
            anchor = ip;
        }
        return EmitLastLiterals(ref op, opEnd, s + anchor, n - anchor) ? (int)(op - d) : -1;
    }

    private static int CompressLazy(byte* s, int n, byte* d, int capacity, ushort* head, ushort* prev, int attempts)
    {
        const int GoodEnough = 512;

        byte* op = d;
        byte* opEnd = d + capacity;
        int ip = 0, anchor = 0, nextInsert = 0;
        int mfLimit = n - MfLimit, matchEnd = n - LastLiterals;

        while (ip <= mfLimit)
        {
            int length = FindLongest(s, head, prev, ref nextInsert, ip, matchEnd, attempts, out int match);
            if (length < MinMatch)
            {
                ip++;
                continue;
            }

            while (length < GoodEnough && ip + 1 <= mfLimit)
            {
                int next = FindLongest(s, head, prev, ref nextInsert, ip + 1, matchEnd, attempts, out int nextMatch);
                if (next > length)
                {
                    ip++;
                    length = next;
                    match = nextMatch;
                    continue;
                }
                if (ip + 2 > mfLimit)
                    break;
                next = FindLongest(s, head, prev, ref nextInsert, ip + 2, matchEnd, attempts, out nextMatch);
                if (next > length + 1)
                {
                    ip += 2;
                    length = next;
                    match = nextMatch;
                    continue;
                }
                break;
            }

            if (!EmitSequence(ref op, opEnd, s + anchor, ip - anchor, ip - match, length))
                return -1;
            ip += length;
            anchor = ip;
        }
        return EmitLastLiterals(ref op, opEnd, s + anchor, n - anchor) ? (int)(op - d) : -1;
    }

    /// <summary>
    /// Forward dynamic programming over the exact byte cost of the LZ4 format. Offsets always cost two bytes, so at
    /// each position only the longest match is needed: every shorter length can reuse its offset.
    /// </summary>
    private int CompressOptimal(byte* s, int n, byte* d, int capacity, ushort* head, ushort* prev, int attempts, int sufficient)
    {
        _price ??= new int[MaxBlockSize + 1];
        _run ??= new int[MaxBlockSize + 1];
        _length ??= new int[MaxBlockSize + 1];
        _distance ??= new ushort[MaxBlockSize + 1];
        _path ??= new int[MaxBlockSize / MinMatch + 1];

        fixed (int* price = _price, run = _run, length = _length, path = _path)
        fixed (ushort* distance = _distance)
        {
            new Span<int>(price, n + 1).Fill(int.MaxValue);
            price[0] = 0;
            run[0] = 0;
            length[0] = 0;

            int mfLimit = n - MfLimit, matchEnd = n - LastLiterals, nextInsert = 0;
            for (int cur = 0; cur < n; cur++)
            {
                int basePrice = price[cur];

                int literals = run[cur] + 1;
                int literalPrice = basePrice + 1 + (literals >= 15 && (literals - 15) % 255 == 0 ? 1 : 0);
                if (literalPrice < price[cur + 1])
                {
                    price[cur + 1] = literalPrice;
                    run[cur + 1] = literals;
                    length[cur + 1] = 0;
                }

                if (cur > mfLimit)
                    continue;

                int longest = FindLongest(s, head, prev, ref nextInsert, cur, matchEnd, attempts, out int match);
                if (longest < MinMatch)
                    continue;

                ushort dist = (ushort)(cur - match);
                if (longest >= sufficient)
                {
                    // Long enough that parsing inside it is not worth the time: this edge is the only way forward.
                    int target = cur + longest;
                    int matchPrice = basePrice + 3 + MatchLengthExtra(longest);
                    if (matchPrice < price[target])
                    {
                        price[target] = matchPrice;
                        run[target] = 0;
                        length[target] = longest;
                        distance[target] = dist;
                    }
                    cur = target - 1;
                    continue;
                }

                for (int l = MinMatch; l <= longest; l++)
                {
                    int target = cur + l;
                    int matchPrice = basePrice + 3 + MatchLengthExtra(l);
                    if (matchPrice < price[target])
                    {
                        price[target] = matchPrice;
                        run[target] = 0;
                        length[target] = l;
                        distance[target] = dist;
                    }
                }
            }

            int matches = 0;
            for (int pos = n; pos > 0;)
            {
                if (length[pos] == 0)
                {
                    pos--;
                }
                else
                {
                    path[matches++] = pos;
                    pos -= length[pos];
                }
            }

            byte* op = d;
            byte* opEnd = d + capacity;
            int anchor = 0;
            for (int i = matches - 1; i >= 0; i--)
            {
                int end = path[i];
                int start = end - length[end];
                if (!EmitSequence(ref op, opEnd, s + anchor, start - anchor, distance[end], length[end]))
                    return -1;
                anchor = end;
            }
            return EmitLastLiterals(ref op, opEnd, s + anchor, n - anchor) ? (int)(op - d) : -1;
        }
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    private static int MatchLengthExtra(int length)
    {
        int m = length - MinMatch;
        return m < 15 ? 0 : 1 + (m - 15) / 255;
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    private static uint Hash(uint sequence) => (sequence * 2654435761u) >> (32 - HashBits);

    private static int FindLongest(byte* s, ushort* head, ushort* prev, ref int nextInsert, int ip, int matchEnd, int attempts, out int match)
    {
        // Positions are stored +1 so that 0 can mean "空". The last 12 bytes are never inserted, so they fit in 16 bits.
        for (int i = nextInsert; i < ip; i++)
        {
            uint h = Hash(*(uint*)(s + i));
            prev[i] = head[h];
            head[h] = (ushort)(i + 1);
        }
        nextInsert = ip;

        byte* cur = s + ip;
        byte* limit = s + matchEnd;
        uint sequence = *(uint*)cur;
        int best = MinMatch - 1;
        match = -1;

        int candidate = head[Hash(sequence)];
        while (candidate != 0 && attempts-- > 0)
        {
            int c = candidate - 1;
            if (s[c + best] == cur[best] && *(uint*)(s + c) == sequence)
            {
                int length = MinMatch + CommonLength(cur + MinMatch, s + c + MinMatch, limit);
                if (length > best)
                {
                    best = length;
                    match = c;
                    if (cur + length >= limit)
                        break;
                }
            }
            candidate = prev[c];
        }
        return best >= MinMatch ? best : 0;
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    private static int CommonLength(byte* a, byte* b, byte* aEnd)
    {
        byte* start = a;
        while (a + 8 <= aEnd)
        {
            ulong x = *(ulong*)a ^ *(ulong*)b;
            if (x != 0)
                return (int)(a - start) + (BitOperations.TrailingZeroCount(x) >> 3);
            a += 8;
            b += 8;
        }
        while (a < aEnd && *a == *b)
        {
            a++;
            b++;
        }
        return (int)(a - start);
    }

    private static bool EmitSequence(ref byte* op, byte* opEnd, byte* literals, int literalCount, int distance, int matchLength)
    {
        if (op + 1 + literalCount / 255 + 1 + literalCount + 2 + matchLength / 255 + 1 > opEnd)
            return false;

        byte* token = op++;
        if (literalCount >= 15)
        {
            *token = 15 << 4;
            int r = literalCount - 15;
            for (; r >= 255; r -= 255)
                *op++ = 255;
            *op++ = (byte)r;
        }
        else
        {
            *token = (byte)(literalCount << 4);
        }

        Buffer.MemoryCopy(literals, op, literalCount, literalCount);
        op += literalCount;

        *op++ = (byte)distance;
        *op++ = (byte)(distance >> 8);

        int m = matchLength - MinMatch;
        if (m >= 15)
        {
            *token |= 15;
            m -= 15;
            for (; m >= 255; m -= 255)
                *op++ = 255;
            *op++ = (byte)m;
        }
        else
        {
            *token |= (byte)m;
        }
        return true;
    }

    private static bool EmitLastLiterals(ref byte* op, byte* opEnd, byte* literals, int count)
    {
        if (op + 1 + count / 255 + 1 + count > opEnd)
            return false;

        if (count >= 15)
        {
            *op++ = 15 << 4;
            int r = count - 15;
            for (; r >= 255; r -= 255)
                *op++ = 255;
            *op++ = (byte)r;
        }
        else
        {
            *op++ = (byte)(count << 4);
        }
        Buffer.MemoryCopy(literals, op, count, count);
        op += count;
        return true;
    }
}

internal static class Lz4Decoder
{
    /// <summary>Strict decoder with full bounds checks. Returns the decoded size, or -1 if the input is malformed.</summary>
    public static int Decode(byte[] src, int srcOffset, int srcLength, byte[] dst, int dstOffset, int dstCapacity)
    {
        int ip = srcOffset, ipEnd = srcOffset + srcLength;
        int op = dstOffset, opEnd = dstOffset + dstCapacity;

        while (ip < ipEnd)
        {
            int token = src[ip++];

            int literals = token >> 4;
            if (literals == 15)
            {
                int b;
                do
                {
                    if (ip >= ipEnd)
                        return -1;
                    b = src[ip++];
                    literals += b;
                } while (b == 255);
            }
            if (literals > ipEnd - ip || literals > opEnd - op)
                return -1;
            Buffer.BlockCopy(src, ip, dst, op, literals);
            ip += literals;
            op += literals;

            if (ip == ipEnd)
                return op - dstOffset;

            if (ipEnd - ip < 2)
                return -1;
            int distance = src[ip] | (src[ip + 1] << 8);
            ip += 2;
            if (distance == 0 || distance > op - dstOffset)
                return -1;

            int length = token & 15;
            if (length == 15)
            {
                int b;
                do
                {
                    if (ip >= ipEnd)
                        return -1;
                    b = src[ip++];
                    length += b;
                } while (b == 255);
            }
            length += 4;
            if (length > opEnd - op)
                return -1;

            int from = op - distance;
            if (distance >= length)
            {
                Buffer.BlockCopy(dst, from, dst, op, length);
                op += length;
            }
            else
            {
                for (int i = 0; i < length; i++)
                    dst[op++] = dst[from++];
            }
        }
        return -1;
    }
}
