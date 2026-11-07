using System.Buffers.Binary;
using System.Net;
using System.Net.NetworkInformation;
using System.Net.Sockets;
using System.Text;

namespace HacBrewPackLz;

/// <summary>Shares game files with hacBrewPackLZ on a Switch, which installs them from there. Over the network it says it's there every second, so the
/// console finds it by itself, and with a USB cable, on Windows, it waits for the console to turn up. The console asks for parts of the files as it needs
/// them, and nothing on the PC is ever written to.</summary>
internal static class Sender
{
    public const int TcpPort = 25870;
    public const int BeaconPort = 25871;
    private const string BeaconMagic = "HBPZ1";
    private const int RequestHeader = 4 + 4 + 8 + 8 + 4;
    private const int LargestRead = 16 << 20;
    /// <summary>A path, or for Estimate, a game's settings and all its paths.</summary>
    private const int LargestPath = 64 << 10;

    private enum Command : uint { Hello = 1, List = 2, Stat = 3, Read = 4, Describe = 5, Estimate = 6 }
    /// <summary>Working: ask again in a moment.</summary>
    private enum Status : uint { Done = 0, NotThere = 1, Failed = 2, Working = 3 }

    /// <param name="watch">Where to keep what's going on, for a window to show. The command line only has what's said.</param>
    /// <param name="keys">The keys, for working out what's in the files for the Switch. Only asked for when it's needed.</param>
    public static void Run(IReadOnlyList<string> inputs, CancellationToken token, SendWatch? watch = null, Func<Keyset>? keys = null)
    {
        var roots = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        foreach (var input in inputs.Where(i => i.Trim().Length > 0))
        {
            string full = Path.GetFullPath(input);
            bool isFile = File.Exists(full);
            // It takes no options, and what follows one, like the keys after -k, is no game to share.
            if (input.StartsWith('-') && !isFile && !Directory.Exists(full))
                throw new ToolException($"'{input}' 不是有效参数：send 只接受要发送到 Switch 的游戏文件或文件夹。");
            if (!isFile && !Directory.Exists(full))
            {
                Ui.Warn($"未找到 '{input}'");
                continue;
            }
            // Folders are shared whole, as an extracted game's files can be anything. A file on its own has to be a game.
            if (isFile && !GameArchive.IsGameFile(full))
            {
                Ui.Warn($"'{input}' 不是 NSP、NSZ、XCI 或 XCZ，已跳过不共享");
                continue;
            }
            string name = Path.GetFileName(full.TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar));
            if (name.Length == 0)
                name = full.Replace(":", "").Trim(Path.DirectorySeparatorChar);
            string unique = name;
            for (int i = 2; roots.ContainsKey(unique); i++)
                unique = $"{name} ({i})";
            roots[unique] = full;
        }
        if (roots.Count == 0)
            throw new ToolException("没有可发送的内容，请给出游戏文件或文件夹。");

        TcpListener? listener = null;
        int port = TcpPort;
        for (; port < TcpPort + 10; port++)
        {
            try
            {
                listener = new TcpListener(IPAddress.Any, port);
                listener.Start();
                break;
            }
            catch (SocketException)
            {
                listener = null;
            }
        }
        if (listener is null)
            throw new ToolException("无法启动网络共享：所需的端口已被占用。");

        Ui.Info($"正以 {Environment.MachineName} 的身份向 Switch 共享 {roots.Count} 个项目：");
        foreach (var root in roots)
            Ui.Info($"  {root.Value}");
        Ui.Info("");
        Ui.Info("在 Switch 上打开 hacBrewPackLZ，这些游戏会出现在「游戏文件」中。请保持此窗口运行直到完成。");
        Ui.Info($"通过网络共享时，PC 与 Switch 必须处于同一局域网；Windows 可能提示是否允许通过防火墙。端口 {port}。");
        if (OperatingSystem.IsWindows())
            Ui.Info("通过 USB 数据线连接时，Windows 需要为 Switch 安装 WinUSB 或 libusbK 驱动（可用 Zadig 安装，与 DBI 使用的是同一个驱动）。");

