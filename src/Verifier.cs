using System.Buffers.Binary;
using System.Security.Cryptography;
using System.Text;

namespace HacBrewPackLz;

/// <summary>
/// Re-reads a finished NSP from its bytes alone: NCA ids, headers, section hashes, the whole IVFC tree, every
/// compression table entry under the same rules as fs::CompressedStorage, every decompressed block, the RomFS tables
/// (through their hash buckets) and the content meta. With source directories it compares file contents byte for byte.
/// </summary>
internal sealed class Verifier
{
    private readonly FileStream _nsp;
    private readonly Keyset _keyset;
    private readonly string? _romfsDir;
    private readonly string? _exefsDir;
    private readonly Dictionary<string, (long offset, long size)> _files = new(StringComparer.OrdinalIgnoreCase);
    private readonly Dictionary<string, byte[]> _hashes = new(StringComparer.OrdinalIgnoreCase);
    private readonly List<(byte[] hash, byte[] id, long size, byte type)> _records = [];
    private int _failures;

    private Verifier(FileStream nsp, Keyset keyset, string? romfsDir, string? exefsDir)
    {
        _nsp = nsp;
        _keyset = keyset;
        _romfsDir = romfsDir;
        _exefsDir = exefsDir;
    }

    public static int Run(string[] args)
    {
        string? keyset = null, file = null, romfs = null, exefs = null;
        for (int i = 0; i < args.Length; i++)
        {
            switch (args[i])
            {
                case "-k" or "--keyset" when i + 1 < args.Length: keyset = args[++i]; break;
                case "--romfsdir" when i + 1 < args.Length: romfs = args[++i]; break;
                case "--exefsdir" when i + 1 < args.Length: exefs = args[++i]; break;
                default:
                    if (args[i].StartsWith('-') || file is not null)
                        throw new ToolException($"Unexpected verify argument {args[i]}");
                    file = args[i];
                    break;
            }
        }
        if (file is null)
            throw new ToolException("verify 需要一个 NSP 文件");
        return Check(file, Keyset.Load(Keyset.Find(keyset)), romfs, exefs);
    }

    public static int RunAfterBuild(Options options, Keyset keyset)
    {
        string nsp = options.OutputPath ?? FindBuiltNsp(options);
        return Check(nsp, keyset, options.NoRomfs ? null : options.RomfsDir, options.ExefsDir);
    }

    private static string FindBuiltNsp(Options options)
    {
        ulong titleId = options.TitleId != 0 ? options.TitleId : Npdm.ReadTitleId(Path.Combine(options.ExefsDir, "main.npdm"));
        return Path.Combine(options.NspDir, $"{titleId:x16}.nsp");
    }

