using Microsoft.Win32.SafeHandles;

namespace HacBrewPackLz;

internal sealed class ToolException(string message) : Exception(message);

/// <summary>
/// Read-only random access bytes. Everything that ends up in an NCA is described as a chain of these, so
/// source files are only ever opened for reading and are never copied or edited on disk.
/// </summary>
internal abstract class DataSource
{
    public abstract long Length { get; }
    public abstract void Read(long offset, byte[] buffer, int bufferOffset, int count);
    public virtual void Release() { }

    /// <summary>Whether several threads can read it at the same time.</summary>
    public virtual bool ReadsAtOnce => false;
}

internal sealed class BytesSource(byte[] data) : DataSource
{
    public override long Length => data.Length;
    public override bool ReadsAtOnce => true;

    public override void Read(long offset, byte[] buffer, int bufferOffset, int count) =>
        Buffer.BlockCopy(data, checked((int)offset), buffer, bufferOffset, count);
}

internal sealed class ZeroSource(long length) : DataSource
{
    public override long Length => length;
    public override bool ReadsAtOnce => true;

    public override void Read(long offset, byte[] buffer, int bufferOffset, int count) =>
        Array.Clear(buffer, bufferOffset, count);
}

internal sealed class FileSource(string path, long length) : DataSource
{
    private readonly object _opening = new();
    private SafeFileHandle? _handle;

    public override long Length => length;
    public override bool ReadsAtOnce => true;

    public override void Read(long offset, byte[] buffer, int bufferOffset, int count)
    {
        SafeFileHandle handle;
        lock (_opening)
            handle = _handle ??= Open();
        int done = 0;
        while (done < count)
        {
            int n = RandomAccess.Read(handle, buffer.AsSpan(bufferOffset + done, count - done), offset + done);
            if (n <= 0)
                throw new ToolException($"'{path}' ended early, it changed while it was being read");
            done += n;
        }
    }

    public override void Release()
    {
        lock (_opening)
        {
            _handle?.Dispose();
            _handle = null;
        }
    }

    private SafeFileHandle Open()
    {
        var handle = File.OpenHandle(path, FileMode.Open, FileAccess.Read, FileShare.Read | FileShare.Delete,
            FileOptions.SequentialScan);
        long actual = RandomAccess.GetLength(handle);
        if (actual != length)
        {
            handle.Dispose();
            throw new ToolException($"'{path}' changed size while building (expected {length}, found {actual})");
        }
        return handle;
    }
}

/// <summary>A file that is already open, read through its own handle without moving any file position.</summary>
internal sealed class HandleSource(SafeFileHandle handle, long length) : DataSource
{
    public override long Length => length;
    public override bool ReadsAtOnce => true;

    public override void Read(long offset, byte[] buffer, int bufferOffset, int count)
    {
        int done = 0;
        while (done < count)
        {
            int n = RandomAccess.Read(handle, buffer.AsSpan(bufferOffset + done, count - done), offset + done);
            if (n <= 0)
                throw new ToolException("文件提前结束");
            done += n;
        }
    }
}

internal sealed class SubSource(DataSource inner, long start, long length) : DataSource
{
    public override long Length => length;
    public override bool ReadsAtOnce => inner.ReadsAtOnce;

    public override void Read(long offset, byte[] buffer, int bufferOffset, int count)
    {
        if (offset < 0 || offset + count > length)
            throw new ArgumentOutOfRangeException(nameof(offset));
        inner.Read(start + offset, buffer, bufferOffset, count);
    }
}

internal sealed class ConcatSource : DataSource
{
    private readonly List<DataSource> _parts = [];
    private readonly List<long> _starts = [];
    private long _length;
    private int _cursor;

    public override long Length => _length;

    public ConcatSource Add(DataSource part)
    {
        if (part.Length == 0)
            return this;
        _parts.Add(part);
        _starts.Add(_length);
        _length += part.Length;
        return this;
    }

    public override void Read(long offset, byte[] buffer, int bufferOffset, int count)
    {
        if (count == 0)
            return;
        if (offset < 0 || offset + count > _length)
            throw new ArgumentOutOfRangeException(nameof(offset));

        int index = Locate(offset);
        while (count > 0)
        {
            var part = _parts[index];
            long inner = offset - _starts[index];
            int n = (int)Math.Min(count, part.Length - inner);
            part.Read(inner, buffer, bufferOffset, n);
            offset += n;
            bufferOffset += n;
            count -= n;
            if (inner + n == part.Length)
            {
                // Sequential reads never come back, so at most one source file stays open at a time.
                part.Release();
                index++;
            }
        }
        _cursor = Math.Min(index, _parts.Count - 1);
    }

    public override void Release()
    {
        foreach (var part in _parts)
            part.Release();
    }

    private int Locate(long offset)
    {
        if (_cursor < _parts.Count && offset >= _starts[_cursor] && offset < _starts[_cursor] + _parts[_cursor].Length)
            return _cursor;
        int i = _starts.BinarySearch(offset);
        return i >= 0 ? i : ~i - 1;
    }
}
