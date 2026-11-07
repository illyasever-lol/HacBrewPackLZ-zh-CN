using System.Buffers.Binary;
using System.Text;
using Microsoft.Win32.SafeHandles;

namespace HacBrewPackLz;

/// <summary>
/// A game file opened for reading: an NSP or NSZ, which are PFS0 archives, or an XCI or XCZ, whose content is in the
/// secure partition of a cartridge image. The kind is worked out from the file itself. The file is never written to.
/// </summary>
internal sealed class GameArchive : IDisposable
{
    public sealed record Entry(string Name, long Offset, long Size);

    public const string DialogFilter = "*.nsp;*.nsz;*.xci;*.xcz";

    private const long CartridgeHeaderSize = 0x200;
    private const long CartridgeMagicOffset = 0x100;
    private const long CartridgePartitionOffset = 0x130;
    private const long KeyAreaSize = 0x1000;
    private const string SecurePartition = "secure";

    private const int Hfs0HeaderSize = 0x10;
    private const int Hfs0EntrySize = 0x40;
    private const int MaxEntries = 0x10000;

    private static readonly string[] Extensions = [".nsp", ".nsz", ".xci", ".xcz"];

    private readonly SafeFileHandle _handle;
    private readonly HandleSource _file;

    public string FilePath { get; }
    public long Length { get; }
    public List<Entry> Entries { get; } = [];

    public GameArchive(string path)
    {
        FilePath = path;

        // Sharing delete: a list of inputs in the console screen or the GUI shouldn't stop anyone tidying up their files.
        _handle = File.OpenHandle(path, FileMode.Open, FileAccess.Read, FileShare.Read | FileShare.Delete);
        Length = RandomAccess.GetLength(_handle);
        _file = new HandleSource(_handle, Length);
        try
        {
            ReadContents();
        }
        catch
        {
            _handle.Dispose();
            throw;
        }
    }

    public static bool IsGameFile(string path) => Extensions.Contains(Path.GetExtension(path), StringComparer.OrdinalIgnoreCase);

    public DataSource Open(Entry entry) => new SubSource(_file, entry.Offset, entry.Size);

    public Entry? Find(string name) =>
        Entries.FirstOrDefault(e => e.Name.Equals(name, StringComparison.OrdinalIgnoreCase));

    public void Dispose() => _handle.Dispose();

    private void ReadContents()
    {
        var magic = new byte[4];
        if (Length >= magic.Length)
        {
            _file.Read(0, magic, 0, magic.Length);
            if (magic.AsSpan().SequenceEqual("PFS0"u8))
            {
                ReadPartition();
                return;
            }
        }

        // Some cartridge dumps put a key area in front of the header.
        foreach (long header in new[] { 0, KeyAreaSize })
        {
            if (Length < header + CartridgeHeaderSize)
                continue;
            _file.Read(header + CartridgeMagicOffset, magic, 0, magic.Length);
            if (magic.AsSpan().SequenceEqual("HEAD"u8))
            {
                ReadCartridge(header);
                return;
            }
        }
        throw new ToolException($"'{FilePath}' is not an NSP, NSZ, XCI or XCZ");
    }

    private void ReadPartition()
    {
        var head = new byte[0x10];
        if (Length < head.Length)
            throw new ToolException($"'{FilePath}' is too small to be an NSP");
        _file.Read(0, head, 0, head.Length);

        int count = BinaryPrimitives.ReadInt32LittleEndian(head.AsSpan(4));
        int nameTable = BinaryPrimitives.ReadInt32LittleEndian(head.AsSpan(8));
        long tableSize = count * 0x18L + nameTable;
        if (count < 0 || count > MaxEntries || nameTable < 0 || 0x10 + tableSize > Length)
            throw new ToolException($"'{FilePath}' has a damaged NSP header");

        var table = new byte[tableSize];
        _file.Read(0x10, table, 0, table.Length);
        long dataStart = 0x10 + tableSize;
        var names = table.AsSpan(count * 0x18);
        for (int i = 0; i < count; i++)
        {
            var e = table.AsSpan(i * 0x18);
            long offset = BinaryPrimitives.ReadInt64LittleEndian(e);
            long size = BinaryPrimitives.ReadInt64LittleEndian(e[8..]);
            int nameOffset = BinaryPrimitives.ReadInt32LittleEndian(e[0x10..]);
            if (offset < 0 || size < 0 || dataStart + offset + size > Length || nameOffset < 0 || nameOffset >= nameTable)
                throw new ToolException($"'{FilePath}' has a damaged NSP entry");
            var name = names[nameOffset..];
            int end = name.IndexOf((byte)0);
            Entries.Add(new Entry(Encoding.UTF8.GetString(end >= 0 ? name[..end] : name), dataStart + offset, size));
        }
    }

