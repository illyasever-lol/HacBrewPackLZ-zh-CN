using System.Buffers.Binary;

namespace HacBrewPackLz;

internal enum CompressionType : byte
{
    None = 0,
    Zeros = 1,
    Lz4 = 3,
}

internal struct CompressionEntry
{
    public long VirtualOffset;
    public long PhysicalOffset;
    public CompressionType Type;
    public int PhysicalSize;
}

/// <summary>
/// The BKTR table of an NCA compression layer: a 16 KiB node tree (L1, optional L2) over 16 KiB entry sets of
/// 0x18 byte entries. Geometry matches fs::CompressedStorage and fssystem::BucketTree.
/// </summary>
internal static class BucketTree
{
    public const int NodeSize = 0x4000;
    public const int EntrySize = 0x18;
    public const int NodeHeaderSize = 0x10;
    public const int EntriesPerSet = (NodeSize - NodeHeaderSize) / EntrySize;
    public const int OffsetsPerNode = (NodeSize - NodeHeaderSize) / sizeof(long);
    public const uint Magic = 0x52544B42; // "BKTR"
    public const uint Version = 1;

    public delegate void EntryVisitor(int index, ReadOnlySpan<byte> entry);

    public static int EntrySetCount(int entries, int entrySize = EntrySize) =>
        DivideUp(entries, (NodeSize - NodeHeaderSize) / entrySize);

    public static int L2NodeCount(int entries, int entrySize = EntrySize)
    {
        int sets = EntrySetCount(entries, entrySize);
        if (sets <= OffsetsPerNode)
            return 0;
        int estimate = DivideUp(sets, OffsetsPerNode);
        if (estimate > OffsetsPerNode)
            throw new ToolException("RomFS 需要的压缩表项过多，请增大 --blocksize");
        return DivideUp(sets - (OffsetsPerNode - (estimate - 1)), OffsetsPerNode);
    }

    public static long NodeStorageSize(int entries, int entrySize = EntrySize) =>
        (1 + L2NodeCount(entries, entrySize)) * (long)NodeSize;

    public static long EntryStorageSize(int entries, int entrySize = EntrySize) =>
        EntrySetCount(entries, entrySize) * (long)NodeSize;

    /// <summary>Checks the 0x10 byte header stored in an NCA FS header and returns its entry count.</summary>
    public static int ReadHeader(ReadOnlySpan<byte> header, string what)
    {
        if (BinaryPrimitives.ReadUInt32LittleEndian(header) != Magic || BinaryPrimitives.ReadUInt32LittleEndian(header[4..]) > Version)
            throw new ToolException($"{what} has an invalid BKTR table header");
        int entries = BinaryPrimitives.ReadInt32LittleEndian(header[8..]);
        if (entries <= 0)
            throw new ToolException($"{what} has an empty BKTR table");
        return entries;
    }

    /// <summary>
    /// Visits every entry of a table of any entry size, in order, and returns the table's end offset. Retail tables can
    /// be padded larger than they need to be, so only the node and entry storage sizes are trusted.
    /// </summary>
    public static long EnumerateEntries(byte[] table, int entrySize, int entryCount, string what, EntryVisitor visit)
    {
        long entryStorage = NodeStorageSize(entryCount, entrySize);
        int sets = EntrySetCount(entryCount, entrySize);
        if (entryStorage + sets * (long)NodeSize > table.Length)
            throw new ToolException($"{what} has a BKTR table smaller than its entry count needs");

        int perSet = (NodeSize - NodeHeaderSize) / entrySize;
        int seen = 0;
        long end = 0;
        for (int set = 0; set < sets; set++)
        {
            var node = table.AsSpan((int)(entryStorage + set * (long)NodeSize), NodeSize);
            int index = BinaryPrimitives.ReadInt32LittleEndian(node);
            int count = BinaryPrimitives.ReadInt32LittleEndian(node[4..]);
            long setEnd = BinaryPrimitives.ReadInt64LittleEndian(node[8..]);
            if (index != set || count <= 0 || count > perSet || seen + count > entryCount || setEnd < end)
                throw new ToolException($"{what} has a damaged BKTR entry set");
            for (int i = 0; i < count; i++)
                visit(seen + i, node.Slice(NodeHeaderSize + i * entrySize, entrySize));
            seen += count;
            end = setEnd;
        }
        if (seen != entryCount)
            throw new ToolException($"{what} has {seen} BKTR entries, its header says {entryCount}");
        return end;
    }

