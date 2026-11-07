using System.Collections.Concurrent;

namespace HacBrewPackLz;

internal sealed class CompressedData
{
    public required long VirtualSize { get; init; }
    public required long TableOffset { get; init; }
    public required long TableSize { get; init; }
    public required long DataSize { get; init; }
    public required int EntryCount { get; init; }
    public required long Lz4Blocks { get; init; }
    public required long StoredBlocks { get; init; }
    public required IvfcResult Ivfc { get; init; }
}

/// <summary>Where plain bytes go, in order, as they're made.</summary>
internal interface IByteSink
{
    void Write(byte[] buffer, int offset, int count);
}

/// <summary>
/// Cuts an image into fixed size blocks, LZ4 compresses them in parallel and passes on the result plus its BKTR
/// table. Blocks that do not shrink are stored raw. Every compressed block is decoded again and compared with its
/// input before it is accepted, so a compressor fault cannot alter game data.
/// </summary>
internal static class RomFsCompressor
{
    /// <summary>fs::CompressedStorage reads compressed entries through a 64 KiB buffer and rejects anything larger.</summary>
    public const int MaxPhysicalBlockSize = 0x10000;

    /// <summary>CompressionBlockAlignment in fssystem: physical offsets of compressed entries must be aligned to this.</summary>
    private const int PhysicalAlignment = 0x10;

    private const int MaxStoredRun = 0x4000000;
    private const int MinimumSaving = 64;
    private const int BatchCount = 4;

    private sealed class Batch(int capacity, int blocks)
    {
        public readonly byte[] Input = new byte[capacity];
        public readonly byte[] Output = new byte[capacity];
        public readonly int[] OutputLength = new int[blocks];
        public long VirtualOffset;
        public int Bytes;
    }

    [ThreadStatic] private static Lz4Encoder? t_encoder;
    [ThreadStatic] private static byte[]? t_check;

    /// <summary>Average padding in front of a compressed block, from the 0x10 alignment.</summary>
    public const int AveragePadding = 8;

    /// <summary>
    /// A block has to save more than it costs: splitting a raw run adds table entries and padding, and every read has
    /// to decode it. Anything that misses this capacity comes back as -1 and is stored raw.
    /// </summary>
    public static int CompressCapacity(int length) => length - Math.Max(length / 256, MinimumSaving);

    /// <summary>What a block would take up as a compressed entry with its padding, or 0 if it would be stored raw.</summary>
    public static int CompressedBlockSize(byte[] input, int offset, int length, int level)
    {
        var encoder = t_encoder ??= new Lz4Encoder();
        var scratch = t_check ??= new byte[Lz4Encoder.MaxBlockSize];
        int compressed = encoder.Compress(input, offset, length, scratch, 0, CompressCapacity(length), level);
        return compressed > 0 ? compressed + AveragePadding : 0;
    }

    public static CompressedData Run(DataSource image, IByteSink payload, int blockSize, int level, int threads, ProgressReporter progress)
    {
        int blocksPerBatch = Math.Max(threads * 32, 64) * (0x10000 / blockSize);
        var free = new BlockingCollection<Batch>();
        var toCompress = new BlockingCollection<Batch>();
        var toWrite = new BlockingCollection<Batch>();
        for (int i = 0; i < BatchCount; i++)
            free.Add(new Batch(blocksPerBatch * blockSize, blocksPerBatch));

        var writer = new PayloadWriter(payload);
        using var cancel = new CancellationTokenSource();
        var token = cancel.Token;
        long total = image.Length;

        var reading = Task.Factory.StartNew(() =>
        {
            try
            {
                for (long position = 0; position < total;)
                {
                    var batch = free.Take(token);
                    int n = (int)Math.Min(total - position, batch.Input.Length);
                    image.Read(position, batch.Input, 0, n);
                    batch.VirtualOffset = position;
                    batch.Bytes = n;
                    position += n;
                    toCompress.Add(batch, token);
                }
                image.Release();
            }
            catch
            {
                cancel.Cancel();
                throw;
            }
            finally
            {
                toCompress.CompleteAdding();
            }
        }, TaskCreationOptions.LongRunning);

        var writing = Task.Factory.StartNew(() =>
        {
            try
            {
                foreach (var batch in toWrite.GetConsumingEnumerable(token))
                {
                    for (int i = 0, offset = 0; offset < batch.Bytes; i++, offset += blockSize)
                    {
                        int length = Math.Min(blockSize, batch.Bytes - offset);
                        long virtualOffset = batch.VirtualOffset + offset;
                        if (batch.OutputLength[i] > 0)
                            writer.AddCompressed(virtualOffset, batch.Output, offset, batch.OutputLength[i]);
                        else
                            writer.AddStored(virtualOffset, batch.Input, offset, length);
                    }
                    progress.Report(batch.VirtualOffset + batch.Bytes, writer.Position);
                    free.Add(batch, token);
                }
            }
            catch
            {
                cancel.Cancel();
                throw;
            }
        }, TaskCreationOptions.LongRunning);

        try
        {
            var options = new ParallelOptions { MaxDegreeOfParallelism = threads, CancellationToken = token };
            foreach (var batch in toCompress.GetConsumingEnumerable(token))
            {
                int blocks = (batch.Bytes + blockSize - 1) / blockSize;
                Parallel.For(0, blocks, options, i => CompressBlock(batch, i, blockSize, level));
                toWrite.Add(batch, token);
            }
            toWrite.CompleteAdding();
            reading.Wait(CancellationToken.None);
            writing.Wait(CancellationToken.None);
        }
        catch (Exception e)
        {
            cancel.Cancel();
            toWrite.CompleteAdding();
            Rethrow(e, reading, writing);
            throw;
        }

        return writer.Finish(total);
    }

