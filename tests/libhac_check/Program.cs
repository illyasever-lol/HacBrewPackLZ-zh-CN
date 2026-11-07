// Independent check of a compressed NSP through LibHac (from Ryujinx), which ports Nintendo's FS code.
// usage: libhac_check keys nsp [--romfsdir dir] [--exefsdir dir] [--ref ref.nsp] [--random N]
using System.Security.Cryptography;
using LibHac;
using LibHac.Common;
using LibHac.Common.Keys;
using LibHac.Fs;
using LibHac.Fs.Fsa;
using LibHac.FsSystem;
using LibHac.Tools.Fs;
using LibHac.Tools.FsSystem;
using LibHac.Tools.FsSystem.NcaUtils;
using LibHac.Tools.Ncm;
using NcaFsHeader = LibHac.Tools.FsSystem.NcaUtils.NcaFsHeader;
using Path = System.IO.Path;

static class Check
{
    static KeySet _keys = null!;
    static int _failures;

    static void Fail(string m) { _failures++; Console.WriteLine("  FAIL: " + m); }

    static int Main(string[] a)
    {
        string keys = a[0], nsp = a[1];
        string? romfs = null, exefs = null, reference = null, dumpImage = null, baseNsp = null, updateNsp = null;
        int random = 20000;
        for (int i = 2; i < a.Length; i += 2)
        {
            switch (a[i])
            {
                case "--romfsdir": romfs = a[i + 1]; break;
                case "--exefsdir": exefs = a[i + 1]; break;
                case "--ref": reference = a[i + 1]; break;
                case "--random": random = int.Parse(a[i + 1]); break;
                case "--dump-image": dumpImage = a[i + 1]; break;
                case "--base": baseNsp = a[i + 1]; break;
                case "--update": updateNsp = a[i + 1]; break;
            }
        }

        _keys = ExternalKeyReader.ReadKeyFile(keys);
        var (pfs, ncas) = OpenNsp(nsp);
        Console.WriteLine($"PFS0 with {ncas.Count} NCAs");

        if (baseNsp is not null)
        {
            CompareWithSource(ncas, baseNsp, updateNsp);
            Console.WriteLine(_failures == 0 ? "LIBHAC: ALL OK" : $"LIBHAC: {_failures} FAILURES");
            return _failures == 0 ? 0 : 1;
        }

        if (dumpImage is not null)
        {
            var image = ncas.First(n => n.nca.Header.ContentType == NcaContentType.Program).nca
                .OpenStorage(NcaSectionType.Data, IntegrityCheckLevel.ErrorOnInvalid);
            using var output = File.Create(dumpImage);
            image.CopyToStream(output);
            Console.WriteLine($"dumped {output.Length} bytes");
            return 0;
        }

        Nca? program = null;
        foreach (var (name, nca) in ncas)
        {
            Console.WriteLine($"{name}: {nca.Header.ContentType} title {nca.Header.TitleId:x16} sdk {nca.Header.SdkVersion} keygen {nca.Header.KeyGeneration}");
            Console.WriteLine($"  fixed key signature: {nca.VerifyHeaderSignature()} (expected Invalid, needs sigpatches)");
            for (int i = 0; i < 4; i++)
            {
                if (!nca.SectionExists(i))
                    continue;
                var h = nca.GetFsHeader(i);
                Console.WriteLine($"  section {i}: {h.FormatType} {h.HashType} {h.EncryptionType} compression={h.ExistsCompressionLayer()} masterhash={nca.ValidateSectionMasterHash(i)}");
                var v = nca.VerifySection(i, null, true);
                Console.WriteLine($"    VerifySection: {v}");
                if (v != Validity.Valid)
                    Fail($"section {i} verification {v}");
            }
            if (nca.Header.ContentType == NcaContentType.Program)
                program = nca;
            if (nca.Header.ContentType == NcaContentType.Meta)
                CheckCnmt(nca, ncas);
            if (nca.Header.ContentType == NcaContentType.Control)
                CheckControl(nca);
        }

        if (program is null)
        {
            Fail("no Program NCA");
            return 1;
        }

        if (exefs is not null)
            CompareTree(program.OpenFileSystem(NcaSectionType.Code, IntegrityCheckLevel.ErrorOnInvalid), exefs, "ExeFS", "main.npdm");
        if (romfs is not null)
            CompareTree(program.OpenFileSystem(NcaSectionType.Data, IntegrityCheckLevel.ErrorOnInvalid), romfs, "RomFS", null);

        if (reference is not null && program.GetFsHeader(1).ExistsCompressionLayer())
        {
            var (_, refNcas) = OpenNsp(reference);
            var refProgram = refNcas.First(n => n.nca.Header.ContentType == NcaContentType.Program).nca;
            var plain = refProgram.OpenStorage(NcaSectionType.Data, IntegrityCheckLevel.ErrorOnInvalid);
            RandomAccessTest(program, plain, random);
        }
        if (program.GetFsHeader(1).ExistsCompressionLayer())
            BucketTreeTest(program);

        Console.WriteLine(_failures == 0 ? "LIBHAC: ALL OK" : $"LIBHAC: {_failures} FAILURES");
        return _failures == 0 ? 0 : 1;
    }