    /// <summary>Returns node storage followed by entry storage.</summary>
    public static byte[] Build(List<CompressionEntry> entries, long virtualSize)
    {
        int sets = EntrySetCount(entries.Count);
        int l2Nodes = L2NodeCount(entries.Count);
        var table = new byte[checked(NodeStorageSize(entries.Count) + EntryStorageSize(entries.Count))];
        var entryStorage = table.AsSpan((1 + l2Nodes) * NodeSize);

        var setStarts = new long[sets];
        for (int set = 0; set < sets; set++)
        {
            int first = set * EntriesPerSet;
            int count = Math.Min(EntriesPerSet, entries.Count - first);
            long end = first + count < entries.Count ? entries[first + count].VirtualOffset : virtualSize;
            setStarts[set] = entries[first].VirtualOffset;

            var node = entryStorage.Slice(set * NodeSize, NodeSize);
            WriteNodeHeader(node, set, count, end);
            for (int i = 0; i < count; i++)
            {
                var e = node[(NodeHeaderSize + i * EntrySize)..];
                var entry = entries[first + i];
                BinaryPrimitives.WriteInt64LittleEndian(e, entry.VirtualOffset);
                BinaryPrimitives.WriteInt64LittleEndian(e[8..], entry.PhysicalOffset);
                e[0x10] = (byte)entry.Type;
                BinaryPrimitives.WriteInt32LittleEndian(e[0x14..], entry.PhysicalSize);
            }
        }

        var l1 = table.AsSpan(0, NodeSize);
        if (l2Nodes == 0)
        {
            WriteNodeHeader(l1, 0, sets, virtualSize);
            for (int set = 0; set < sets; set++)
                PutOffset(l1, set, setStarts[set]);
            return table;
        }

        // L1 starts with one offset per L2 node; its remaining slots point straight at the first entry sets.
        int setsOnL1 = OffsetsPerNode - l2Nodes;
        WriteNodeHeader(l1, 0, l2Nodes, virtualSize);
        for (int i = 0; i < setsOnL1; i++)
            PutOffset(l1, l2Nodes + i, setStarts[i]);

        for (int n = 0; n < l2Nodes; n++)
        {
            int first = setsOnL1 + n * OffsetsPerNode;
            int count = Math.Min(OffsetsPerNode, sets - first);
            long end = first + count < sets ? setStarts[first + count] : virtualSize;
            var node = table.AsSpan((1 + n) * NodeSize, NodeSize);
            WriteNodeHeader(node, n, count, end);
            for (int i = 0; i < count; i++)
                PutOffset(node, i, setStarts[first + i]);
            PutOffset(l1, n, setStarts[first]);
        }
        return table;
    }

    /// <summary>The 0x10 byte header stored in the NCA FS header.</summary>
    public static void WriteHeader(Span<byte> destination, int entries)
    {
        BinaryPrimitives.WriteUInt32LittleEndian(destination, Magic);
        BinaryPrimitives.WriteUInt32LittleEndian(destination[4..], Version);
        BinaryPrimitives.WriteInt32LittleEndian(destination[8..], entries);
        BinaryPrimitives.WriteInt32LittleEndian(destination[12..], 0);
    }

    private static void WriteNodeHeader(Span<byte> node, int index, int count, long endOffset)
    {
        BinaryPrimitives.WriteInt32LittleEndian(node, index);
        BinaryPrimitives.WriteInt32LittleEndian(node[4..], count);
        BinaryPrimitives.WriteInt64LittleEndian(node[8..], endOffset);
    }

