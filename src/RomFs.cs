using System.Buffers.Binary;
using System.Text;

namespace HacBrewPackLz;

/// <summary>
/// Describes a directory as a RomFS image without copying anything: the image is a chain of table bytes and
/// source file ranges. Layout and ordering follow hacBrewPack, whose output is known to work on hardware.
/// </summary>
internal static class RomFsBuilder
{
    private const uint Empty = 0xFFFFFFFF;
    private const long FilePartitionOffset = 0x200;

    private sealed class DirNode
    {
        public required byte[] Name;
        public required byte[] SortKey;
        public required string HostPath;
        public DirNode? Parent;
        public DirNode? Sibling;
        public DirNode? FirstChild;
        public FileNode? FirstFile;
        public uint EntryOffset;
    }

    private sealed class FileNode
    {
        public required byte[] Name;
        public required byte[] SortKey;
        public required string HostPath;
        public required long Size;
        public required DirNode Parent;
        public byte[]? Replacement;
        public FileNode? Sibling;
        public uint EntryOffset;
        public long Offset;
    }

    private static readonly Comparison<byte[]> KeyOrder = (a, b) => a.AsSpan().SequenceCompareTo(b);

    /// <param name="replacements">In-memory contents to use instead of the file on disk, keyed by top level file name.</param>
    public static DataSource Build(string directory, IReadOnlyDictionary<string, byte[]>? replacements = null)
    {
        var root = new DirNode { Name = [], SortKey = [], HostPath = directory };
        var dirs = new List<DirNode>();
        var files = new List<FileNode>();
        Visit(root, dirs, files, replacements);

        dirs.Sort((a, b) => KeyOrder(a.SortKey, b.SortKey));
        files.Sort((a, b) => KeyOrder(a.SortKey, b.SortKey));
        dirs.Insert(0, root);

        uint entryOffset = 0;
        long partitionSize = 0;
        foreach (var file in files)
        {
            partitionSize = Align(partitionSize, 0x10);
            file.Offset = partitionSize;
            partitionSize += file.Size;
            file.EntryOffset = entryOffset;
            entryOffset += checked(0x20u + (uint)Align(file.Name.Length, 4));
        }
        uint fileTableSize = entryOffset;

        entryOffset = 0;
        foreach (var dir in dirs)
        {
            dir.EntryOffset = entryOffset;
            entryOffset += checked(0x18u + (uint)Align(dir.Name.Length, 4));
        }
        uint dirTableSize = entryOffset;

        var dirHash = NewHashTable(dirs.Count);
        var fileHash = NewHashTable(files.Count);
        var dirTable = new byte[dirTableSize];
        var fileTable = new byte[fileTableSize];

        foreach (var file in files)
        {
            var e = fileTable.AsSpan((int)file.EntryOffset);
            uint hash = PathHash(file.Parent.EntryOffset, file.Name);
            uint bucket = hash % (uint)fileHash.Length;
            Put32(e, 0x00, file.Parent.EntryOffset);
            Put32(e, 0x04, file.Sibling?.EntryOffset ?? Empty);
            BinaryPrimitives.WriteInt64LittleEndian(e[0x08..], file.Offset);
            BinaryPrimitives.WriteInt64LittleEndian(e[0x10..], file.Size);
            Put32(e, 0x18, fileHash[bucket]);
            Put32(e, 0x1C, (uint)file.Name.Length);
            file.Name.CopyTo(e[0x20..]);
            fileHash[bucket] = file.EntryOffset;
        }

        foreach (var dir in dirs)
        {
            var e = dirTable.AsSpan((int)dir.EntryOffset);
            uint parent = dir.Parent?.EntryOffset ?? 0;
            uint hash = PathHash(parent, dir.Name);
            uint bucket = hash % (uint)dirHash.Length;
            Put32(e, 0x00, parent);
            Put32(e, 0x04, dir.Sibling?.EntryOffset ?? Empty);
            Put32(e, 0x08, dir.FirstChild?.EntryOffset ?? Empty);
            Put32(e, 0x0C, dir.FirstFile?.EntryOffset ?? Empty);
            Put32(e, 0x10, dirHash[bucket]);
            Put32(e, 0x14, (uint)dir.Name.Length);
            dir.Name.CopyTo(e[0x18..]);
            dirHash[bucket] = dir.EntryOffset;
        }

        long dirHashOffset = Align(partitionSize + FilePartitionOffset, 4);
        long dirHashSize = dirHash.Length * 4L;
        long dirTableOffset = dirHashOffset + dirHashSize;
        long fileHashOffset = dirTableOffset + dirTableSize;
        long fileHashSize = fileHash.Length * 4L;
        long fileTableOffset = fileHashOffset + fileHashSize;

        var header = new byte[FilePartitionOffset];
        var h = header.AsSpan();
        long[] fields =
        [
            0x50, dirHashOffset, dirHashSize, dirTableOffset, dirTableSize,
            fileHashOffset, fileHashSize, fileTableOffset, fileTableSize, FilePartitionOffset,
        ];
        for (int i = 0; i < fields.Length; i++)
            BinaryPrimitives.WriteInt64LittleEndian(h[(i * 8)..], fields[i]);

        var image = new ConcatSource().Add(new BytesSource(header));
        long position = 0;
        foreach (var file in files)
        {
            image.Add(new ZeroSource(file.Offset - position));
            image.Add(file.Replacement is { } data ? new BytesSource(data) : new FileSource(file.HostPath, file.Size));
            position = file.Offset + file.Size;
        }
        image.Add(new ZeroSource(dirHashOffset - FilePartitionOffset - position));
        image.Add(new BytesSource(ToBytes(dirHash)));
        image.Add(new BytesSource(dirTable));
        image.Add(new BytesSource(ToBytes(fileHash)));
        image.Add(new BytesSource(fileTable));
        return image;
    }