    private static void CompressBlock(Batch batch, int index, int blockSize, int level)
    {
        int offset = index * blockSize;
        int length = Math.Min(blockSize, batch.Bytes - offset);
        var encoder = t_encoder ??= new Lz4Encoder();
        var check = t_check ??= new byte[Lz4Encoder.MaxBlockSize];

        int compressed = encoder.Compress(batch.Input, offset, length, batch.Output, offset, CompressCapacity(length), level);
        if (compressed > 0)
        {
            int decoded = Lz4Decoder.Decode(batch.Output, offset, compressed, check, 0, check.Length);
            if (decoded != length || !check.AsSpan(0, length).SequenceEqual(batch.Input.AsSpan(offset, length)))
                throw new ToolException($"Internal error: LZ4 round trip failed at RomFS offset 0x{batch.VirtualOffset + offset:x}");
        }
        batch.OutputLength[index] = Math.Max(compressed, 0);
    }

    private static void Rethrow(Exception thrown, params Task[] tasks)
    {
        foreach (var task in tasks)
        {
            try
            {
                task.Wait(CancellationToken.None);
            }
            catch (AggregateException e)
            {
                foreach (var inner in e.Flatten().InnerExceptions)
                {
                    if (inner is not OperationCanceledException)
                        throw new ToolException(inner.Message);
                }
            }
        }
        if (thrown is AggregateException aggregate)
        {
            foreach (var inner in aggregate.Flatten().InnerExceptions)
            {
                if (inner is not OperationCanceledException)
                    throw new ToolException(inner.Message);
            }
            throw new OperationCanceledException();
        }
    }

    private sealed class PayloadWriter(IByteSink payload)
    {
        private static readonly byte[] Zeros = new byte[IvfcBuilder.BlockSize];

        private readonly IvfcBuilder _ivfc = new();
        private readonly List<CompressionEntry> _entries = [];
        private long _lz4Blocks;
        private long _storedBlocks;
        private long _storedRunEnd = -1;
        private long _storedRunSize;

        public long Position { get; private set; }

        public void AddCompressed(long virtualOffset, byte[] buffer, int offset, int length)
        {
            Align(PhysicalAlignment);
            _entries.Add(new CompressionEntry
            {
                VirtualOffset = virtualOffset, PhysicalOffset = Position, Type = CompressionType.Lz4, PhysicalSize = length,
            });
            Write(buffer, offset, length);
            _lz4Blocks++;
        }

        public void AddStored(long virtualOffset, byte[] buffer, int offset, int length)
        {
            // Consecutive stored blocks share one entry: raw entries are random accessible and have no size limit.
            // As in retail NCAs, a raw entry's physical size is -1, because the firmware never reads it.
            if (_storedRunEnd == Position && _storedRunSize + length <= MaxStoredRun)
            {
                _storedRunSize += length;
            }
            else
            {
                Align(PhysicalAlignment);
                _entries.Add(new CompressionEntry
                {
                    VirtualOffset = virtualOffset, PhysicalOffset = Position, Type = CompressionType.None, PhysicalSize = -1,
                });
                _storedRunSize = length;
            }
            Write(buffer, offset, length);
            _storedRunEnd = Position;
            _storedBlocks++;
        }

        public CompressedData Finish(long virtualSize)
        {
            Align(IvfcBuilder.BlockSize);
            long tableOffset = Position;
            var table = BucketTree.Build(_entries, virtualSize);
            Write(table, 0, table.Length);

            return new CompressedData
            {
                VirtualSize = virtualSize,
                TableOffset = tableOffset,
                TableSize = table.Length,
                DataSize = Position,
                EntryCount = _entries.Count,
                Lz4Blocks = _lz4Blocks,
                StoredBlocks = _storedBlocks,
                Ivfc = _ivfc.Finish(Position, dataFirst: true),
            };
        }

        private void Align(int alignment)
        {
            int padding = (int)(-Position & (alignment - 1));
            if (padding > 0)
                Write(Zeros, 0, padding);
        }

        private void Write(byte[] buffer, int offset, int length)
        {
            payload.Write(buffer, offset, length);
            _ivfc.Add(buffer, offset, length);
            Position += length;
        }
    }
}