        var sent = watch ?? new SendWatch();
        sent.Started(Environment.MachineName, port);
        var answers = new SendAnswers(inside => Resolve(roots, inside), keys ?? (() => Keyset.Load(Keyset.Find(null))));
        // The tests connect straight to the port, and leave the cable and the Switch on the network to whatever else is sending.
        bool quiet = Environment.GetEnvironmentVariable("HBP_SEND_UNANNOUNCED") == "1";
        var threads = new List<Thread> { new(() => Accept(listener, roots, answers, sent, token)) { IsBackground = true, Name = "网络" } };
        if (!quiet)
            threads.Add(new Thread(() => Announce(port, token)) { IsBackground = true, Name = "广播宣告" });
        if (OperatingSystem.IsWindows() && !quiet)
            threads.Add(new Thread(() => UsbLoop(roots, answers, sent, token)) { IsBackground = true, Name = "USB" });
        foreach (var thread in threads)
            thread.Start();

        token.WaitHandle.WaitOne();
        listener.Stop();
        throw new OperationCanceledException(token);
    }

    /// <summary>Says it's there, every second, to every network it's on.</summary>
    private static void Announce(int port, CancellationToken token)
    {
        byte[] message = Encoding.UTF8.GetBytes($"{BeaconMagic} {port} {Environment.MachineName}");
        using var udp = new UdpClient { EnableBroadcast = true };
        while (!token.IsCancellationRequested)
        {
            foreach (var target in BroadcastAddresses())
            {
                try
                {
                    udp.Send(message, message.Length, new IPEndPoint(target, BeaconPort));
                }
                catch (SocketException)
                {
                }
            }
            token.WaitHandle.WaitOne(1000);
        }
    }

    private static IEnumerable<IPAddress> BroadcastAddresses()
    {
        var found = new HashSet<IPAddress> { IPAddress.Broadcast };
        try
        {
            foreach (var adapter in NetworkInterface.GetAllNetworkInterfaces())
            {
                if (adapter.OperationalStatus != OperationalStatus.Up || adapter.NetworkInterfaceType == NetworkInterfaceType.Loopback)
                    continue;
                foreach (var address in adapter.GetIPProperties().UnicastAddresses)
                {
                    if (address.Address.AddressFamily != AddressFamily.InterNetwork || address.IPv4Mask is null)
                        continue;
                    byte[] ip = address.Address.GetAddressBytes(), mask = address.IPv4Mask.GetAddressBytes();
                    for (int i = 0; i < 4; i++)
                        ip[i] |= (byte)~mask[i];
                    found.Add(new IPAddress(ip));
                }
            }
        }
        catch (NetworkInformationException)
        {
        }
        return found;
    }

    private static void Accept(TcpListener listener, Dictionary<string, string> roots, SendAnswers answers, SendWatch sent, CancellationToken token)
    {
        while (!token.IsCancellationRequested)
        {
            TcpClient client;
            try
            {
                client = listener.AcceptTcpClient();
            }
            catch (Exception e) when (e is SocketException or ObjectDisposedException or InvalidOperationException)
            {
                return;
            }
            client.NoDelay = true;
            new Thread(() =>
            {
                string who = (client.Client.RemoteEndPoint as IPEndPoint)?.Address.ToString() ?? "网络";
                Ui.Info($"Switch 已从 {who} 连接。");
                string how = $"通过网络（{who}）";
                sent.Connected(how);
                using (client)
                    Serve(client.GetStream(), roots, answers, sent, token);
                sent.Disconnected(how);
                Ui.Info($"{who} 的 Switch 已断开连接。");
            }) { IsBackground = true, Name = "正在提供服务" }.Start();
        }
    }

    /// <summary>Answers whatever the console asks, until it goes away.</summary>
    private static void Serve(Stream stream, Dictionary<string, string> roots, SendAnswers answers, SendWatch sent, CancellationToken token)
    {
        FileStream? open = null;
        string? openPath = null;
        try
        {
            var header = new byte[RequestHeader];
            while (!token.IsCancellationRequested)
            {
                if (!ReadAll(stream, header))
                    return;
                if (header[0] != 'H' || header[1] != 'B' || header[2] != 'P' || header[3] != 'Q')
                    return;
                var command = (Command)BinaryPrimitives.ReadUInt32LittleEndian(header.AsSpan(4));
                long a = BinaryPrimitives.ReadInt64LittleEndian(header.AsSpan(8));
                long b = BinaryPrimitives.ReadInt64LittleEndian(header.AsSpan(16));
                int pathLength = BinaryPrimitives.ReadInt32LittleEndian(header.AsSpan(24));
                if (pathLength < 0 || pathLength > LargestPath)
                    return;
                var pathBytes = new byte[pathLength];
                if (pathLength > 0 && !ReadAll(stream, pathBytes))
                    return;
                string inside = Encoding.UTF8.GetString(pathBytes);

                Status status = Status.Done;
                byte[] answer = [];
                try
                {
                    switch (command)
                    {
                        case Command.Hello:
                            answer = Encoding.UTF8.GetBytes($"{AppInfo.Name} {AppInfo.Version}\n{Environment.MachineName}");
                            break;
                        case Command.List:
                            (status, answer) = List(roots, inside);
                            break;
                        case Command.Stat:
                            (status, answer) = Stat(roots, inside);
                            break;
                        case Command.Read:
                            string? path = Resolve(roots, inside);
                            if (path is null || !File.Exists(path) || a < 0 || b < 0 || b > LargestRead)
                            {
                                status = Status.NotThere;
                                break;
                            }
                            if (openPath != path)
                            {
                                open?.Dispose();
                                open = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete, 1 << 16, FileOptions.SequentialScan);
                                openPath = path;
                            }
                            answer = new byte[b];
                            open!.Seek(a, SeekOrigin.Begin);
                            int got = 0;
                            while (got < answer.Length)
                            {
                                int n = open.Read(answer, got, answer.Length - got);
                                if (n <= 0)
                                    break;
                                got += n;
                            }
                            if (got != answer.Length)
                                status = Status.Failed;
                            else
                                sent.Read(path, b, open.Length);
                            break;
                        case Command.Describe or Command.Estimate:
                            var known = command == Command.Describe ? answers.Describe(inside) : answers.Estimate(inside);
                            status = !known.Ready ? Status.Working : known.Done ? Status.Done : Status.Failed;
                            answer = known.Data;
                            break;
                        default:
                            status = Status.Failed;
                            break;
                    }
                }
                catch (Exception e) when (e is IOException or UnauthorizedAccessException)
                {
                    status = Status.Failed;
                    answer = [];
                }

                var reply = new byte[16];
                "HBPR"u8.CopyTo(reply);
                BinaryPrimitives.WriteUInt32LittleEndian(reply.AsSpan(4), (uint)status);
                BinaryPrimitives.WriteInt64LittleEndian(reply.AsSpan(8), status == Status.Done ? answer.Length : 0);
                stream.Write(reply);
                if (status == Status.Done && answer.Length > 0)
                    stream.Write(answer);
                stream.Flush();
            }
        }
        catch (Exception e) when (e is IOException or ObjectDisposedException or SocketException)
        {
        }
        finally
        {
            open?.Dispose();
        }
    }

    private static bool ReadAll(Stream stream, byte[] buffer)
    {
        int got = 0;
        while (got < buffer.Length)
        {
            int n = stream.Read(buffer, got, buffer.Length - got);
            if (n <= 0)
                return false;
            got += n;
        }
        return true;
    }

    /// <summary>What a path from the console is on the PC, which has to be inside something that's been shared. Null if it isn't.</summary>
    private static string? Resolve(Dictionary<string, string> roots, string inside)
    {
        // No file has a NUL in its name, and the path functions throw on one.
        if (inside.Contains('\0'))
            return null;
        var parts = inside.Split('/', StringSplitOptions.RemoveEmptyEntries);
        if (parts.Length == 0 || !roots.TryGetValue(parts[0], out var root))
            return null;
        if (parts.Length == 1)
            return root;
        if (!Directory.Exists(root) || parts.Skip(1).Any(p => p is "." or ".." || p.Contains('\\') || p.Contains(':')))
            return null;
        string full = Path.GetFullPath(Path.Combine([root, .. parts.Skip(1)]));
        string within = root.EndsWith(Path.DirectorySeparatorChar) ? root : root + Path.DirectorySeparatorChar;
        return full.StartsWith(within, StringComparison.OrdinalIgnoreCase) ? full : null;
    }

    private static (Status, byte[]) List(Dictionary<string, string> roots, string inside)
    {
        var text = new StringBuilder();
        if (inside.Trim('/').Length == 0)
        {
            foreach (var root in roots)
            {
                if (Directory.Exists(root.Value))
                    text.Append($"D\t{root.Key}\n");
                else
                    text.Append($"F\t{new FileInfo(root.Value).Length}\t{root.Key}\n");
            }
            return (Status.Done, Encoding.UTF8.GetBytes(text.ToString()));
        }
        string? path = Resolve(roots, inside);
        if (path is null || !Directory.Exists(path))
            return (Status.NotThere, []);
        var folder = new DirectoryInfo(path);
        foreach (var child in folder.EnumerateDirectories())
        {
            if (!child.Attributes.HasFlag(FileAttributes.Hidden) && !child.Attributes.HasFlag(FileAttributes.System))
                text.Append($"D\t{child.Name}\n");
        }
        foreach (var file in folder.EnumerateFiles())
        {
            if (!file.Attributes.HasFlag(FileAttributes.Hidden) && !file.Attributes.HasFlag(FileAttributes.System))
                text.Append($"F\t{file.Length}\t{file.Name}\n");
        }
        return (Status.Done, Encoding.UTF8.GetBytes(text.ToString()));
    }

    private static (Status, byte[]) Stat(Dictionary<string, string> roots, string inside)
    {
        if (inside.Trim('/').Length == 0)
            return (Status.Done, "D"u8.ToArray());
        string? path = Resolve(roots, inside);
        if (path is not null && Directory.Exists(path))
            return (Status.Done, "D"u8.ToArray());
        if (path is not null && File.Exists(path))
            return (Status.Done, Encoding.UTF8.GetBytes($"F\t{new FileInfo(path).Length}"));
        return (Status.NotThere, []);
    }

    /// <summary>The console can only be reached over USB once Windows has a driver for it that lets programs talk to it, as with DBI.</summary>
    private static void UsbLoop(Dictionary<string, string> roots, SendAnswers answers, SendWatch sent, CancellationToken token)
    {
        bool saidWaiting = false;
        while (!token.IsCancellationRequested)
        {
            using var device = SwitchUsb.Open(out string? problem);
            if (device is null)
            {
                if (problem is not null && !saidWaiting)
                {
                    Ui.Warn(problem);
                    saidWaiting = true;
                }
                sent.UsbProblem(problem);
                token.WaitHandle.WaitOne(2000);
                continue;
            }
            saidWaiting = false;
            sent.UsbProblem(null);
            Ui.Info("Switch 已通过 USB 数据线连接。");
            sent.Connected("通过 USB 数据线");
            using (var stream = new UsbStream(device))
            {
                using var stopping = token.Register(() => device.Dispose());
                Serve(stream, roots, answers, sent, token);
            }
            sent.Disconnected("通过 USB 数据线");
            Ui.Info("Switch 已断开 USB 数据线连接。");
            token.WaitHandle.WaitOne(1000);
        }
    }
}