    private static void PutOffset(Span<byte> node, int index, long offset) =>
        BinaryPrimitives.WriteInt64LittleEndian(node[(NodeHeaderSize + index * sizeof(long))..], offset);

    private static int DivideUp(int value, int divisor) => (value + divisor - 1) / divisor;
}

/// <summary>Reads a BKTR table back with the same lookups and validation order as fssystem::BucketTree.</summary>
internal sealed class BucketTreeReader
{
    private readonly byte[] _table;
    private readonly int _nodeStorageSize;
    private readonly int _sets;
    private readonly int _l1Count;
    private readonly long _endOffset;

    public int EntryCount { get; }
    public long VirtualSize => _endOffset;

    public BucketTreeReader(byte[] table, int entryCount)
    {
        if (entryCount <= 0)
            throw new ToolException("压缩表没有条目");
        long needed = BucketTree.NodeStorageSize(entryCount) + BucketTree.EntryStorageSize(entryCount);
        if (needed > table.Length)
            throw new ToolException("压缩表小于其条目数所要求的大小");

        _table = table;
        EntryCount = entryCount;
        _nodeStorageSize = (int)BucketTree.NodeStorageSize(entryCount);
        _sets = BucketTree.EntrySetCount(entryCount);

        var l1 = table.AsSpan(0, BucketTree.NodeSize);
        (_l1Count, _endOffset) = ReadNodeHeader(l1, 0, sizeof(long));

        long start = HasOffsetsAfterL2 ? Offset(l1, _l1Count) : Offset(l1, 0);
        long begin = Offset(l1, 0);
        if (start < 0 || start > begin || start >= _endOffset)
            throw new ToolException("压缩表 L1 偏移无效");
    }

    private bool HasL2 => BucketTree.OffsetsPerNode < _sets;
    private bool HasOffsetsAfterL2 => HasL2 && _l1Count < BucketTree.OffsetsPerNode;

    /// <summary>Finds the entry holding <paramref name="virtualAddress"/> by walking the tree top down.</summary>
    public (CompressionEntry entry, long entryEnd) Find(long virtualAddress)
    {
        if (virtualAddress < 0 || virtualAddress >= _endOffset)
            throw new ToolException($"Virtual address 0x{virtualAddress:x} is outside the compression table");

        var l1 = _table.AsSpan(0, BucketTree.NodeSize);
        int set;
        if (HasOffsetsAfterL2 && virtualAddress < Offset(l1, 0))
        {
            int position = UpperBound(l1, _l1Count, BucketTree.OffsetsPerNode, virtualAddress);
            if (position <= _l1Count)
                throw new ToolException("压缩表 L1 查找失败");
            set = position - 1 - _l1Count;
        }
        else
        {
            int position = UpperBound(l1, 0, _l1Count, virtualAddress);
            if (position <= 0)
                throw new ToolException("压缩表 L1 查找失败");
            int index = position - 1;
            if (HasL2)
            {
                var l2 = _table.AsSpan((index + 1) * BucketTree.NodeSize, BucketTree.NodeSize);
                var (count, end) = ReadNodeHeader(l2, index, sizeof(long));
                if (virtualAddress >= end)
                    throw new ToolException("压缩表 L2 结束偏移错误");
                int inner = UpperBound(l2, 0, count, virtualAddress);
                if (inner <= 0)
                    throw new ToolException("压缩表 L2 查找失败");
                set = (BucketTree.OffsetsPerNode - _l1Count) + BucketTree.OffsetsPerNode * index + inner - 1;
            }
            else
            {
                set = index;
            }
        }

        if (set < 0 || set >= _sets)
            throw new ToolException("压缩表条目集索引超出范围");

        var node = _table.AsSpan(_nodeStorageSize + set * BucketTree.NodeSize, BucketTree.NodeSize);
        var (entryCount, setEnd) = ReadNodeHeader(node, set, BucketTree.EntrySize);
        if (virtualAddress >= setEnd)
            throw new ToolException("压缩表条目集结束偏移错误");

        int low = 0, high = entryCount;
        while (low < high)
        {
            int mid = (low + high) / 2;
            if (ReadEntry(node, mid).VirtualOffset <= virtualAddress)
                low = mid + 1;
            else
                high = mid;
        }
        if (low == 0)
            throw new ToolException("压缩表条目查找失败");
        var entry = ReadEntry(node, low - 1);
        long entryEnd = low < entryCount ? ReadEntry(node, low).VirtualOffset : setEnd;
        return (entry, entryEnd);
    }

