using System.Security.Cryptography;

namespace HacBrewPackLz;

/// <summary>Builds an NSP from folders.</summary>
internal sealed class Builder
{
    private readonly Options _options;
    private readonly NcaWriter _writer;

    private NspWriter _nsp = null!;

    public string? OutputPath { get; private set; }

    public Builder(Options options, Keyset keyset)
    {
        _options = options;
        int generationIndex = Math.Max(options.KeyGeneration - 1, 0);
        var keyAreaKey = keyset.TryKeyAreaKeyApplication(generationIndex)
            ?? throw new ToolException($"key_area_key_application_{generationIndex:x2} (for keygeneration {options.KeyGeneration}) is not present in the keyset file");
        _writer = new NcaWriter(keyset.HeaderKey, keyAreaKey, options.KeyAreaKey, options.KeyGeneration, options.SdkVersion);
    }

    public int Run()
    {
        using RSA? signer = _options.NoSignNcaSig2 ? null : RSA.Create(2048);
        byte[]? modulus = signer?.ExportParameters(false).Modulus;

        var npdm = Npdm.Load(Path.Combine(_options.ExefsDir, "main.npdm"), _options.TitleId, modulus, out ulong titleId);
        var nacp = Nacp.Load(Path.Combine(_options.ControlDir, "control.nacp"), _options.TitleId, _options.TitleName,
            _options.TitlePublisher, !_options.NoPatchNacpLogo);
        CheckDirectories(_options);

        string nspPath = _options.OutputPath ?? Path.Combine(_options.NspDir, $"{titleId:x16}.nsp");
        string nspDir = Path.GetDirectoryName(Path.GetFullPath(nspPath))!;
        Directory.CreateDirectory(nspDir);
        RoomCheck.ForBuild(_options, nspDir);

        int contentCount = 2 + (_options.HtmlDocDir is null ? 0 : 1) + (_options.LegalInfoDir is null ? 0 : 1);

        Ui.Info($"Title ID: {titleId:x16}");
        CompressionSummary? summary = null;
        var records = new List<ContentRecord>();
        using (_nsp = new NspWriter(nspPath, contentCount))
        {
            records.Add(new ContentRecord(WriteProgramNca(titleId, npdm, signer, ref summary), Cnmt.ContentTypeProgram));
            records.Add(new ContentRecord(WriteRomFsNca("Control", NcaContentType.Control, _options.ControlDir, titleId,
                new Dictionary<string, byte[]> { ["control.nacp"] = nacp }), Cnmt.ContentTypeControl));
            if (_options.HtmlDocDir is { } html)
                records.Add(new ContentRecord(WriteRomFsNca("HtmlDoc", NcaContentType.Manual, html, titleId, null), Cnmt.ContentTypeHtmlDocument));
            if (_options.LegalInfoDir is { } legal)
                records.Add(new ContentRecord(WriteRomFsNca("LegalInfo", NcaContentType.Manual, legal, titleId, null), Cnmt.ContentTypeLegalInformation));

            uint requiredSystemVersion = _options.RequiredSystemVersion ?? (summary is null ? 0 : Options.CompressionMinSystemVersion);
            var cnmt = Cnmt.Build(titleId, _options.TitleVersion ?? 0, requiredSystemVersion, records);
            WriteMetaNca(titleId, cnmt);

            _nsp.Finish();
            if (_options.KeepNcaDir)
                _nsp.ExportNcas(_options.NcaDir);
        }

        OutputPath = nspPath;
        PrintSummary(titleId, nspPath, summary);
        return 0;
    }

    private NcaInfo WriteProgramNca(ulong titleId, byte[] npdm, RSA? signer, ref CompressionSummary? summary)
    {
        var sections = new NcaSection?[3];

        Ui.Detail("正在计算 ExeFS 哈希");
        sections[0] = NcaSections.Pfs0(BuildPfs0(_options.ExefsDir, new Dictionary<string, byte[]> { ["main.npdm"] = npdm }),
            NcaSections.ExefsHashBlockSize, !_options.Plaintext);

        if (!_options.NoLogo)
            sections[2] = NcaSections.Pfs0(BuildPfs0(_options.LogoDir, null), NcaSections.LogoHashBlockSize, encrypted: false);

        if (_options.NoRomfs)
            return _nsp.WriteNca(_writer, "Program", NcaContentType.Program, titleId, sections, signer);

        var image = RomFsBuilder.Build(_options.RomfsDir);
        if (!_options.Compress)
        {
            var hashing = new ProgressReporter("正在计算 RomFS 哈希", image.Length);
            sections[1] = NcaSections.RomFs(image, !_options.Plaintext, hashing);
            hashing.Done();
            return _nsp.WriteNca(_writer, "Program", NcaContentType.Program, titleId, sections, signer);
        }

        var builder = _nsp.BeginNca(_writer);
        builder.Add(0, sections[0]!);
        var progress = new ProgressReporter("正在压缩 RomFS", image.Length, showOutputSize: true);
        var compressed = builder.AddCompressed(1, image, _options.BlockSize, _options.Level, _options.Threads, !_options.Plaintext, progress);
        progress.Done();
        if (sections[2] is { } logo)
            builder.Add(2, logo);

        summary = new CompressionSummary(compressed.VirtualSize, compressed.DataSize, compressed.Lz4Blocks, compressed.StoredBlocks, compressed.EntryCount);
        return _nsp.EndNca(builder, NcaContentType.Program, titleId, signer);
    }