/// <summary>What sending to a Switch is doing: who's connected, and how much of each file the Switch has read. Which file it's on, and how far along,
/// is also said every few seconds, for the command line.</summary>
internal sealed class SendWatch
{
    private const double SpeedSeconds = 3;

    private readonly object _gate = new();
    private readonly List<string> _connected = [];
    private readonly Dictionary<string, (long read, long length)> _files = new(StringComparer.OrdinalIgnoreCase);
    private readonly Queue<(DateTime at, long bytes)> _recent = new();
    private string _machine = "";
    private int _port;
    private string? _usbProblem;
    private string? _file;
    private DateTime _lastRead = DateTime.MinValue;
    private DateTime _lastSaid = DateTime.MinValue;

    /// <summary>Raised on whichever thread it happened on.</summary>
    public event Action? Changed;

    public void Started(string machine, int port)
    {
        lock (_gate)
        {
            _machine = machine;
            _port = port;
        }
        Changed?.Invoke();
    }

    public void Connected(string how)
    {
        lock (_gate)
            _connected.Add(how);
        Changed?.Invoke();
    }

    public void Disconnected(string how)
    {
        lock (_gate)
            _connected.Remove(how);
        Changed?.Invoke();
    }

    public void UsbProblem(string? problem)
    {
        lock (_gate)
        {
            if (_usbProblem == problem)
                return;
            _usbProblem = problem;
        }
        Changed?.Invoke();
    }