    /// <summary>Walks every entry in order, checking the links between entry sets the way Visitor::MoveNext does.</summary>
    public IEnumerable<(CompressionEntry entry, long entryEnd)> EnumerateAll()
    {
        long previousEnd = 0;
        int seen = 0;
        for (int set = 0; set < _sets; set++)
        {
            var (count, end) = ReadNodeHeaderAt(set);
            if (ReadEntryAt(set, 0).VirtualOffset != previousEnd)
                throw new ToolException($"Compression table entry set {set} does not start where the previous one ended");
            for (int i = 0; i < count; i++)
            {
                var entry = ReadEntryAt(set, i);
                long entryEnd = i + 1 < count ? ReadEntryAt(set, i + 1).VirtualOffset : end;
                if (entryEnd <= entry.VirtualOffset)
                    throw new ToolException($"Compression table entry at 0x{entry.VirtualOffset:x} has no size");
                seen++;
                yield return (entry, entryEnd);
            }
            previousEnd = end;
        }
        if (seen != EntryCount)
            throw new ToolException($"Compression table holds {seen} entries, its header says {EntryCount}");
        if (previousEnd != _endOffset)
            throw new ToolException("压缩表结束偏移不一致");
    }

    private (int count, long end) ReadNodeHeaderAt(int set) =>
        ReadNodeHeader(_table.AsSpan(_nodeStorageSize + set * BucketTree.NodeSize, BucketTree.NodeSize), set, BucketTree.EntrySize);

    private CompressionEntry ReadEntryAt(int set, int index) =>
        ReadEntry(_table.AsSpan(_nodeStorageSize + set * BucketTree.NodeSize, BucketTree.NodeSize), index);

    private static (int count, long end) ReadNodeHeader(ReadOnlySpan<byte> node, int expectedIndex, int itemSize)
    {
        int index = BinaryPrimitives.ReadInt32LittleEndian(node);
        int count = BinaryPrimitives.ReadInt32LittleEndian(node[4..]);
        long end = BinaryPrimitives.ReadInt64LittleEndian(node[8..]);
        int max = (BucketTree.NodeSize - BucketTree.NodeHeaderSize) / itemSize;
        if (index != expectedIndex || count <= 0 || count > max || end < 0)
            throw new ToolException($"Compression table node {expectedIndex} has an invalid header");
        return (count, end);
    }

    private static CompressionEntry ReadEntry(ReadOnlySpan<byte> node, int index)
    {
        var e = node[(BucketTree.NodeHeaderSize + index * BucketTree.EntrySize)..];
        return new CompressionEntry
        {
            VirtualOffset = BinaryPrimitives.ReadInt64LittleEndian(e),
            PhysicalOffset = BinaryPrimitives.ReadInt64LittleEndian(e[8..]),
            Type = (CompressionType)e[0x10],
            PhysicalSize = BinaryPrimitives.ReadInt32LittleEndian(e[0x14..]),
        };
    }

    private static long Offset(ReadOnlySpan<byte> node, int index) =>
        BinaryPrimitives.ReadInt64LittleEndian(node[(BucketTree.NodeHeaderSize + index * sizeof(long))..]);

    private static int UpperBound(ReadOnlySpan<byte> node, int first, int last, long value)
    {
        while (first < last)
        {
            int mid = (first + last) / 2;
            if (Offset(node, mid) <= value)
                first = mid + 1;
            else
                last = mid;
        }
        return first;
    }
}