    static (PartitionFileSystem pfs, List<(string name, Nca nca)> ncas) OpenNsp(string path)
    {
        var storage = new LocalStorage(path, FileAccess.Read);
        var pfs = new PartitionFileSystem();
        pfs.Initialize(storage).ThrowIfFailure();
        foreach (var e in pfs.EnumerateEntries("/", "*.tik"))
        {
            using var file = new UniqueRef<IFile>();
            pfs.OpenFile(ref file.Ref, e.FullPath.ToU8Span(), OpenMode.Read).ThrowIfFailure();
            var ticket = new LibHac.Tools.Es.Ticket(file.Get.AsStream());
            _keys.ExternalKeySet.Add(new LibHac.Fs.RightsId(ticket.RightsId), new LibHac.Spl.AccessKey(ticket.GetTitleKey(_keys))).ThrowIfFailure();
        }

        var list = new List<(string, Nca)>();
        foreach (var e in pfs.EnumerateEntries("/", "*.nca"))
        {
            using var file = new UniqueRef<IFile>();
            pfs.OpenFile(ref file.Ref, e.FullPath.ToU8Span(), OpenMode.Read).ThrowIfFailure();
            list.Add((e.Name, new Nca(_keys, file.Release().AsStorage())));
        }
        return (pfs, list);
    }

    // Reads the original base (and update) through LibHac and compares every file with the rebuilt NSP.
    static void CompareWithSource(List<(string name, Nca nca)> rebuilt, string baseNsp, string? updateNsp)
    {
        var (_, baseNcas) = OpenNsp(baseNsp);
        List<(string name, Nca nca)>? updateNcas = updateNsp is null ? null : OpenNsp(updateNsp).ncas;

        foreach (NcaContentType type in new[] { NcaContentType.Program, NcaContentType.Control, NcaContentType.Data })
        {
            var mine = rebuilt.Where(n => n.nca.Header.ContentType == type).Select(n => n.nca).ToList();
            var fromBase = baseNcas.Where(n => n.nca.Header.ContentType == type).Select(n => n.nca).ToList();
            var fromUpdate = updateNcas?.Where(n => n.nca.Header.ContentType == type).Select(n => n.nca).ToList();
            for (int i = 0; i < mine.Count; i++)
            {
                var b = i < fromBase.Count ? fromBase[i] : null;
                var u = fromUpdate is not null && i < fromUpdate.Count ? fromUpdate[i] : null;
                for (int s = 0; s < 3; s++)
                {
                    if (!mine[i].SectionExists(s))
                        continue;
                    IFileSystem expected;
                    if (u is not null && u.SectionExists(s))
                        expected = b is not null ? b.OpenFileSystemWithPatch(u, s, IntegrityCheckLevel.ErrorOnInvalid) : u.OpenFileSystem(s, IntegrityCheckLevel.ErrorOnInvalid);
                    else
                        expected = b!.OpenFileSystem(s, IntegrityCheckLevel.ErrorOnInvalid);
                    var actual = mine[i].OpenFileSystem(s, IntegrityCheckLevel.ErrorOnInvalid);
                    CompareFileSystems(expected, actual, $"{type} section {s}");
                }
            }
        }
    }

