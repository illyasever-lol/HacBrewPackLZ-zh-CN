using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;

namespace HacBrewPackLz;

internal static class Program
{
    private static int Main(string[] args)
    {
        AppDomain.CurrentDomain.UnhandledException += (_, e) =>
        {
            if (e.ExceptionObject is Exception crash)
                ProblemReports.Crashed(crash);
        };
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
            // Run by itself, as a double click in a Mac's Finder does, it opens the home screen, unless it's in a folder
            // of things to build, which hacBrewPack builds with nothing more said.
            if (args is ["menu"] || (args.Length == 0 && !new[] { "exefs", "romfs", "control" }.Any(Directory.Exists)))
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
            Ui.Error($"Error: {Help.WithNextStep(e.Message)}");
        }

        if (ownsWindow && canAsk)
        {
            Ui.Info("");
            Console.Write("按 Enter 关闭此窗口");
            Console.ReadLine();
        }
        return result;
    }

    /// <summary>Sends a report, as Report a problem does on the screens: what's set and the system it's on, with what the person says, and never
    /// any keys. When it can't be sent, it's saved as a file.</summary>
    private static int SendReport(string said)
    {
        var report = new StringBuilder();
        report.AppendLine($"{AppInfo.Name} {AppInfo.Version}, sent from the command line");
        report.AppendLine($"{System.Runtime.InteropServices.RuntimeInformation.OSDescription}, {System.Runtime.InteropServices.RuntimeInformation.OSArchitecture}");
        report.AppendLine();
        report.AppendLine("== Settings");
        foreach (string line in Settings.Load().Lines())
            report.AppendLine(line);
        var outcome = ReportSender.Send("manual", "", said.Length > 0 ? said[..Math.Min(said.Length, 80)] : "A report from the command line", said, Help.Clean(report.ToString(), null));
        if (outcome.Sent || outcome is { SavedTo: not null, Problem: null })
            Ui.Success(outcome.Said);
        else
            Ui.Warn(outcome.Said);
        return outcome.Sent || outcome.SavedTo is not null ? 0 : 1;
    }

    /// <summary>What goes with a problem from the command line: the version, the system and the options, without the files given.</summary>
    private static string CommandLineReport(string[] args)
    {
        var report = new StringBuilder();
        report.AppendLine($"{AppInfo.Name} {AppInfo.Version}, from the command line");
        report.AppendLine($"{RuntimeInformation.OSDescription}, {RuntimeInformation.OSArchitecture}");
        report.AppendLine();
        report.AppendLine("== Options");
        foreach (string arg in args.Where(a => a.StartsWith('-')))
        {
            string name = arg.Split('=')[0];
            report.AppendLine(ValuesKept.Contains(name) ? arg : name);
        }
        return report.ToString();
    }

    /// <summary>Options whose values only say how it was set up, as numbers. Every other one goes in a report by its name alone, however it's
    /// written, as its value can be a path, a name or a key.</summary>
    private static readonly HashSet<string> ValuesKept =
        ["--level", "--blocksize", "--threads", "--minsaving", "--keygeneration", "--sdkversion", "--titleversion", "--requiredsystemversion"];

    private static int Run(string[] args, bool ownsWindow)
    {
        if (args.Length > 0 && args[0] == "verify")
            return Verifier.Run(args[1..]);
        if (args.Length > 0 && args[0] is "details" or "look")
            return LookCommand.Run(args[0], args[1..]);
        if (args.Length > 0 && args[0] == "report")
            return SendReport(string.Join(" ", args[1..]));
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

        // The verbs the screens use, compress and check, and the name the command line has always had, convert.
        var mode = BatchMode.Ask;
        if (args.Length > 0 && args[0] is "convert" or "compress" or "extract" or "analyze" or "check")
        {
            mode = args[0] switch
            {
                "convert" or "compress" => BatchMode.Convert,
                "extract" => BatchMode.Extract,
                "check" => BatchMode.Check,
                _ => BatchMode.Analyze,
            };
            args = args[1..];
        }

        var options = Options.Parse(args);
        ProblemReports.AgreedOnCommandLine = options.SendProblems;
        ProblemReports.ReportText = () => CommandLineReport(args);
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