    public static int Check(string path, Keyset keyset, string? romfs, string? exefs)
    {
        if (!File.Exists(path))
            throw new ToolException($"'{path}' not found");
        using var nsp = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read, 1 << 16);
        var verifier = new Verifier(nsp, keyset, romfs, exefs);
        Ui.Heading($"Verifying {path}");
        verifier.VerifyNsp();
        if (verifier._failures > 0)
        {
            Ui.Error($"FAILED: {verifier._failures} problem(s) found");
            return 1;
        }
        Ui.Success("正常：所有检查通过");
        return 0;
    }

    private void Fail(string message)
    {
        _failures++;
        Ui.Error($"  FAIL: {message}");
    }

    private void Pass(string message) => Ui.Detail($"  ok: {message}");

    private void VerifyNsp()
    {
        var head = ReadExact(0, 0x10);
        if (!head.AsSpan(0, 4).SequenceEqual("PFS0"u8))
            throw new ToolException("不是 NSP（PFS0 魔数错误）");
        int count = BinaryPrimitives.ReadInt32LittleEndian(head.AsSpan(4));
        int nameTable = BinaryPrimitives.ReadInt32LittleEndian(head.AsSpan(8));
        var table = ReadExact(0x10, count * 0x18 + nameTable);
        long dataStart = 0x10 + count * 0x18L + nameTable;
        long expected = dataStart;
        for (int i = 0; i < count; i++)
        {
            var e = table.AsSpan(i * 0x18);
            long offset = BinaryPrimitives.ReadInt64LittleEndian(e);
            long size = BinaryPrimitives.ReadInt64LittleEndian(e[8..]);
            int nameOffset = BinaryPrimitives.ReadInt32LittleEndian(e[0x10..]);
            var names = table.AsSpan(count * 0x18 + nameOffset);
            string name = Encoding.UTF8.GetString(names[..names.IndexOf((byte)0)]);
            _files[name] = (dataStart + offset, size);
            expected += size;
        }
        if (_nsp.Length != expected)
            Fail($"NSP is {_nsp.Length} bytes, its header describes {expected}");

        foreach (var (name, (offset, size)) in _files.OrderBy(f => f.Value.offset))
        {
            Ui.Info($"{name} ({ProgressReporter.Format(size)})");
            if (name.EndsWith(".nca", StringComparison.OrdinalIgnoreCase))
                VerifyNca(name, offset, size);
        }

        foreach (var (hash, id, size, type) in _records)
        {
            string name = Convert.ToHexString(id).ToLowerInvariant() + ".nca";
            if (!_files.TryGetValue(name, out var file))
            {
                Fail($"content meta lists {name}, which is not in the NSP");
                continue;
            }
            if (file.size != size)
                Fail($"content meta size of {name} is {size}, the file has {file.size}");
            if (!_hashes.TryGetValue(name, out var actual) || !actual.AsSpan().SequenceEqual(hash))
                Fail($"content meta hash of {name} does not match");
            else
                Ui.Detail($"cnmt record ok: {name} type {type}");
        }
    }

    private void VerifyNca(string name, long offset, long size)
    {
        var header = ReadExact(offset, 0xC00);
        NcaCrypto.XtsTransform(_keyset.HeaderKey, header, 0xC00, encrypt: false);
        var h = header.AsSpan();
        if (!h[0x200..0x204].SequenceEqual("NCA3"u8))
        {
            Fail("文件头解密后不是 NCA3");
            return;
        }

        var hash = HashRange(offset, size);
        _hashes[name] = hash;
        string id = Convert.ToHexString(hash, 0, 16).ToLowerInvariant();
        if (!name.StartsWith(id, StringComparison.OrdinalIgnoreCase))
            Fail($"file name does not match its SHA-256 ({id})");
        else
            Pass("文件名与 SHA-256 相符");

        long declared = BinaryPrimitives.ReadInt64LittleEndian(h[0x208..]);
        if (declared != size)
            Fail($"header content size is {declared}, the file has {size}");
        if (BinaryPrimitives.ReadUInt32LittleEndian(h[0x21C..]) < 0x000B0000)
            Fail("SDK 版本低于固件可接受的最低值 000B0000");

        if (h.Slice(0x230, 0x10).IndexOfAnyExcept((byte)0) >= 0)
        {
            Pass("使用了票据中的标题密钥，因此未校验其区段");
            return;
        }

        int generation = Math.Max(h[0x206], h[0x220]);
        int index = generation > 0 ? generation - 1 : 0;
        var kaek = _keyset.TryKeyAreaKeyApplication(index);
        if (kaek is null)
        {
            Fail($"key_area_key_application_{index:x2} is not in the keyset");
            return;
        }
        var keyArea = NcaCrypto.EcbDecrypt(kaek, h.Slice(0x300, 0x40).ToArray());
        var sectionKey = keyArea[0x20..0x30];
        byte contentType = h[0x205];

        for (int i = 0; i < 4; i++)
        {
            uint start = BinaryPrimitives.ReadUInt32LittleEndian(h[(0x240 + i * 0x10)..]);
            uint end = BinaryPrimitives.ReadUInt32LittleEndian(h[(0x244 + i * 0x10)..]);
            if (start == 0 && end == 0)
                continue;

            var fsHeader = h.Slice(0x400 + i * 0x200, 0x200).ToArray();
            if (!SHA256.HashData(fsHeader).AsSpan().SequenceEqual(h.Slice(0x280 + i * 0x20, 0x20)))
            {
                Fail($"section {i}: FS header hash mismatch");
                continue;
            }

            long sectionStart = start * 0x200L, sectionSize = (end - (long)start) * 0x200;
            if (sectionStart < 0xC00 || sectionStart + sectionSize > size)
            {
                Fail($"section {i}: outside the file");
                continue;
            }

            var section = new SectionStream(_nsp, offset, sectionStart, sectionSize,
                fsHeader[4] == 3 ? sectionKey : null, BinaryPrimitives.ReadUInt64LittleEndian(fsHeader.AsSpan(0x140)));
            switch (fsHeader[3])
            {
                case 2:
                    VerifyPfs0Section(i, fsHeader, section, contentType);
                    break;
                case 3:
                    VerifyRomFsSection(i, fsHeader, section, contentType);
                    break;
                case 5 or 6:
                    Pass($"section {i}: uses a SHA-3 hash tree, which wasn't checked");
                    break;
                default:
                    Fail($"section {i}: unexpected hash type {fsHeader[3]}");
                    break;
            }
            section.Dispose();
        }
    }

    private void VerifyPfs0Section(int index, byte[] fs, SectionStream section, byte contentType)
    {
        var f = fs.AsSpan();
        int block = BinaryPrimitives.ReadInt32LittleEndian(f[0x28..]);
        long hashSize = BinaryPrimitives.ReadInt64LittleEndian(f[0x38..]);
        long pfsOffset = BinaryPrimitives.ReadInt64LittleEndian(f[0x40..]);
        long pfsSize = BinaryPrimitives.ReadInt64LittleEndian(f[0x48..]);

        var table = section.ReadBytes(BinaryPrimitives.ReadInt64LittleEndian(f[0x30..]), (int)hashSize);
        if (!SHA256.HashData(table).AsSpan().SequenceEqual(f.Slice(0x08, 0x20)))
        {
            Fail($"section {index}: PFS0 master hash mismatch");
            return;
        }

        var buffer = new byte[block];
        long blocks = (pfsSize + block - 1) / block;
        for (long i = 0; i < blocks; i++)
        {
            int n = (int)Math.Min(block, pfsSize - i * block);
            section.Read(pfsOffset + i * block, buffer, 0, n);
            if (!SHA256.HashData(buffer.AsSpan(0, n)).AsSpan().SequenceEqual(table.AsSpan((int)(i * 0x20), 0x20)))
            {
                Fail($"section {index}: PFS0 block {i} hash mismatch");
                return;
            }
        }

        var header = section.ReadBytes(pfsOffset, 0x10);
        int count = BinaryPrimitives.ReadInt32LittleEndian(header.AsSpan(4));
        int names = BinaryPrimitives.ReadInt32LittleEndian(header.AsSpan(8));
        var entries = section.ReadBytes(pfsOffset + 0x10, count * 0x18 + names);
        long dataStart = pfsOffset + 0x10 + count * 0x18L + names;
        var listed = new List<string>();
        for (int i = 0; i < count; i++)
        {
            var e = entries.AsSpan(i * 0x18);
            long offset = BinaryPrimitives.ReadInt64LittleEndian(e);
            long size = BinaryPrimitives.ReadInt64LittleEndian(e[8..]);
            var nameSpan = entries.AsSpan(count * 0x18 + BinaryPrimitives.ReadInt32LittleEndian(e[0x10..]));
            string name = Encoding.UTF8.GetString(nameSpan[..nameSpan.IndexOf((byte)0)]);
            listed.Add(name);

            if (name.EndsWith(".cnmt", StringComparison.OrdinalIgnoreCase))
                ParseCnmt(section.ReadBytes(dataStart + offset, (int)size));

            if (index == 0 && contentType == 0 && _exefsDir is not null && !name.Equals("main.npdm", StringComparison.OrdinalIgnoreCase))
            {
                string source = Path.Combine(_exefsDir, name);
                if (!File.Exists(source) || new FileInfo(source).Length != size)
                    Fail($"ExeFS {name}: missing or different size in {_exefsDir}");
                else if (!section.ReadBytes(dataStart + offset, (int)size).AsSpan().SequenceEqual(File.ReadAllBytes(source)))
                    Fail($"ExeFS {name}: differs from {source}");
            }
        }
        Pass($"section {index}: PFS0 hashes valid, files: {string.Join(", ", listed)}");
    }

    private void ParseCnmt(byte[] cnmt)
    {
        int extended = BinaryPrimitives.ReadUInt16LittleEndian(cnmt.AsSpan(0x0E));
        int contents = BinaryPrimitives.ReadUInt16LittleEndian(cnmt.AsSpan(0x10));
        for (int i = 0; i < contents; i++)
        {
            var r = cnmt.AsSpan(0x20 + extended + i * 0x38);
            long size = 0;
            for (int b = 5; b >= 0; b--)
                size = (size << 8) | r[0x30 + b];
            _records.Add((r[..0x20].ToArray(), r.Slice(0x20, 0x10).ToArray(), size, r[0x36]));
        }
    }

    private void VerifyRomFsSection(int index, byte[] fs, SectionStream section, byte contentType)
    {
        var f = fs.AsSpan();
        if (!f.Slice(0x08, 4).SequenceEqual("IVFC"u8))
        {
            Fail($"section {index}: missing IVFC magic");
            return;
        }

        int levels = BinaryPrimitives.ReadInt32LittleEndian(f[0x14..]) - 1;
        if (levels < 1 || levels > 6)
        {
            Fail($"section {index}: unexpected IVFC layer count");
            return;
        }
        var offsets = new long[levels];
        var sizes = new long[levels];
        var blockSizes = new int[levels];
        for (int l = 0; l < levels; l++)
        {
            offsets[l] = BinaryPrimitives.ReadInt64LittleEndian(f[(0x18 + l * 0x18)..]);
            sizes[l] = BinaryPrimitives.ReadInt64LittleEndian(f[(0x20 + l * 0x18)..]);
            blockSizes[l] = 1 << BinaryPrimitives.ReadInt32LittleEndian(f[(0x28 + l * 0x18)..]);
        }

        // The header's master hash covers level 0, each level's hashes cover the next, and the last level is the data.
        var parent = f.Slice(0xC8, 0x20).ToArray();
        for (int l = 0; l < levels; l++)
        {
            int block = blockSizes[l];
            byte[]? child = l == levels - 1 ? null : section.ReadBytes(offsets[l], (int)sizes[l]);
            var buffer = new byte[block];
            long blocks = (sizes[l] + block - 1) / block;
            for (long b = 0; b < blocks; b++)
            {
                int n = (int)Math.Min(block, sizes[l] - b * block);
                section.Read(offsets[l] + b * block, buffer, 0, n);
                Array.Clear(buffer, n, block - n);
                if ((b + 1) * 0x20 > parent.Length ||
                    !SHA256.HashData(buffer).AsSpan().SequenceEqual(parent.AsSpan((int)(b * 0x20), 0x20)))
                {
                    Fail($"section {index}: IVFC level {l} block {b} hash mismatch");
                    return;
                }
            }
            if (child is not null)
                parent = child;
        }
        Pass($"section {index}: IVFC hash tree valid");

        var data = new SubStream(section, offsets[levels - 1], sizes[levels - 1]);
        IVirtualReader reader;
        long tableOffset = BinaryPrimitives.ReadInt64LittleEndian(f[0x178..]);
        long tableSize = BinaryPrimitives.ReadInt64LittleEndian(f[0x180..]);
        if (tableOffset != 0 && tableSize != 0)
        {
            var compressed = VerifyCompressionLayer(index, f, data, tableOffset, tableSize);
            if (compressed is null)
                return;
            reader = compressed;
        }
        else
        {
            reader = new PlainReader(data);
        }

        VerifyRomFsImage(index, reader, index == 1 && contentType == 0 ? _romfsDir : null);
    }

    private CompressedReader? VerifyCompressionLayer(int index, ReadOnlySpan<byte> f, SubStream data, long tableOffset, long tableSize)
    {
        if (!f.Slice(0x188, 4).SequenceEqual("BKTR"u8) || BinaryPrimitives.ReadUInt32LittleEndian(f[0x18C..]) > BucketTree.Version)
        {
            Fail($"section {index}: compression header is not a valid BKTR header");
            return null;
        }
        int entryCount = BinaryPrimitives.ReadInt32LittleEndian(f[0x190..]);
        long needed = BucketTree.NodeStorageSize(entryCount) + BucketTree.EntryStorageSize(entryCount);
        if (needed > tableSize || tableOffset + tableSize > data.Length)
        {
            Fail($"section {index}: compression table does not fit in the data level");
            return null;
        }

        var table = new byte[tableSize];
        data.Read(tableOffset, table, 0, table.Length);
        var tree = new BucketTreeReader(table, entryCount);

        var entries = new List<(CompressionEntry entry, long end)>(entryCount);
        var buffer = new byte[Lz4Encoder.MaxBlockSize];
        var decoded = new byte[Lz4Encoder.MaxBlockSize];
        long lz4 = 0, stored = 0, physicalTotal = 0;
        long previousPhysicalEnd = 0;
        foreach (var (entry, end) in tree.EnumerateAll())
        {
            long virtualSize = end - entry.VirtualOffset;
            bool isLz4 = entry.Type == CompressionType.Lz4;
            long physicalSize = isLz4 ? entry.PhysicalSize : virtualSize;
            if (entry.PhysicalOffset < 0 || entry.PhysicalOffset < previousPhysicalEnd)
            {
                Fail($"section {index}: entry at 0x{entry.VirtualOffset:x} overlaps the previous one");
                return null;
            }
            if (entry.PhysicalOffset + physicalSize > tableOffset)
            {
                Fail($"section {index}: entry at 0x{entry.VirtualOffset:x} runs past the data area");
                return null;
            }
            if (isLz4)
            {
                if (entry.PhysicalOffset % 0x10 != 0)
                {
                    Fail($"section {index}: LZ4 entry at 0x{entry.VirtualOffset:x} is not 0x10 aligned");
                    return null;
                }
                if (entry.PhysicalSize > RomFsCompressor.MaxPhysicalBlockSize || virtualSize > Lz4Encoder.MaxBlockSize)
                {
                    Fail($"section {index}: LZ4 entry at 0x{entry.VirtualOffset:x} exceeds the 64 KiB limit");
                    return null;
                }
                data.Read(entry.PhysicalOffset, buffer, 0, entry.PhysicalSize);
                int n = Lz4Decoder.Decode(buffer, 0, entry.PhysicalSize, decoded, 0, decoded.Length);
                if (n != virtualSize)
                {
                    Fail($"section {index}: LZ4 entry at 0x{entry.VirtualOffset:x} decodes to {n} bytes, expected {virtualSize}");
                    return null;
                }
                lz4++;
            }
            else if (entry.Type == CompressionType.None)
            {
                // The firmware never reads a raw entry's physical size: retail NCAs have -1 there.
                if (entry.PhysicalSize != -1 && entry.PhysicalSize != virtualSize)
                {
                    Fail($"section {index}: raw entry at 0x{entry.VirtualOffset:x} has mismatched sizes");
                    return null;
                }
                stored++;
            }
            else
            {
                Fail($"section {index}: entry at 0x{entry.VirtualOffset:x} has unexpected type {(int)entry.Type}");
                return null;
            }
            physicalTotal += physicalSize;
            previousPhysicalEnd = entry.PhysicalOffset + physicalSize;
            entries.Add((entry, end));
        }

        // Spot check the tree walk against the flat list, including both ends of every entry set boundary.
        var random = new Random(1);
        for (int i = 0; i < entries.Count; i += Math.Max(1, entries.Count / 2048))
        {
            var (entry, end) = entries[i];
            CheckLookup(tree, entry.VirtualOffset, entry, end);
            CheckLookup(tree, end - 1, entry, end);
            CheckLookup(tree, random.NextInt64(entry.VirtualOffset, end), entry, end);
        }

        Pass($"section {index}: compression table valid: {entryCount} entries ({lz4} LZ4, {stored} raw runs), " +
             $"{ProgressReporter.Format(tree.VirtualSize)} virtual in {ProgressReporter.Format(physicalTotal)} physical");
        return new CompressedReader(data, tree);
    }

    private void CheckLookup(BucketTreeReader tree, long address, CompressionEntry expected, long end)
    {
        var (found, foundEnd) = tree.Find(address);
        if (found.VirtualOffset != expected.VirtualOffset || foundEnd != end)
            Fail($"tree lookup of 0x{address:x} found the entry at 0x{found.VirtualOffset:x}, expected 0x{expected.VirtualOffset:x}");
    }

    private void VerifyRomFsImage(int index, IVirtualReader reader, string? sourceDir)
    {
        var head = new byte[0x50];
        reader.Read(0, head, 0, head.Length);
        long Field(int i) => BinaryPrimitives.ReadInt64LittleEndian(head.AsSpan(i * 8));
        if (Field(0) != 0x50)
        {
            Fail($"section {index}: RomFS header size is not 0x50");
            return;
        }

        var dirHash = ReadTable(reader, Field(1), Field(2));
        var dirTable = ReadTable(reader, Field(3), Field(4));
        var fileHash = ReadTable(reader, Field(5), Field(6));
        var fileTable = ReadTable(reader, Field(7), Field(8));
        long partition = Field(9);
        long imageEnd = Field(7) + Field(8);
        if (imageEnd > reader.Length)
            Fail($"section {index}: RomFS ends at 0x{imageEnd:x}, the storage is 0x{reader.Length:x}");

        var found = new Dictionary<string, (long offset, long size)>(StringComparer.Ordinal);
        WalkDirectory(0, "", dirTable, dirHash, fileTable, fileHash, found);
        long total = found.Values.Sum(v => v.size);
        Pass($"section {index}: RomFS tables valid, {found.Count} files, {ProgressReporter.Format(total)}");

        if (sourceDir is null)
            return;

        int differences = 0;
        var seen = new HashSet<string>(StringComparer.Ordinal);
        var root = Path.GetFullPath(sourceDir);
        var buffer = new byte[1 << 20];
        var sourceBuffer = new byte[1 << 20];
        var progress = new ProgressReporter("正在将 RomFS 与源文件比对", total);
        long compared = 0;
        foreach (var path in Directory.EnumerateFiles(root, "*", SearchOption.AllDirectories))
        {
            string relative = Path.GetRelativePath(root, path).Replace('\\', '/');
            seen.Add(relative);
            if (!found.TryGetValue(relative, out var entry))
            {
                Fail($"source file {relative} is not in the RomFS");
                differences++;
                continue;
            }

            using var source = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read, 1 << 16, FileOptions.SequentialScan);
            if (source.Length != entry.size)
            {
                Fail($"{relative}: size {entry.size} in the RomFS, {source.Length} on disk");
                differences++;
                continue;
            }
            for (long done = 0; done < entry.size;)
            {
                int n = (int)Math.Min(buffer.Length, entry.size - done);
                reader.Read(partition + entry.offset + done, buffer, 0, n);
                source.ReadExactly(sourceBuffer, 0, n);
                if (!buffer.AsSpan(0, n).SequenceEqual(sourceBuffer.AsSpan(0, n)))
                {
                    Fail($"{relative}: content differs near offset 0x{done:x}");
                    differences++;
                    break;
                }
                done += n;
                compared += n;
                progress.Report(compared, compared);
            }
        }
        foreach (var name in found.Keys)
        {
            if (!seen.Contains(name))
            {
                Fail($"RomFS file {name} is not in the source directory");
                differences++;
            }
        }
        progress.Done();
        if (differences == 0)
            Pass($"section {index}: all {found.Count} RomFS files are byte identical to {sourceDir}");
    }

    private void WalkDirectory(uint dirOffset, string path, byte[] dirTable, byte[] dirHash, byte[] fileTable, byte[] fileHash,
        Dictionary<string, (long offset, long size)> found)
    {
        uint Read32(byte[] table, long offset) => BinaryPrimitives.ReadUInt32LittleEndian(table.AsSpan((int)offset));
        const uint Empty = 0xFFFFFFFF;

        for (uint file = Read32(dirTable, dirOffset + 0x0C); file != Empty; file = Read32(fileTable, file + 0x04))
        {
            int nameSize = (int)Read32(fileTable, file + 0x1C);
            var name = fileTable.AsSpan((int)file + 0x20, nameSize);
            string full = path + Encoding.UTF8.GetString(name);
            found[full] = (BinaryPrimitives.ReadInt64LittleEndian(fileTable.AsSpan((int)file + 0x08)),
                BinaryPrimitives.ReadInt64LittleEndian(fileTable.AsSpan((int)file + 0x10)));

            if (!HashChainContains(fileHash, fileTable, RomFsBuilder.PathHash(dirOffset, name), file, 0x18))
                Fail($"RomFS file {full} cannot be reached through the file hash table");
            if (Read32(fileTable, file) != dirOffset)
                Fail($"RomFS file {full} has the wrong parent");
        }

        for (uint child = Read32(dirTable, dirOffset + 0x08); child != Empty; child = Read32(dirTable, child + 0x04))
        {
            int nameSize = (int)Read32(dirTable, child + 0x14);
            var name = dirTable.AsSpan((int)child + 0x18, nameSize);
            string full = path + Encoding.UTF8.GetString(name) + "/";

            if (!HashChainContains(dirHash, dirTable, RomFsBuilder.PathHash(dirOffset, name), child, 0x10))
                Fail($"RomFS directory {full} cannot be reached through the directory hash table");
            if (Read32(dirTable, child) != dirOffset)
                Fail($"RomFS directory {full} has the wrong parent");
            WalkDirectory(child, full, dirTable, dirHash, fileTable, fileHash, found);
        }
    }

    private static bool HashChainContains(byte[] buckets, byte[] table, uint hash, uint target, int nextField)
    {
        uint bucket = hash % (uint)(buckets.Length / 4);
        uint entry = BinaryPrimitives.ReadUInt32LittleEndian(buckets.AsSpan((int)bucket * 4));
        while (entry != 0xFFFFFFFF)
        {
            if (entry == target)
                return true;
            entry = BinaryPrimitives.ReadUInt32LittleEndian(table.AsSpan((int)entry + nextField));
        }
        return false;
    }

    private static byte[] ReadTable(IVirtualReader reader, long offset, long size)
    {
        var table = new byte[size];
        reader.Read(offset, table, 0, table.Length);
        return table;
    }

    private byte[] HashRange(long offset, long size)
    {
        using var sha = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
        var buffer = new byte[1 << 20];
        var progress = new ProgressReporter("正在计算 NCA 哈希", size);
        _nsp.Position = offset;
        for (long left = size; left > 0;)
        {
            int n = _nsp.Read(buffer, 0, (int)Math.Min(buffer.Length, left));
            if (n <= 0)
                throw new ToolException("NSP 意外结束");
            sha.AppendData(buffer, 0, n);
            left -= n;
            progress.Report(size - left, size - left);
        }
        progress.Done();
        return sha.GetHashAndReset();
    }

    private byte[] ReadExact(long offset, int size)
    {
        var buffer = new byte[size];
        _nsp.Position = offset;
        _nsp.ReadExactly(buffer);
        return buffer;
    }

    private interface IVirtualReader
    {
        long Length { get; }
        void Read(long offset, byte[] buffer, int bufferOffset, int count);
    }

    private sealed class SectionStream(FileStream file, long ncaOffset, long sectionStart, long sectionSize, byte[]? key, ulong upperIv) : IDisposable
    {
        private readonly AesCtrTransform? _ctr = key is null ? null : new AesCtrTransform(key, upperIv);
        private byte[] _scratch = new byte[0x10000];

        public void Read(long offset, byte[] buffer, int bufferOffset, int count)
        {
            if (offset < 0 || offset + count > sectionSize)
                throw new ToolException("读取超出 NCA 区段范围");
            Ui.Token.ThrowIfCancellationRequested();

            long fileOffset = sectionStart + offset;
            int head = (int)(fileOffset & 15);
            int aligned = (head + count + 15) & ~15;
            if (_scratch.Length < aligned)
                _scratch = new byte[aligned];
            file.Position = ncaOffset + fileOffset - head;
            file.ReadExactly(_scratch, 0, aligned);
            _ctr?.Transform(_scratch, 0, aligned, fileOffset - head);
            Buffer.BlockCopy(_scratch, head, buffer, bufferOffset, count);
        }

        public byte[] ReadBytes(long offset, int count)
        {
            var bytes = new byte[count];
            Read(offset, bytes, 0, count);
            return bytes;
        }

        public void Dispose() => _ctr?.Dispose();
    }

    private sealed class SubStream(SectionStream section, long start, long length)
    {
        public long Length => length;

        public void Read(long offset, byte[] buffer, int bufferOffset, int count)
        {
            if (offset < 0 || offset + count > length)
                throw new ToolException("读取超出 IVFC 数据层范围");
            section.Read(start + offset, buffer, bufferOffset, count);
        }
    }

    private sealed class PlainReader(SubStream data) : IVirtualReader
    {
        public long Length => data.Length;
        public void Read(long offset, byte[] buffer, int bufferOffset, int count) => data.Read(offset, buffer, bufferOffset, count);
    }

    /// <summary>Random access the way the firmware does it: find the entry through the tree, then decode the whole block.</summary>
    private sealed class CompressedReader(SubStream data, BucketTreeReader tree) : IVirtualReader
    {
        private readonly byte[] _compressed = new byte[Lz4Encoder.MaxBlockSize];
        private readonly byte[] _block = new byte[Lz4Encoder.MaxBlockSize];
        private long _blockStart = -1;
        private int _blockLength;

        public long Length => tree.VirtualSize;

        public void Read(long offset, byte[] buffer, int bufferOffset, int count)
        {
            while (count > 0)
            {
                var (entry, end) = tree.Find(offset);
                long inEntry = offset - entry.VirtualOffset;
                int n = (int)Math.Min(count, end - offset);
                if (entry.Type == CompressionType.None)
                {
                    data.Read(entry.PhysicalOffset + inEntry, buffer, bufferOffset, n);
                }
                else
                {
                    if (_blockStart != entry.VirtualOffset)
                    {
                        data.Read(entry.PhysicalOffset, _compressed, 0, entry.PhysicalSize);
                        _blockLength = Lz4Decoder.Decode(_compressed, 0, entry.PhysicalSize, _block, 0, _block.Length);
                        if (_blockLength != end - entry.VirtualOffset)
                            throw new ToolException($"LZ4 entry at 0x{entry.VirtualOffset:x} failed to decode");
                        _blockStart = entry.VirtualOffset;
                    }
                    Buffer.BlockCopy(_block, (int)inEntry, buffer, bufferOffset, n);
                }
                offset += n;
                bufferOffset += n;
                count -= n;
            }
        }
    }
}
