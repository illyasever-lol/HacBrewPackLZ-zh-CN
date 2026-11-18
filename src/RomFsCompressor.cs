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
    /// <summary>Blocks that point at a piece already stored, and how much they hold.</summary>
    public long SharedBlocks { get; init; }
    public long SharedBytes { get; init; }
    /// <summary>What of that was found held again while compressing, rather than through the update, which no estimate knows of.</summary>
    public long FoundBytes { get; init; }
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
/// input before it is accepted, so a compressor fault cannot alter game data. A piece held more than once, as
/// <see cref="SharedPieces.Planner"/> finds it while the image is read, is stored once, and only pointed at where it's
/// held again once two hashes of all its bytes are seen to be the same.
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
        public readonly (ulong, ulong)[] Hashes = new (ulong, ulong)[blocks];
        /// <summary>The blocks of the plan it holds.</summary>
        public readonly List<SharedPieces.Block> Blocks = new(blocks);
        public long VirtualOffset;
        public int Bytes;
    }

    /// <summary>Where a shared piece was stored, and what it held.</summary>
    private readonly record struct StoredPiece(long Physical, int PhysicalSize, bool Compressed, (ulong, ulong) Hash);

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

    public static CompressedData Run(DataSource image, IByteSink payload, int blockSize, int level, int threads, ProgressReporter progress,
        IReadOnlyList<UpdateMap.Range>? kept = null)
    {
        int blocksPerBatch = Math.Max(threads * 32, 64) * (0x10000 / blockSize);
        var planner = new SharedPieces.Planner(image, blockSize, kept);
        var pieces = new Dictionary<int, StoredPiece>();
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
                // As many blocks as there's room for, planned as they're read.
                while (true)
                {
                    var batch = free.Take(token);
                    if (!planner.Next(batch.Input, blocksPerBatch, batch.Blocks, out batch.VirtualOffset, out batch.Bytes))
                        break;
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
                var encoder = new Lz4Encoder();
                var check = new byte[Lz4Encoder.MaxBlockSize];
                foreach (var batch in toWrite.GetConsumingEnumerable(token))
                {
                    for (int i = 0; i < batch.Blocks.Count; i++)
                    {
                        var block = batch.Blocks[i];
                        int offset = (int)(block.Offset - batch.VirtualOffset);
                        int compressed = batch.OutputLength[i];
                        if (block.Piece >= 0 && !block.First)
                        {
                            var piece = pieces[block.Piece];
                            if (piece.Hash == batch.Hashes[i])
                            {
                                writer.AddReference(block.Offset, piece.Physical, piece.PhysicalSize, piece.Compressed, block.Length, block.Found);
                                continue;
                            }
                            // Not the same after all, so it's stored as anything else is.
                            compressed = Compress(encoder, check, batch.Input, offset, block.Length, batch.Output, offset, level, block.Offset);
                        }
                        long physical = compressed > 0
                            ? writer.AddCompressed(block.Offset, batch.Output, offset, compressed)
                            : writer.AddStored(block.Offset, batch.Input, offset, block.Length);
                        if (block.Piece >= 0 && block.First)
                            pieces[block.Piece] = new StoredPiece(physical, compressed, compressed > 0, batch.Hashes[i]);
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
                Parallel.For(0, batch.Blocks.Count, options, i => CompressBlock(batch, i, level));
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

    /// <summary>Of the blocks stored where they are, every this many one is compressed to measure the rest by.</summary>
    public const int MeasureEvery = 16;

    private const double Confidence95 = 1.96;

    /// <summary>What compressing an image comes to, as <see cref="Measure"/> finds it.</summary>
    /// <param name="Stored">What the blocks stored where they are come to, compressed or not, with the padding before each compressed one.</param>
    /// <param name="Margin">Give or take, from how much the blocks compressed to measure by differ from their neighbours.</param>
    /// <param name="Held">Blocks that point at a piece stored before, each a table entry of its own, and the bytes they hold.</param>
    public readonly record struct Measurement(long Stored, double Margin, long Lz4Blocks, long RawBlocks, long Held, long HeldBytes);

    /// <summary>
    /// Reads all of the image once, as compressing does, and writes nothing. Every block the planner finds held again is counted just as
    /// compressing stores it, as a pointer at the piece, once the same two hashes agree. Of the blocks stored where they are, every
    /// <paramref name="every"/>th is compressed, counting from the start, so it's the same blocks every time, with the same encoder and
    /// level, and what those come to goes for all of them. Whatever reads the image checks it as it goes, so a damaged game fails here,
    /// before anything is written. The Switch's RomFsCompressor::measure does the same, sum for sum, so both come to the same figure.
    /// </summary>
    public static Measurement Measure(DataSource image, int blockSize, int level, int threads, ProgressReporter progress,
        IReadOnlyList<UpdateMap.Range>? kept = null, int every = MeasureEvery)
    {
        int blocksPerBatch = Math.Max(threads * 32, 64) * (0x10000 / blockSize);
        var planner = new SharedPieces.Planner(image, blockSize, kept);
        var free = new BlockingCollection<Batch>();
        var full = new BlockingCollection<Batch>();
        // One being read while the other is gone through.
        for (int i = 0; i < 2; i++)
            free.Add(new Batch(blocksPerBatch * blockSize, blocksPerBatch));
        using var cancel = CancellationTokenSource.CreateLinkedTokenSource(Ui.Token);
        var token = cancel.Token;

        var reading = Task.Factory.StartNew(() =>
        {
            try
            {
                while (true)
                {
                    var batch = free.Take(token);
                    if (!planner.Next(batch.Input, blocksPerBatch, batch.Blocks, out batch.VirtualOffset, out batch.Bytes))
                        break;
                    full.Add(batch, token);
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
                full.CompleteAdding();
            }
        }, TaskCreationOptions.LongRunning);

        var pieces = new Dictionary<int, (ulong, ulong)>();
        long counted = 0;
        long sampledStored = 0, sampledLength = 0, sampledLz4 = 0, sampledRaw = 0, notSampled = 0;
        long exactStored = 0, exactLz4 = 0, exactRaw = 0;
        long held = 0, heldBytes = 0;
        // The successive differences between the blocks compressed to measure by, in order, for the margin.
        double previous = 0, squares = 0;
        long measured = 0;
        var parallel = new ParallelOptions { MaxDegreeOfParallelism = threads, CancellationToken = token };
        try
        {
            foreach (var batch in full.GetConsumingEnumerable(token))
            {
                var blocks = batch.Blocks;
                var compress = new bool[blocks.Count];
                for (int i = 0; i < blocks.Count; i++)
                {
                    if (blocks[i].Piece < 0 || blocks[i].First)
                        compress[i] = counted++ % every == 0;
                }
                Parallel.For(0, blocks.Count, parallel, i =>
                {
                    var block = blocks[i];
                    int offset = (int)(block.Offset - batch.VirtualOffset);
                    if (block.Piece >= 0)
                    {
                        var bytes = batch.Input.AsSpan(offset, block.Length);
                        batch.Hashes[i] = (SharedPieces.Hash(bytes, 1), SharedPieces.Hash(bytes, 2));
                    }
                    batch.OutputLength[i] = compress[i] ? CompressedBlockSize(batch.Input, offset, block.Length, level) : -1;
                });

                for (int i = 0; i < blocks.Count; i++)
                {
                    var block = blocks[i];
                    if (block.Piece >= 0 && !block.First)
                    {
                        if (pieces.TryGetValue(block.Piece, out var hash) && hash == batch.Hashes[i])
                        {
                            held++;
                            heldBytes += block.Length;
                            continue;
                        }
                        // Not the same after all, so it's stored as anything else is. That's rare enough to work out on its own.
                        int size = CompressedBlockSize(batch.Input, (int)(block.Offset - batch.VirtualOffset), block.Length, level);
                        exactStored += size > 0 ? size : block.Length;
                        if (size > 0)
                            exactLz4++;
                        else
                            exactRaw++;
                        continue;
                    }
                    if (block.Piece >= 0)
                        pieces[block.Piece] = batch.Hashes[i];
                    int compressed = batch.OutputLength[i];
                    if (compressed < 0)
                    {
                        notSampled += block.Length;
                        continue;
                    }
                    long stored = compressed > 0 ? compressed : block.Length;
                    sampledStored += stored;
                    sampledLength += block.Length;
                    if (compressed > 0)
                        sampledLz4++;
                    else
                        sampledRaw++;
                    double ratio = (double)stored / block.Length;
                    if (measured > 0)
                        squares += (ratio - previous) * (ratio - previous);
                    previous = ratio;
                    measured++;
                }
                progress.Report(batch.VirtualOffset + batch.Bytes, 0);
                free.Add(batch, token);
            }
            reading.Wait(CancellationToken.None);
        }
        catch (Exception e)
        {
            cancel.Cancel();
            Rethrow(e, reading);
            throw;
        }

        // What's stored where it is goes by the blocks compressed to measure by. The margin is for a sample taken evenly across, with the
        // share of it that was compressed taken off.
        long total = sampledLength + notSampled;
        double scale = sampledLength > 0 ? (double)total / sampledLength : 0;
        double share = total > 0 ? (double)sampledLength / total : 1;
        double variance = measured > 1 ? (1 - share) / measured * squares / (2.0 * (measured - 1)) : 0;
        return new Measurement(
            exactStored + (long)(sampledStored * scale),
            Confidence95 * Math.Sqrt(variance) * total,
            exactLz4 + (long)(sampledLz4 * scale),
            exactRaw + (long)(sampledRaw * scale),
            held,
            heldBytes);
    }

    private static void CompressBlock(Batch batch, int index, int level)
    {
        var block = batch.Blocks[index];
        int offset = (int)(block.Offset - batch.VirtualOffset);
        // Two hashes the planner didn't use, so nothing points at a piece without all three agreeing.
        if (block.Piece >= 0)
        {
            var bytes = batch.Input.AsSpan(offset, block.Length);
            batch.Hashes[index] = (SharedPieces.Hash(bytes, 1), SharedPieces.Hash(bytes, 2));
        }
        // Where a piece is held again, it's only compared with what was stored.
        if (block.Piece >= 0 && !block.First)
        {
            batch.OutputLength[index] = 0;
            return;
        }
        var encoder = t_encoder ??= new Lz4Encoder();
        var check = t_check ??= new byte[Lz4Encoder.MaxBlockSize];
        batch.OutputLength[index] = Compress(encoder, check, batch.Input, offset, block.Length, batch.Output, offset, level, block.Offset);
    }

    /// <summary>The compressed size, or 0 if it's to be stored as it is.</summary>
    private static int Compress(Lz4Encoder encoder, byte[] check, byte[] input, int offset, int length, byte[] output, int outputOffset, int level, long virtualOffset)
    {
        int compressed = encoder.Compress(input, offset, length, output, outputOffset, CompressCapacity(length), level);
        if (compressed > 0)
        {
            int decoded = Lz4Decoder.Decode(output, outputOffset, compressed, check, 0, check.Length);
            if (decoded != length || !check.AsSpan(0, length).SequenceEqual(input.AsSpan(offset, length)))
                throw new ToolException($"Internal error: LZ4 round trip failed at RomFS offset 0x{virtualOffset:x}");
        }
        return Math.Max(compressed, 0);
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
        private long _sharedBlocks;
        private long _sharedBytes;
        private long _foundBytes;
        private long _storedRunEnd = -1;
        private long _storedRunSize;

        public long Position { get; private set; }

        /// <returns>Where it's stored.</returns>
        public long AddCompressed(long virtualOffset, byte[] buffer, int offset, int length)
        {
            Align(PhysicalAlignment);
            long physical = Position;
            _entries.Add(new CompressionEntry
            {
                VirtualOffset = virtualOffset, PhysicalOffset = physical, Type = CompressionType.Lz4, PhysicalSize = length,
            });
            Write(buffer, offset, length);
            _lz4Blocks++;
            _storedRunEnd = -1;
            return physical;
        }

        /// <summary>A piece stored before, held here too, found while compressing or through the update.</summary>
        public void AddReference(long virtualOffset, long physical, int physicalSize, bool compressed, int length, bool found)
        {
            _sharedBlocks++;
            _sharedBytes += length;
            _foundBytes += found ? length : 0;
            if (compressed)
            {
                _entries.Add(new CompressionEntry
                {
                    VirtualOffset = virtualOffset, PhysicalOffset = physical, Type = CompressionType.Lz4, PhysicalSize = physicalSize,
                });
                _storedRunEnd = -1;
                return;
            }
            // Stored as it is, it carries on a run of stored bytes that ends where it starts, as anything stored does.
            if (_storedRunEnd == physical && _storedRunSize + length <= MaxStoredRun)
            {
                _storedRunSize += length;
            }
            else
            {
                _entries.Add(new CompressionEntry
                {
                    VirtualOffset = virtualOffset, PhysicalOffset = physical, Type = CompressionType.None, PhysicalSize = -1,
                });
                _storedRunSize = length;
            }
            _storedRunEnd = physical + length;
        }

        /// <returns>Where it's stored.</returns>
        public long AddStored(long virtualOffset, byte[] buffer, int offset, int length)
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
            long physical = Position;
            Write(buffer, offset, length);
            _storedRunEnd = Position;
            _storedBlocks++;
            return physical;
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
                SharedBlocks = _sharedBlocks,
                SharedBytes = _sharedBytes,
                FoundBytes = _foundBytes,
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