    private NcaInfo WriteRomFsNca(string label, NcaContentType type, string directory, ulong titleId, IReadOnlyDictionary<string, byte[]>? replacements)
    {
        var sections = new NcaSection?[] { NcaSections.RomFs(RomFsBuilder.Build(directory, replacements), !_options.Plaintext, null) };
        return _nsp.WriteNca(_writer, label, type, titleId, sections, null);
    }

    private void WriteMetaNca(ulong titleId, byte[] cnmt)
    {
        var pfs0 = Pfs0Builder.Build([(Cnmt.FileName(titleId), new BytesSource(cnmt))]);
        var sections = new NcaSection?[] { NcaSections.Pfs0(pfs0, NcaSections.MetaHashBlockSize, !_options.Plaintext) };
        _nsp.WriteNca(_writer, "Meta", NcaContentType.Meta, titleId, sections, null, ".cnmt.nca");
    }

    public static ConcatSource BuildPfs0(string directory, IReadOnlyDictionary<string, byte[]>? replacements)
    {
        var files = new DirectoryInfo(directory).EnumerateFiles()
            .OrderBy(f => f.Name, StringComparer.OrdinalIgnoreCase)
            .Select(f => (f.Name, replacements is not null && replacements.TryGetValue(f.Name, out var data)
                ? (DataSource)new BytesSource(data)
                : new FileSource(f.FullName, f.Length)));
        return Pfs0Builder.Build(files);
    }

    public static void CheckDirectories(Options options)
    {
        if (!options.NoRomfs && !Directory.Exists(options.RomfsDir))
            throw new ToolException($"RomFS directory '{options.RomfsDir}' not found (use --romfsdir or --noromfs)");
        if (!options.NoLogo && !Directory.Exists(options.LogoDir))
            throw new ToolException($"Logo directory '{options.LogoDir}' not found (use --logodir or --nologo)");
        foreach (var dir in new[] { options.HtmlDocDir, options.LegalInfoDir })
        {
            if (dir is not null && !Directory.Exists(dir))
                throw new ToolException($"Directory '{dir}' not found");
        }
    }

    private void PrintSummary(ulong titleId, string nspPath, CompressionSummary? summary)
    {
        Ui.Info("");
        Ui.Heading("摘要");
        Ui.Info("");
        Ui.Info($"Title ID: {titleId:x16}");
        uint sdk = _options.SdkVersion;
        Ui.Info($"SDK 版本: {(sdk >> 24) & 0xFF}.{(sdk >> 16) & 0xFF}.{(sdk >> 8) & 0xFF}.{sdk & 0xFF}");
        Ui.Info($"Section Crypto Type: {(_options.Plaintext ? "Plaintext" : "Regular Crypto")}");
        Ui.Info($"密钥世代: {FirmwareVersions.Describe(_options.KeyGeneration)} (NCA 头部写入 0x{_options.KeyGeneration:X})");
        Ui.Info($"Key area key 2: {Convert.ToHexString(_options.KeyAreaKey)}");
        Ui.Info($"Program NCA RomFS Section: {(_options.NoRomfs ? "No" : summary is null ? "Yes (not compressed)" : "Yes (LZ4 compressed)")}");
        Ui.Info($"Program NCA Logo Section: {(_options.NoLogo ? "No" : "Yes")}");
        Ui.Info($"HtmlDoc NCA: {(_options.HtmlDocDir is null ? "No" : "Yes")}");
        Ui.Info($"LegalInfo NCA: {(_options.LegalInfoDir is null ? "No" : "Yes")}");
        if (summary is { } s)
        {
            Ui.Info($"RomFS size: {ProgressReporter.Format(s.VirtualSize)}");
            Ui.Info($"RomFS stored as: {ProgressReporter.Format(s.DataSize)} ({s.DataSize * 100.0 / s.VirtualSize:0.0}%, saved {ProgressReporter.Format(Math.Max(s.VirtualSize - s.DataSize, 0))})");
            Ui.Info($"RomFS blocks: {s.Lz4Blocks} LZ4, {s.StoredBlocks} stored raw ({s.EntryCount} table entries, {ProgressReporter.Format(_options.BlockSize)} block size, level {_options.Level})");
        }
        Ui.Info($"Created NSP: {nspPath} ({ProgressReporter.Format(new FileInfo(nspPath).Length)})");
    }

    private sealed record CompressionSummary(long VirtualSize, long DataSize, long Lz4Blocks, long StoredBlocks, int EntryCount);
}