    static void CompareFileSystems(IFileSystem expected, IFileSystem actual, string label)
    {
        var want = expected.EnumerateEntries("/", "*", SearchOptions.RecurseSubdirectories).Where(e => e.Type == DirectoryEntryType.File).ToDictionary(e => e.FullPath);
        var have = actual.EnumerateEntries("/", "*", SearchOptions.RecurseSubdirectories).Where(e => e.Type == DirectoryEntryType.File).ToDictionary(e => e.FullPath);
        foreach (var missing in want.Keys.Except(have.Keys))
            Fail($"{label}: {missing} is missing from the rebuilt NSP");
        foreach (var extra in have.Keys.Except(want.Keys))
            Fail($"{label}: {extra} is only in the rebuilt NSP");

        long bytes = 0;
        int skipped = 0;
        var a = new byte[1 << 20];
        var b = new byte[1 << 20];
        foreach (var (path, entry) in want)
        {
            if (!have.TryGetValue(path, out var other))
                continue;
            if (entry.Name == "main.npdm")
            {
                skipped++;
                continue;
            }
            if (entry.Size != other.Size)
            {
                Fail($"{label}: {path} size {entry.Size} vs {other.Size}");
                continue;
            }
            using var fa = new UniqueRef<IFile>();
            using var fb = new UniqueRef<IFile>();
            expected.OpenFile(ref fa.Ref, path.ToU8Span(), OpenMode.Read).ThrowIfFailure();
            actual.OpenFile(ref fb.Ref, path.ToU8Span(), OpenMode.Read).ThrowIfFailure();
            for (long done = 0; done < entry.Size;)
            {
                int n = (int)Math.Min(a.Length, entry.Size - done);
                fa.Get.Read(out long ra, done, a.AsSpan(0, n)).ThrowIfFailure();
                fb.Get.Read(out long rb, done, b.AsSpan(0, n)).ThrowIfFailure();
                if (ra != n || rb != n || !a.AsSpan(0, n).SequenceEqual(b.AsSpan(0, n)))
                {
                    Fail($"{label}: {path} differs near 0x{done:x}");
                    break;
                }
                done += n;
                bytes += n;
            }
        }
        Console.WriteLine($"{label}: {want.Count} files, {bytes} bytes identical to the original read by LibHac" + (skipped > 0 ? " (main.npdm skipped, it is patched on purpose)" : ""));
    }

    static void CheckCnmt(Nca meta, List<(string name, Nca nca)> ncas)
    {
        var fs = meta.OpenFileSystem(0, IntegrityCheckLevel.ErrorOnInvalid);
        var e = fs.EnumerateEntries("/", "*.cnmt").Single();
        using var file = new UniqueRef<IFile>();
        fs.OpenFile(ref file.Ref, e.FullPath.ToU8Span(), OpenMode.Read).ThrowIfFailure();
        var cnmt = new Cnmt(file.Get.AsStream());
        Console.WriteLine($"  cnmt {e.Name}: {cnmt.Type} title {cnmt.TitleId:x16} patch {cnmt.PatchTitleId:x16} min system {cnmt.MinimumSystemVersion} entries {cnmt.ContentEntries.Length}");
        foreach (var c in cnmt.ContentEntries)
        {
            string id = Convert.ToHexString(c.NcaId).ToLowerInvariant() + ".nca";
            var match = ncas.FirstOrDefault(n => n.name == id);
            Console.WriteLine($"    {c.Type} {id} size {c.Size}: {(match.nca is null ? "MISSING" : "present")}");
            if (match.nca is null)
                Fail("cnmt entry missing from NSP: " + id);
        }
    }

    static void CheckControl(Nca control)
    {
        var fs = control.OpenFileSystem(0, IntegrityCheckLevel.ErrorOnInvalid);
        Console.WriteLine("  control files: " + string.Join(", ", fs.EnumerateEntries("/", "*").Select(x => x.Name)));
    }

    static void CompareTree(IFileSystem fs, string dir, string label, string? skip)
    {
        var root = Path.GetFullPath(dir);
        var onDisk = Directory.EnumerateFiles(root, "*", SearchOption.AllDirectories)
            .ToDictionary(p => "/" + Path.GetRelativePath(root, p).Replace('\\', '/'), p => p);
        int files = 0;
        long bytes = 0;
        var buffer = new byte[1 << 20];
        var source = new byte[1 << 20];
        foreach (var e in fs.EnumerateEntries("/", "*", SearchOptions.RecurseSubdirectories))
        {
            if (e.Type != DirectoryEntryType.File)
                continue;
            if (!onDisk.Remove(e.FullPath, out var path))
            {
                Fail($"{label}: {e.FullPath} is not in the source directory");
                continue;
            }
            if (skip is not null && e.Name == skip)
                continue;
            using var file = new UniqueRef<IFile>();
            fs.OpenFile(ref file.Ref, e.FullPath.ToU8Span(), OpenMode.Read).ThrowIfFailure();
            using var disk = File.OpenRead(path);
            if (disk.Length != e.Size)
            {
                Fail($"{label}: {e.FullPath} size {e.Size} vs {disk.Length}");
                continue;
            }
            for (long done = 0; done < e.Size;)
            {
                int n = (int)Math.Min(buffer.Length, e.Size - done);
                file.Get.Read(out long read, done, buffer.AsSpan(0, n)).ThrowIfFailure();
                if (read != n)
                    throw new Exception("short read");
                disk.ReadExactly(source, 0, n);
                if (!buffer.AsSpan(0, n).SequenceEqual(source.AsSpan(0, n)))
                {
                    Fail($"{label}: {e.FullPath} differs near 0x{done:x}");
                    break;
                }
                done += n;
                bytes += n;
            }
            files++;
        }
        foreach (var missing in onDisk.Keys)
            Fail($"{label}: source file {missing} not found through the filesystem");
        Console.WriteLine($"{label}: {files} files, {bytes} bytes byte-identical to {dir} through LibHac");
    }