    /// <summary>
    /// A cartridge's root partition holds other partitions. A game's content is all in the secure one, which can also
    /// hold its updates and DLC. The update partition is system firmware, and the others are of no use to an NSP.
    /// </summary>
    private void ReadCartridge(long header)
    {
        var offset = new byte[sizeof(long)];
        _file.Read(header + CartridgePartitionOffset, offset, 0, offset.Length);
        long root = header + BinaryPrimitives.ReadInt64LittleEndian(offset);
        if (root < 0 || root >= Length)
            throw new ToolException($"'{FilePath}' has a damaged cartridge header");

        // Only the secure partition is used, so it's the only one that has to be all there.
        var secure = ReadHfs0(root, mustFit: false).FirstOrDefault(e => e.Name == SecurePartition)
            ?? throw new ToolException($"'{FilePath}' has no secure partition");
        Entries.AddRange(ReadHfs0(secure.Offset, mustFit: true));
    }

    /// <summary>The entries of an HFS0 partition, with offsets from the start of the file.</summary>
    private List<Entry> ReadHfs0(long start, bool mustFit)
    {
        var head = new byte[Hfs0HeaderSize];
        if (start < 0 || start + Hfs0HeaderSize > Length)
            throw new ToolException($"'{FilePath}' is cut short: a partition lies outside the file");
        _file.Read(start, head, 0, head.Length);
        if (!head.AsSpan(0, 4).SequenceEqual("HFS0"u8))
            throw new ToolException($"'{FilePath}' has a damaged partition");

        int count = BinaryPrimitives.ReadInt32LittleEndian(head.AsSpan(4));
        int nameTable = BinaryPrimitives.ReadInt32LittleEndian(head.AsSpan(8));
        long tableSize = count * (long)Hfs0EntrySize + nameTable;
        if (count < 0 || count > MaxEntries || nameTable < 0 || start + Hfs0HeaderSize + tableSize > Length)
            throw new ToolException($"'{FilePath}' has a damaged partition");

        var table = new byte[tableSize];
        _file.Read(start + Hfs0HeaderSize, table, 0, table.Length);
        long dataStart = start + Hfs0HeaderSize + tableSize;
        var names = table.AsSpan(count * Hfs0EntrySize);
        var entries = new List<Entry>(count);
        for (int i = 0; i < count; i++)
        {
            var e = table.AsSpan(i * Hfs0EntrySize);
            long offset = BinaryPrimitives.ReadInt64LittleEndian(e);
            long size = BinaryPrimitives.ReadInt64LittleEndian(e[8..]);
            int nameOffset = BinaryPrimitives.ReadInt32LittleEndian(e[0x10..]);
            if (offset < 0 || size < 0 || nameOffset < 0 || nameOffset >= nameTable)
                throw new ToolException($"'{FilePath}' has a damaged partition");
            if (mustFit && dataStart + offset + size > Length)
                throw new ToolException($"'{FilePath}' is cut short: a file in its partition lies outside the file");
            var name = names[nameOffset..];
            int end = name.IndexOf((byte)0);
            entries.Add(new Entry(Encoding.UTF8.GetString(end >= 0 ? name[..end] : name), dataStart + offset, size));
        }
        return entries;
    }
}
