using System.Buffers.Binary;
using System.Security.Cryptography;
using System.Text;

namespace HacBrewPackLz;

internal sealed class Pfs0Archive
{
    public sealed record Entry(string Name, long Offset, long Size);

    public DataSource Data { get; }
    public int HashBlockSize { get; }
    public List<Entry> Files { get; } = [];

    private Pfs0Archive(DataSource data, int hashBlockSize)
    {
        Data = data;
        HashBlockSize = hashBlockSize;
    }

    public DataSource Open(Entry entry) => new SubSource(Data, entry.Offset, entry.Size);

    public byte[] ReadAll(Entry entry)
    {
        var bytes = new byte[entry.Size];
        Data.Read(entry.Offset, bytes, 0, bytes.Length);
        return bytes;
    }

    public static Pfs0Archive Open(DataSource logical, NcaFsSection section, string what, bool verify = true)
    {
        if (section.FsType != NcaFsSection.FsPartition)
            throw new ToolException($"{what} is not a PFS0");
        if (section.HashType is not (NcaFsSection.HashSha256 or NcaFsSection.HashSha3))
            throw new ToolException($"{what} has a PFS0 with hash type {section.HashType}, which isn't supported");

        var h = section.Header.AsSpan();
        int blockSize = BinaryPrimitives.ReadInt32LittleEndian(h[0x28..]);
        long tableOffset = BinaryPrimitives.ReadInt64LittleEndian(h[0x30..]);
        long tableSize = BinaryPrimitives.ReadInt64LittleEndian(h[0x38..]);
        long dataOffset = BinaryPrimitives.ReadInt64LittleEndian(h[0x40..]);
        long dataSize = BinaryPrimitives.ReadInt64LittleEndian(h[0x48..]);
        if (blockSize <= 0 || tableOffset < 0 || tableSize < 0 || tableOffset + tableSize > logical.Length || dataOffset < 0 || dataSize < 0x10 || dataOffset + dataSize > logical.Length)
            throw new ToolException($"{what} has an invalid PFS0 layout");

        var data = new SubSource(logical, dataOffset, dataSize);
        if (verify && section.HashType == NcaFsSection.HashSha256)
        {
            var table = new byte[tableSize];
            logical.Read(tableOffset, table, 0, table.Length);
            if (!SHA256.HashData(table).AsSpan().SequenceEqual(h.Slice(0x08, 0x20)))
                throw new ToolException($"{what}: PFS0 master hash check failed, the file is damaged");
            CheckPartialBlocks(data, table, blockSize, what);
        }

        var archive = new Pfs0Archive(data, blockSize);
        archive.ReadEntries(what);
        return archive;
    }

    /// <summary>Unlike IVFC, a short last block in a PFS0 hash table is hashed at its real length.</summary>
    private static void CheckPartialBlocks(DataSource data, byte[] table, int blockSize, string what)
    {
        var buffer = new byte[blockSize];
        long blocks = (data.Length + blockSize - 1) / blockSize;
        if (blocks * 0x20 > table.Length)
            throw new ToolException($"{what}: the PFS0 hash table is too short, the file is damaged");
        Span<byte> hash = stackalloc byte[0x20];
        for (long i = 0; i < blocks; i++)
        {
            int n = (int)Math.Min(blockSize, data.Length - i * blockSize);
            data.Read(i * blockSize, buffer, 0, n);
            SHA256.HashData(buffer.AsSpan(0, n), hash);
            if (!hash.SequenceEqual(table.AsSpan((int)(i * 0x20), 0x20)))
                throw new ToolException($"{what}: PFS0 hash check failed at offset 0x{i * blockSize:x}, the file is damaged");
        }
    }

