using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;

namespace HacBrewPackLz;

internal static class Program
{
    private static int Main(string[] args)
    {
        Updater.Tidy();
        bool isConsole = !Console.IsOutputRedirected;
        bool canAsk = isConsole && !Console.IsInputRedirected;
        bool colors = isConsole && Terminal.EnableVirtualTerminal();
        if (isConsole)
            Console.OutputEncoding = Encoding.UTF8;

        // Started by double clicking or by dropping files on it, the window is ours and would vanish when the program ends.
        bool ownsWindow = OwnsConsoleWindow();
        if (canAsk && colors)
        {
            if (args is ["menu"])
                return Tui.Run([]);
            if (ownsWindow && args.All(a => !a.StartsWith('-') && (File.Exists(a) || Directory.Exists(a))))
                return Tui.Run(args);
        }

        Ui.Sink = colors ? new ConsoleSink() : new PlainSink();
        Ui.Heading($"{AppInfo.Name} v{AppInfo.Version}");

        int result = 1;
        try
        {
            result = Run(args, ownsWindow);
        }
        catch (Exception e) when (e is ToolException or IOException or UnauthorizedAccessException or CryptographicException)
        {
            Ui.Error($"Error: {e.Message}");
        }

        if (ownsWindow && canAsk)
        {
            Ui.Info("");
            Console.Write("按 Enter 关闭此窗口");
            Console.ReadLine();
        }
        return result;
    }

    private static int Run(string[] args, bool ownsWindow)
    {
        if (args.Length > 0 && args[0] == "verify")
            return Verifier.Run(args[1..]);
        if (args.Length > 0 && args[0] == "send")
        {
            if (args.Length < 2)
                throw new ToolException("请给出要发送到 Switch 的游戏文件或文件夹。");
            using var stop = new CancellationTokenSource();
            Console.CancelKeyPress += (_, e) =>
            {
                e.Cancel = true;
                stop.Cancel();
            };
            Ui.Info("按 Ctrl+C 停止。");
            try
            {
                Sender.Run(args[1..], stop.Token);
            }
            catch (OperationCanceledException)
            {
                Ui.Info("已停止向 Switch 发送。");
            }
            return 0;
        }
        if (args is ["update"])
        {
            var (updated, message) = Updater.Run();
            if (updated)
                Ui.Success(message);
            else
                Ui.Info(message);
            return message.StartsWith("Couldn't", StringComparison.Ordinal) ? 1 : 0;
        }
        if (args.Contains("-h") || args.Contains("--help") || (args.Length == 0 && ownsWindow))
        {
            Console.Write(HelpText.Plain(Console.IsOutputRedirected ? 100 : Terminal.Width - 1));
            return 0;
        }

        var mode = BatchMode.Ask;
        if (args.Length > 0 && args[0] is "convert" or "extract" or "analyze")
        {
            mode = args[0] switch
            {
                "convert" => BatchMode.Convert,
                "extract" => BatchMode.Extract,
                _ => BatchMode.Analyze,
            };
            args = args[1..];
        }

        var options = Options.Parse(args);
        if (mode == BatchMode.Analyze && options.Inputs.Count == 0)
            return Analyzer.ReportBuild(options);

        var keyset = Keyset.Load(Keyset.Find(options.KeysetPath));
        if (mode != BatchMode.Ask || options.Inputs.Count > 0)
            return Batch.Run(options, keyset, mode);

        int result = new Builder(options, keyset).Run();
        if (result == 0 && options.VerifyAfterBuild)
            result = Verifier.RunAfterBuild(options, keyset);
        return result;
    }

    private static bool OwnsConsoleWindow()
    {
        if (!OperatingSystem.IsWindows())
            return false;
        var processes = new uint[2];
        return GetConsoleProcessList(processes, (uint)processes.Length) == 1;
    }

    [DllImport("kernel32.dll")]
    private static extern uint GetConsoleProcessList([Out] uint[] processList, uint processCount);
}