    // Walks the BKTR table through LibHac.FsSystem.BucketTree, a port of fssystem::BucketTree including its Verify checks.
    static void BucketTreeTest(Nca program)
    {
        NcaFsHeader header = program.GetFsHeader(1);
        ref NcaCompressionInfo info = ref header.GetCompressionInfo();
        var hashed = program.OpenStorage(1, IntegrityCheckLevel.ErrorOnInvalid, true);

        BucketTree.Header treeHeader = default;
        info.TableHeader.ItemsRo.CopyTo(SpanHelpers.AsByteSpan(ref treeHeader));
        treeHeader.Verify().ThrowIfFailure();

        const int NodeSize = 0x4000, EntrySize = 0x18;
        long nodeBytes = BucketTree.QueryNodeStorageSize(NodeSize, EntrySize, treeHeader.EntryCount);
        long entryBytes = BucketTree.QueryEntryStorageSize(NodeSize, EntrySize, treeHeader.EntryCount);
        Console.WriteLine($"BKTR: {treeHeader.EntryCount} entries, node storage 0x{nodeBytes:x}, entry storage 0x{entryBytes:x}, table 0x{info.TableOffset:x}+0x{info.TableSize:x}");
        if (nodeBytes + entryBytes > info.TableSize)
            Fail("table smaller than node+entry storage");

        using var node = new ValueSubStorage(hashed, info.TableOffset, nodeBytes);
        using var entry = new ValueSubStorage(hashed, info.TableOffset + nodeBytes, entryBytes);

        using var tree = new BucketTree();
        tree.Initialize(new ArrayPoolMemoryResource(), in node, in entry, NodeSize, EntrySize, treeHeader.EntryCount).ThrowIfFailure();
        program.OpenStorage(NcaSectionType.Data, IntegrityCheckLevel.ErrorOnInvalid).GetSize(out long virtualSize).ThrowIfFailure();

        using var visitor = new BucketTree.Visitor();
        tree.Find(ref visitor.Ref, 0).ThrowIfFailure();

        long expectedVirtual = 0, previousPhysicalEnd = 0, count = 0, lz4 = 0, raw = 0;
        while (true)
        {
            var e = visitor.Get<RawEntry>();
            if (e.VirtualOffset != expectedVirtual && count == 0)
                Fail("first entry does not start at 0");
            if (e.PhysicalOffset < previousPhysicalEnd)
                Fail($"entry at 0x{e.VirtualOffset:x} overlaps the previous physical range");
            if (e.Type == 0 && e.PhysicalSize != -1)
                Fail($"raw entry at 0x{e.VirtualOffset:x} doesn't have the retail physical size of -1");
            if (e.Type == 3)
            {
                lz4++;
                if (e.PhysicalOffset % 0x10 != 0)
                    Fail($"LZ4 entry at 0x{e.VirtualOffset:x} is not 0x10 aligned (CompressionBlockAlignment)");
                if (e.PhysicalSize > 0x10000)
                    Fail($"LZ4 entry at 0x{e.VirtualOffset:x} exceeds the 64 KiB block_size_max");
            }
            else if (e.Type == 0)
            {
                raw++;
            }
            else
            {
                Fail($"entry at 0x{e.VirtualOffset:x} has type {e.Type}");
            }
            count++;
            long nextVirtual = virtualSize;
            bool more = visitor.CanMoveNext();
            if (more)
            {
                visitor.MoveNext().ThrowIfFailure();
                nextVirtual = visitor.Get<RawEntry>().VirtualOffset;
            }
            previousPhysicalEnd = e.PhysicalOffset + (e.Type == 0 ? nextVirtual - e.VirtualOffset : e.PhysicalSize);
            if (!more)
                break;
            expectedVirtual = nextVirtual;
        }
        if (count != treeHeader.EntryCount)
            Fail($"visited {count} entries, header says {treeHeader.EntryCount}");
        if (previousPhysicalEnd > info.TableOffset)
            Fail("physical data overlaps the table");

        // Random lookups walk L1, L2 and entry sets from the top each time.
        var rng = new Random(77);
        for (int i = 0; i < 20000; i++)
        {
            long address = rng.NextInt64(virtualSize);
            using var v = new BucketTree.Visitor();
            tree.Find(ref v.Ref, address).ThrowIfFailure();
            if (v.Get<RawEntry>().VirtualOffset > address)
                Fail($"lookup of 0x{address:x} found a later entry");
            if (v.CanMoveNext())
            {
                v.MoveNext().ThrowIfFailure();
                if (v.Get<RawEntry>().VirtualOffset <= address)
                    Fail($"lookup of 0x{address:x} did not find the last entry at or before it");
            }
        }
        Console.WriteLine($"BucketTree port: {count} entries ok ({lz4} LZ4, {raw} raw runs), virtual size 0x{virtualSize:x}, 20000 lookups ok");
    }