    public void Read(string path, long count, long length)
    {
        lock (_gate)
        {
            var now = DateTime.UtcNow;
            var (read, _) = _files.GetValueOrDefault(path);
            // What's read more than once still only counts the once, as far as how much is left goes.
            _files[path] = (Math.Min(read + count, length), length);
            _recent.Enqueue((now, count));
            while (_recent.Count > 0 && (now - _recent.Peek().at).TotalSeconds > SpeedSeconds)
                _recent.Dequeue();
            _lastRead = now;

            bool newFile = path != _file;
            _file = path;
            if (newFile || (now - _lastSaid).TotalSeconds >= 5)
            {
                _lastSaid = now;
                if (newFile)
                    Ui.Info($"正在发送 {Path.GetFileName(path)}");
                else
                    Ui.Info($"  已发送 {ProgressReporter.Format(_files[path].read)} / 共 {ProgressReporter.Format(length)}");
            }
        }
        Changed?.Invoke();
    }

    public SendSnapshot Snapshot()
    {
        lock (_gate)
        {
            var now = DateTime.UtcNow;
            bool active = (now - _lastRead).TotalSeconds < SpeedSeconds;
            long speed = active ? (long)(_recent.Where(r => (now - r.at).TotalSeconds <= SpeedSeconds).Sum(r => r.bytes) / SpeedSeconds) : 0;
            return new SendSnapshot(_machine, _port, _connected.ToArray(), _usbProblem, active ? _file : null, speed,
                new Dictionary<string, (long, long)>(_files, StringComparer.OrdinalIgnoreCase));
        }
    }
}

/// <param name="Reading">The file the Switch is reading now, if it's reading anything.</param>
internal sealed record SendSnapshot(string Machine, int Port, string[] Connected, string? UsbProblem, string? Reading, long BytesPerSecond,
    IReadOnlyDictionary<string, (long read, long length)> Files)
{
    /// <summary>How much of these files, or of the files in these folders, the Switch has read.</summary>
    public (long read, long length) Of(IEnumerable<string> paths)
    {
        long read = 0, length = 0;
        foreach (string path in paths)
        {
            string full = Path.GetFullPath(path);
            string inside = full.TrimEnd(Path.DirectorySeparatorChar) + Path.DirectorySeparatorChar;
            foreach (var file in Files)
            {
                if (string.Equals(file.Key, full, StringComparison.OrdinalIgnoreCase) || file.Key.StartsWith(inside, StringComparison.OrdinalIgnoreCase))
                {
                    read += file.Value.read;
                    length += file.Value.length;
                }
            }
        }
        return (read, length);
    }
}