    private void ReadEntries(string what)
    {
        var head = new byte[0x10];
        Data.Read(0, head, 0, head.Length);
        if (!head.AsSpan(0, 4).SequenceEqual("PFS0"u8))
            throw new ToolException($"{what} doesn't start with a PFS0 header");
        int count = BinaryPrimitives.ReadInt32LittleEndian(head.AsSpan(4));
        int nameTable = BinaryPrimitives.ReadInt32LittleEndian(head.AsSpan(8));
        long tableSize = count * 0x18L + nameTable;
        if (count < 0 || nameTable < 0 || 0x10 + tableSize > Data.Length)
            throw new ToolException($"{what} has a damaged PFS0 header");

        var table = new byte[tableSize];
        Data.Read(0x10, table, 0, table.Length);
        long dataStart = 0x10 + tableSize;
        var names = table.AsSpan(count * 0x18);
        for (int i = 0; i < count; i++)
        {
            var e = table.AsSpan(i * 0x18);
            long offset = BinaryPrimitives.ReadInt64LittleEndian(e);
            long size = BinaryPrimitives.ReadInt64LittleEndian(e[8..]);
            int nameOffset = BinaryPrimitives.ReadInt32LittleEndian(e[0x10..]);
            if (offset < 0 || size < 0 || dataStart + offset + size > Data.Length || nameOffset < 0 || nameOffset >= nameTable)
                throw new ToolException($"{what} has a damaged PFS0 entry");
            var name = names[nameOffset..];
            int end = name.IndexOf((byte)0);
            Files.Add(new Entry(LocalNames.CheckEntryName(Encoding.UTF8.GetString(end >= 0 ? name[..end] : name), what), dataStart + offset, size));
        }
    }
}

internal sealed class RomFsArchive
{
    private const uint Empty = 0xFFFFFFFF;
    private const long MaxTableSize = 1L << 30;

    public sealed record Entry(string Path, long Offset, long Size);

    public DataSource Image { get; }
    public List<string> Directories { get; } = [];
    public List<Entry> Files { get; } = [];

    private RomFsArchive(DataSource image) => Image = image;

    public static RomFsArchive Open(DataSource image, string what)
    {
        var head = new byte[0x50];
        if (image.Length < head.Length)
            throw new ToolException($"{what} is too small to hold a RomFS");
        image.Read(0, head, 0, head.Length);
        long Field(int i) => BinaryPrimitives.ReadInt64LittleEndian(head.AsSpan(i * 8));
        if (Field(0) != 0x50)
            throw new ToolException($"{what} doesn't start with a RomFS header");

        var dirs = ReadTable(image, Field(3), Field(4), what);
        var files = ReadTable(image, Field(7), Field(8), what);
        long partition = Field(9);

        var archive = new RomFsArchive(image);
        var seenDirs = new HashSet<uint>();
        var seenFiles = new HashSet<uint>();
        var pending = new Stack<(uint offset, string path)>();
        pending.Push((0, ""));
        while (pending.Count > 0)
        {
            var (dir, path) = pending.Pop();
            if (!seenDirs.Add(dir))
                throw new ToolException($"{what} has a loop in its directory table");

            for (uint file = Field32(dirs, dir, 0x0C, what); file != Empty; file = Field32(files, file, 0x04, what))
            {
                if (!seenFiles.Add(file))
                    throw new ToolException($"{what} has a loop in its file table");
                string name = Name(files, file, 0x1C, 0x20, what);
                long offset = BinaryPrimitives.ReadInt64LittleEndian(Slice(files, file + 0x08L, 8, what));
                long size = BinaryPrimitives.ReadInt64LittleEndian(Slice(files, file + 0x10L, 8, what));
                if (offset < 0 || size < 0 || partition + offset + size > image.Length)
                    throw new ToolException($"{what}: file {path}{name} lies outside the RomFS");
                archive.Files.Add(new Entry(path + name, partition + offset, size));
            }

            for (uint child = Field32(dirs, dir, 0x08, what); child != Empty; child = Field32(dirs, child, 0x04, what))
            {
                string childPath = path + Name(dirs, child, 0x14, 0x18, what) + "/";
                archive.Directories.Add(childPath.TrimEnd('/'));
                pending.Push((child, childPath));
            }
        }
        return archive;
    }

    private static byte[] ReadTable(DataSource image, long offset, long size, string what)
    {
        if (offset < 0 || size < 0 || size > MaxTableSize || offset + size > image.Length)
            throw new ToolException($"{what} has a damaged RomFS header");
        var table = new byte[size];
        image.Read(offset, table, 0, table.Length);
        return table;
    }

    private static uint Field32(byte[] table, uint entry, int field, string what) =>
        BinaryPrimitives.ReadUInt32LittleEndian(Slice(table, (long)entry + field, 4, what));

    private static string Name(byte[] table, uint entry, int sizeField, int nameField, string what)
    {
        uint size = Field32(table, entry, sizeField, what);
        return LocalNames.CheckEntryName(Encoding.UTF8.GetString(Slice(table, (long)entry + nameField, size, what)), what);
    }

    private static ReadOnlySpan<byte> Slice(byte[] table, long offset, long size, string what)
    {
        if (offset < 0 || size < 0 || offset + size > table.Length)
            throw new ToolException($"{what} has a damaged RomFS table");
        return table.AsSpan((int)offset, (int)size);
    }
}
