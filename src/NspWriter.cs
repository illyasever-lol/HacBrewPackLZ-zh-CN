using System.Buffers.Binary;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;

namespace HacBrewPackLz;

/// <summary>
/// Writes an NSP in one pass: a reserved PFS0 header, then each NCA straight into the file, then the real header.
/// NCA names have a fixed length, so the header size is known before their hashes are. The file only gets its final
/// name once it is complete.
/// </summary>
internal sealed class NspWriter : IDisposable
{
    private const int PfsEntrySize = 0x18;
    private const int NcaNameLength = 36;      // 32 hex digits + ".nca"
    private const int MetaNcaNameLength = 41;  // 32 hex digits + ".cnmt.nca"

    private readonly string _path;
    private readonly string _partPath;
    private readonly FileStream _file;
    private readonly long _dataStart;
    private readonly int _expectedFiles;
    private readonly List<(string name, long offset, long size)> _files = [];
    private readonly List<PosixSignalRegistration> _signals = [];
    private bool _finished;

    /// <param name="contentCount">How many content NCAs will be written, not counting the meta NCAs.</param>
    /// <param name="metaCount">How many CNMT meta NCAs will be written: one per title packed into the NSP.</param>
    public NspWriter(string path, int contentCount, int metaCount = 1)
    {
        _path = path;
        _partPath = path + ".part";
        _expectedFiles = contentCount + metaCount;
        _dataStart = HeaderSize(contentCount, metaCount);

        _file = new FileStream(_partPath, FileMode.Create, FileAccess.ReadWrite, FileShare.None, 1 << 20);
        _file.SetLength(_dataStart);
        _file.Position = _dataStart;

        // On Windows these also cover Ctrl-Break and closing the console window, so a stopped build leaves no partial file.
        foreach (var signal in new[] { PosixSignal.SIGINT, PosixSignal.SIGQUIT, PosixSignal.SIGHUP, PosixSignal.SIGTERM })
            _signals.Add(PosixSignalRegistration.Create(signal, _ => Abandon()));
    }

    public static long HeaderSize(int contentCount, int metaCount = 1)
    {
        int nameTable = Align(contentCount * (NcaNameLength + 1) + metaCount * (MetaNcaNameLength + 1), 0x20);
        return 0x10 + (contentCount + metaCount) * PfsEntrySize + nameTable;
    }

    public NcaInfo WriteNca(NcaWriter writer, string label, NcaContentType type, ulong titleId, NcaSection?[] sections, RSA? signer,
        string extension = ".nca", int contentIndex = 0, uint? sdkVersion = null)
    {
        long total = sections.Where(s => s is not null).Sum(s => (s!.Data.Length + 0x1FF) & ~0x1FFL);
        var progress = new ProgressReporter($"Writing {label} NCA", total);
        long start = _file.Position;
        var info = writer.Write(_file, type, titleId, sections, signer, progress, contentIndex, sdkVersion);
        progress.Done();
        _files.Add((info.Id + extension, start - _dataStart, info.Size));
        Ui.Detail($"  {info.Id}{extension} ({ProgressReporter.Format(info.Size)})");
        return info;
    }

    /// <summary>For an NCA that's compressed on its way into the file. It starts right here, and gets its header at the end.</summary>
    public NcaBuilder BeginNca(NcaWriter writer) => new(writer, _file);

    public NcaInfo EndNca(NcaBuilder builder, NcaContentType type, ulong titleId, RSA? signer, int contentIndex = 0, uint? sdkVersion = null)
    {
        var info = builder.Finish(type, titleId, signer, contentIndex, sdkVersion);
        _files.Add((info.Id + ".nca", builder.Start - _dataStart, info.Size));
        Ui.Detail($"  {info.Id}.nca ({ProgressReporter.Format(info.Size)})");
        return info;
    }

    public void Finish()
    {
        if (_files.Count != _expectedFiles)
            throw new InvalidOperationException($"NSP reserved {_expectedFiles} files but received {_files.Count}");

        WriteHeader();
        _file.Flush(true);
        _file.Dispose();
        File.Move(_partPath, _path, overwrite: true);
        _finished = true;
    }

    /// <summary>Copies the finished NCAs out to a folder. Only valid after Finish.</summary>
    public void ExportNcas(string directory)
    {
        Directory.CreateDirectory(directory);
        using var nsp = new FileStream(_path, FileMode.Open, FileAccess.Read, FileShare.Read, 1 << 20);
        var buffer = new byte[1 << 20];
        foreach (var (name, offset, size) in _files)
        {
            using var output = new FileStream(Path.Combine(directory, name), FileMode.Create, FileAccess.Write, FileShare.None, 1 << 20);
            nsp.Position = _dataStart + offset;
            for (long left = size; left > 0;)
            {
                int n = nsp.Read(buffer, 0, (int)Math.Min(buffer.Length, left));
                if (n <= 0)
                    throw new ToolException($"Unexpected end of '{_path}'");
                output.Write(buffer, 0, n);
                left -= n;
            }
        }
    }

    public void Dispose()
    {
        foreach (var signal in _signals)
            signal.Dispose();
        _file.Dispose();
        if (!_finished)
            TryDelete(_partPath);
    }

    private void Abandon()
    {
        try
        {
            _file.Dispose();
        }
        catch (Exception e) when (e is IOException or ObjectDisposedException)
        {
            // Another thread was writing to it. The process is exiting either way.
        }
        TryDelete(_partPath);
        Environment.Exit(130);
    }

    private void WriteHeader()
    {
        using var strings = new MemoryStream();
        var nameOffsets = new int[_files.Count];
        for (int i = 0; i < _files.Count; i++)
        {
            nameOffsets[i] = (int)strings.Position;
            strings.Write(Encoding.ASCII.GetBytes(_files[i].name));
            strings.WriteByte(0);
        }

        int nameTable = (int)(_dataStart - 0x10 - _files.Count * PfsEntrySize);
        if (strings.Length > nameTable)
            throw new InvalidOperationException("PFS0 名称表预留空间不足");

        var header = new byte[_dataStart];
        var h = header.AsSpan();
        "PFS0"u8.CopyTo(h);
        BinaryPrimitives.WriteInt32LittleEndian(h[4..], _files.Count);
        BinaryPrimitives.WriteInt32LittleEndian(h[8..], nameTable);
        for (int i = 0; i < _files.Count; i++)
        {
            var e = h[(0x10 + i * PfsEntrySize)..];
            BinaryPrimitives.WriteInt64LittleEndian(e, _files[i].offset);
            BinaryPrimitives.WriteInt64LittleEndian(e[8..], _files[i].size);
            BinaryPrimitives.WriteInt32LittleEndian(e[0x10..], nameOffsets[i]);
        }
        strings.ToArray().CopyTo(h[(0x10 + _files.Count * PfsEntrySize)..]);

        _file.Flush();
        _file.Position = 0;
        _file.Write(header);
    }

    private static void TryDelete(string path)
    {
        try
        {
            File.Delete(path);
        }
        catch (IOException)
        {
        }
        catch (UnauthorizedAccessException)
        {
        }
    }

    private static int Align(int value, int alignment) => (value + alignment - 1) & ~(alignment - 1);
}