    [System.Runtime.InteropServices.StructLayout(System.Runtime.InteropServices.LayoutKind.Explicit, Size = 0x18)]
    struct RawEntry
    {
        [System.Runtime.InteropServices.FieldOffset(0)] public long VirtualOffset;
        [System.Runtime.InteropServices.FieldOffset(8)] public long PhysicalOffset;
        [System.Runtime.InteropServices.FieldOffset(0x10)] public byte Type;
        [System.Runtime.InteropServices.FieldOffset(0x14)] public int PhysicalSize;
    }

    // Reads the decompressed storage LibHac exposes and compares it with the uncompressed hacBrewPack RomFS.
    static void RandomAccessTest(Nca program, IStorage plain, int randomReads)
    {
        var storage = program.OpenStorage(NcaSectionType.Data, IntegrityCheckLevel.ErrorOnInvalid);
        storage.GetSize(out long size).ThrowIfFailure();
        plain.GetSize(out long plainSize).ThrowIfFailure();
        Console.WriteLine($"decompressed size 0x{size:x}, reference RomFS size 0x{plainSize:x}");
        if (size != plainSize)
            Fail("virtual size differs from the uncompressed RomFS");

        var expected = new byte[8 * 1024 * 1024];
        var actual = new byte[expected.Length];

        void Compare(long offset, int count, string what)
        {
            plain.Read(offset, expected.AsSpan(0, count)).ThrowIfFailure();
            actual.AsSpan(0, count).Fill(0xAA);
            var result = storage.Read(offset, actual.AsSpan(0, count));
            if (result.IsFailure())
                throw new Exception($"{what}: read 0x{offset:x}+0x{count:x} failed with {result}");
            if (!expected.AsSpan(0, count).SequenceEqual(actual.AsSpan(0, count)))
                throw new Exception($"{what}: read 0x{offset:x}+0x{count:x} returned wrong data");
        }

        int reads = 0;
        foreach (int chunk in new[] { 0x10000, 0x4000, 0x1000, 777, 0x300000 })
        {
            if (chunk < 0x1000 && size > 64 * 1024 * 1024)
                continue;
            for (long o = 0; o < size; o += chunk)
            {
                Compare(o, (int)Math.Min(chunk, size - o), "sequential");
                reads++;
            }
            Console.WriteLine($"  sequential reads of 0x{chunk:x}: ok");
        }

        var rng = new Random(0x1234);
        for (int i = 0; i < randomReads; i++)
        {
            long offset = rng.NextInt64(size);
            int max = rng.Next(4) switch { 0 => 64, 1 => 0x4000, 2 => 0x20000, _ => 0x400000 };
            int count = (int)Math.Min(rng.Next(1, max + 1), size - offset);
            Compare(offset, count, "random");
            reads++;
        }
        Console.WriteLine($"  {randomReads} random reads: ok");

        // Straddle the very end of the image too.
        Compare(size - 1, 1, "end");
        Compare(Math.Max(size - 0x12345, 0), (int)Math.Min(0x12345, size), "end");
        Console.WriteLine($"Random access: {reads} reads identical to the uncompressed hacBrewPack RomFS");
    }
}