    private static void Visit(DirNode dir, List<DirNode> allDirs, List<FileNode> allFiles, IReadOnlyDictionary<string, byte[]>? replacements)
    {
        var childDirs = new List<DirNode>();
        var childFiles = new List<FileNode>();
        var info = new DirectoryInfo(dir.HostPath);
        foreach (var entry in info.EnumerateFileSystemInfos())
        {
            var name = Encoding.UTF8.GetBytes(entry.Name);
            byte[] key = [.. dir.SortKey, (byte)'\\', .. name];
            if (entry is DirectoryInfo)
            {
                childDirs.Add(new DirNode { Name = name, SortKey = key, HostPath = entry.FullName, Parent = dir });
            }
            else
            {
                byte[]? replacement = null;
                if (dir.Parent is null)
                    replacements?.TryGetValue(entry.Name, out replacement);
                long size = replacement?.Length ?? ((FileInfo)entry).Length;
                childFiles.Add(new FileNode
                {
                    Name = name, SortKey = key, HostPath = entry.FullName, Size = size, Parent = dir, Replacement = replacement,
                });
            }
        }

        childDirs.Sort((a, b) => KeyOrder(a.SortKey, b.SortKey));
        childFiles.Sort((a, b) => KeyOrder(a.SortKey, b.SortKey));
        for (int i = 0; i + 1 < childDirs.Count; i++)
            childDirs[i].Sibling = childDirs[i + 1];
        for (int i = 0; i + 1 < childFiles.Count; i++)
            childFiles[i].Sibling = childFiles[i + 1];
        dir.FirstChild = childDirs.Count > 0 ? childDirs[0] : null;
        dir.FirstFile = childFiles.Count > 0 ? childFiles[0] : null;

        allDirs.AddRange(childDirs);
        allFiles.AddRange(childFiles);
        foreach (var child in childDirs)
            Visit(child, allDirs, allFiles, replacements);
    }

    public static uint PathHash(uint parent, ReadOnlySpan<byte> name)
    {
        uint hash = parent ^ 123456789;
        foreach (byte c in name)
        {
            hash = (hash >> 5) | (hash << 27);
            hash ^= c;
        }
        return hash;
    }

    private static uint[] NewHashTable(int entries)
    {
        uint count;
        if (entries < 3)
        {
            count = 3;
        }
        else if (entries < 19)
        {
            count = (uint)entries | 1;
        }
        else
        {
            count = (uint)entries;
            while (count % 2 == 0 || count % 3 == 0 || count % 5 == 0 || count % 7 == 0 || count % 11 == 0 || count % 13 == 0 || count % 17 == 0)
                count++;
        }
        var table = new uint[count];
        Array.Fill(table, Empty);
        return table;
    }

    private static byte[] ToBytes(uint[] values)
    {
        var bytes = new byte[values.Length * 4];
        for (int i = 0; i < values.Length; i++)
            BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(i * 4), values[i]);
        return bytes;
    }

    private static void Put32(Span<byte> span, int offset, uint value) =>
        BinaryPrimitives.WriteUInt32LittleEndian(span[offset..], value);

    private static long Align(long value, long alignment) => (value + alignment - 1) & ~(alignment - 1);
}
