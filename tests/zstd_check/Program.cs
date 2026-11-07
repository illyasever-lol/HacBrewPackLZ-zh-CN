using System.Diagnostics;
using HacBrewPackLz;

// Checks the Zstandard decoder against files made by the real thing, and against damaged copies of them.
//   zstd_check <folder>            every <name>.zst must decode to <name>.raw, whole and as a stream in odd sized reads
//   zstd_check <folder> --fuzz N   N damaged copies of each must decode or be refused, without crashing or overrunning

if (args.Length == 2 && args[0] == "--bench")
{
    byte[] data = File.ReadAllBytes(args[1]);
    var stream = new ZstdStream(new BytesSource(data), 0, data.Length);
    for (int round = 0; round < 3; round++)
    {
        stream.Rewind();
        var timer = Stopwatch.StartNew();
        try
        {
            while (stream.Advance())
            {
            }
        }
        catch (ToolException)
        {
            // A slice of a stream just stops. What counts is how far it got and how quickly.
        }
        Console.WriteLine($"decoded {stream.Position / 1048576.0:0} MiB in {timer.Elapsed.TotalSeconds:0.00} s = {stream.Position / 1048576.0 / timer.Elapsed.TotalSeconds:0} MiB/s");
    }
    return 0;
}

string folder = args[0];
int fuzz = args.Length > 2 && args[1] == "--fuzz" ? int.Parse(args[2]) : 0;
int failures = 0;
int files = 0;
long bytes = 0;
var clock = new Stopwatch();

foreach (string path in Directory.EnumerateFiles(folder, "*.zst").OrderBy(p => p, StringComparer.Ordinal))
{
    files++;
    byte[] compressed = File.ReadAllBytes(path);
    byte[] expected = File.ReadAllBytes(Path.ChangeExtension(path, ".raw"));
    string name = Path.GetFileName(path);

    if (fuzz > 0)
    {
        failures += Fuzz(compressed, expected.Length, fuzz, name);
        continue;
    }

    clock.Start();
    var decoder = new ZstdDecoder();
    var whole = new byte[expected.Length];
    int made = decoder.Decompress(compressed, 0, compressed.Length, whole, 0, whole.Length);
    clock.Stop();
    bytes += expected.Length;
    if (made != expected.Length || !whole.AsSpan().SequenceEqual(expected))
    {
        Console.WriteLine($"FAIL whole: {name} made {made} of {expected.Length}, differs at {FirstDifference(whole, expected, made)}");
        failures++;
        continue;
    }

    var stream = new ZstdStream(new BytesSource(compressed), 0, compressed.Length);
    var streamed = new byte[expected.Length];
    var random = new Random(expected.Length);
    long position = 0;
    while (position < expected.Length)
    {
        while (position >= stream.Position)
        {
            if (!stream.Advance())
                break;
        }
        if (position >= stream.Position)
            break;
        int want = (int)Math.Min(random.Next(1, 300000), expected.Length - position);
        position += stream.Copy(position, streamed, (int)position, want);
    }

    // A frame may end with an empty block, but nothing more may come out of it.
    while (stream.Advance())
    {
    }
    bool ended = stream.Position == expected.Length;
    if (position != expected.Length || !ended || !streamed.AsSpan().SequenceEqual(expected))
    {
        Console.WriteLine($"FAIL stream: {name} reached {position} of {expected.Length}, ended {ended}");
        failures++;
        continue;
    }

    // Reading back what's still in memory, then starting over, must both give the same bytes.
    if (expected.Length > 0)
    {
        var again = new byte[Math.Min(expected.Length, 4096)];
        long from = Math.Max(stream.Oldest, expected.Length - (long)again.Length);
        int n = stream.Copy(from, again, 0, again.Length);
        stream.Rewind();
        if (stream.Position != 0 || !again.AsSpan(0, n).SequenceEqual(expected.AsSpan((int)from, n)) || !stream.Advance() && expected.Length > 0)
        {
            Console.WriteLine($"FAIL rewind: {name}");
            failures++;
            continue;
        }
    }
}

if (fuzz == 0 && bytes > 0)
    Console.WriteLine($"{files} files, {bytes / 1048576.0:0.0} MiB, whole frame decoding at {bytes / 1048576.0 / clock.Elapsed.TotalSeconds:0} MiB/s");
Console.WriteLine(failures == 0 ? "ZSTD: ALL OK" : $"ZSTD: {failures} FAILED");
return failures == 0 ? 0 : 1;

static int Fuzz(byte[] compressed, int expectedLength, int rounds, string name)
{
    var random = new Random(compressed.Length + 1);
    var capacity = new byte[Math.Max(expectedLength, 1) + 1024];
    var decoder = new ZstdDecoder();
    int refused = 0, survived = 0;
    for (int round = 0; round < rounds; round++)
    {
        var damaged = (byte[])compressed.Clone();
        int edits = random.Next(1, 8);
        for (int i = 0; i < edits && damaged.Length > 0; i++)
        {
            int at = random.Next(damaged.Length);
            switch (random.Next(3))
            {
                case 0:
                    damaged[at] ^= (byte)(1 << random.Next(8));
                    break;
                case 1:
                    damaged[at] = (byte)random.Next(256);
                    break;
                default:
                    damaged[at] = (byte)(random.Next(2) * 255);
                    break;
            }
        }
        int length = random.Next(4) == 0 ? random.Next(damaged.Length + 1) : damaged.Length;

        try
        {
            decoder.Decompress(damaged, 0, length, capacity, 0, capacity.Length);
            survived++;
        }
        catch (ToolException)
        {
            refused++;
        }

        try
        {
            var stream = new ZstdStream(new BytesSource(damaged), 0, length);
            for (int blocks = 0; blocks < 4096 && stream.Advance(); blocks++)
            {
            }
        }
        catch (ToolException)
        {
        }
    }
    Console.WriteLine($"fuzz {name}: {rounds} rounds, {refused} refused, {survived} decoded to something");
    return 0;
}

static long FirstDifference(byte[] a, byte[] b, int made)
{
    int length = Math.Min(made, b.Length);
    for (int i = 0; i < length; i++)
    {
        if (a[i] != b[i])
            return i;
    }
    return length;
}
