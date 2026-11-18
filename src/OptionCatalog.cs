using System.Text;

namespace HacBrewPackLz;

/// <summary>Which jobs read an option.</summary>
[Flags]
internal enum OptionUse
{
    Build = 1,
    Convert = 2,
    Extract = 4,
}

internal enum OptionKind
{
    Toggle,
    Slider,
    Choice,
    Number,
    Text,
    Hex,
    File,
    Folder,

    /// <summary>Not a setting but something to do, with a button, among the options of the home screen and the GUI.</summary>
    Action,
}

internal sealed record OptionSpec(string Flag, string Label, OptionKind Kind, OptionUse Use, string Help)
{
    /// <summary>Whether a toggle's flag is on, with nothing stored meaning its default.</summary>
    public bool ToggleOn(string stored) => (stored.Length > 0 ? stored : DefaultApplies ? Default : "") == "1";

    /// <summary>What to store for a toggle's flag: nothing for its default, so a toggle that's on to begin with is kept off with a 0.</summary>
    public string ToggleValue(bool on) => on == ToggleOn("") ? "" : on ? "1" : "0";

    /// <summary>Also accepted on the command line.</summary>
    public string? Short { get; init; }

    /// <summary>What leaving it empty means, for people reading the console screen or the GUI.</summary>
    public string Placeholder { get; init; } = "";

    public string[] Choices { get; init; } = [];
    public int Min { get; init; }
    public int Max { get; init; }

    /// <summary>Starting value for a slider or a choice.</summary>
    public string Default { get; init; } = "";

    /// <summary>The home screen and the GUI go by the default as if it had been chosen, even though the command line doesn't.</summary>
    public bool DefaultApplies { get; init; }

    /// <summary>The toggle reads "on" when the flag is absent.</summary>
    public bool Inverted { get; init; }

    /// <summary>v18：下拉框旁允许自定义填写（SDK / 所需系统版本）。</summary>
    public bool Editable { get; init; }

    /// <summary>Help for the command line when it has to say more than the GUI does.</summary>
    public string? CommandHelp { get; init; }

    /// <summary>What an action's button says.</summary>
    public string Verb { get; init; } = "";

    /// <summary>What it does in a few words, on one line, where each option has to take the same room, as on the welcome.</summary>
    public string? Brief { get; init; }
}

internal sealed record OptionGroup(string Title, OptionSpec[] Options);

/// <summary>Every option, in one place: the command line help, the console screen and the GUI are all made from it.</summary>
internal static class OptionCatalog
{
    private const OptionUse Rebuilds = OptionUse.Build | OptionUse.Convert;
    private const OptionUse All = OptionUse.Build | OptionUse.Convert | OptionUse.Extract;

    public static readonly OptionGroup Compression = new("压缩",
    [
        new("--nocompress", "压缩游戏数据", OptionKind.Toggle, Rebuilds,
            "关闭时，游戏按原样重打包，列表中的更新补丁会并入本体。这样更快，只有带更新补丁的游戏体积才会变小。")
        {
            Inverted = true, CommandHelp = "像 hacBrewPack 一样不压缩存储 RomFS",
        },
        new("--level", "压缩级别", OptionKind.Slider, Rebuilds,
            "1 和 2 为 LZ4，速度最快；3 到 12 为 LZ4 HC，体积更小但打包更慢。9 是较均衡的选择。")
        {
            Min = Lz4Encoder.MinLevel, Max = Lz4Encoder.MaxLevel, Default = "9",
            CommandHelp = "1-2 为 LZ4（最快），3-12 为 LZ4 HC（更小、更慢），默认 9",
        },
        new("--blocksize", "块大小", OptionKind.Choice, Rebuilds,
            "块越大压缩率越好，零售游戏使用 64K。")
        {
            Choices = ["4K", "8K", "16K", "32K", "64K"], Default = "64K",
            CommandHelp = "压缩块大小，2 的幂，从 4K 到 64K，默认 64K",
        },
        new("--threads", "线程数", OptionKind.Number, Rebuilds,
            "用于压缩的处理器线程数。")
        {
            Min = 1, Max = 1024, Placeholder = "全部线程",
            CommandHelp = "压缩线程数，默认使用全部逻辑处理器",
        },
    ]);

    public static readonly OptionGroup Worthwhile = new("值得处理的游戏",
    [
        new("--minsaving", "最小节省比例", OptionKind.Choice, OptionUse.Convert,
            "实测节省比例低于此值的游戏会归入「已完成」，把时间用在收益最大的游戏上。仍可手动勾选它们。")
        {
            Brief = "跳过节省空间较少的游戏",
            Choices = ["关", "5%", "10%", "15%", "20%", "25%", "30%", "40%", "50%"], Default = "5%", DefaultApplies = true,
            CommandHelp = "跳过节省比例低于该百分比的游戏，每个游戏都会先分析。默认关闭",
        },
        new("--measure", "完整分析", OptionKind.Toggle, OptionUse.Convert,
            "开启后，在快速预览每个游戏之外还会完整读取全部数据（不写入任何文件），优先处理可能改变结论的游戏，使每项数值精确并自动勾选值得处理的游戏。期间列表仍可使用，压缩或其他操作会中止分析。关闭时，快速预览无法确认的游戏保持未勾选。")
        {
            Default = "1", DefaultApplies = true,
            Brief = "完整读取每个游戏，得出精确数值",
            CommandHelp = "完整读取快速预览无法确认的标题（不写入任何文件），使分析能给出节省量，转换时压缩其中值得处理的游戏；否则转换会跳过它们",
        },
        new("--hidesmall", "隐藏 1 MB 以下的游戏", OptionKind.Toggle, OptionUse.Convert | OptionUse.Extract,
            "跳过没有更新补丁或 DLC 的小体积游戏，例如 HOME 菜单里自制程序的快捷方式，这些不值得列入。")
        {
            Default = "1", DefaultApplies = true,
            CommandHelp = "跳过小于 1 MB 且无更新补丁或 DLC 的游戏，例如自制程序快捷方式（转发器）。主界面与 GUI 默认开启",
        },
        new("--showunusable", "隐藏无法使用的文件", OptionKind.Toggle, 0,
            "在文件夹里找到的、不是游戏或无法读取的文件不会进入「待处理」，但仍列在「全部」顶部。单独添加的文件总会说明问题所在。")
        {
            Inverted = true, CommandHelp = "在主界面与 GUI 中列出文件夹里无法使用的文件",
        },
    ]);

    public static readonly OptionGroup Output = new("输出",
    [
        new("--nspdir", "NSP 输出文件夹", OptionKind.Folder, Rebuilds,
            "生成的 NSP 存放位置。")
        {
            Brief = "压缩后游戏的保存位置",
            Placeholder = "与源文件同目录",
            CommandHelp = "Set output nsp directory. Default when converting is beside the input, and when building .\\hacbrewpack_nsp\\",
        },
        new("--outdir", "解包文件夹", OptionKind.Folder, OptionUse.Extract,
            "解包文件的存放位置，每个游戏一个文件夹。")
        {
            Placeholder = "与源文件同目录",
            CommandHelp = "设置解包输出目录，默认与输入文件同目录",
        },
        new("--tempdir", "临时文件夹", OptionKind.Folder, Rebuilds,
            "仅用于需要回溯的固态 NSZ，其他情况用不到。")
        {
            Placeholder = "NSP 输出文件夹",
            CommandHelp = "设置固态 NSZ 回溯读取内容的目录，默认为输出 NSP 的目录",
        },
        new("--mergedlc", "整合打包（本体+更新+DLC 合并为单一 NSP）", OptionKind.Toggle, Rebuilds,
            "开启后，同一游戏的本体、更新补丁与全部 DLC 合并为一个 NSP（内含各自的内容元数据）；关闭时 DLC 仍单独输出。")
        {
            CommandHelp = "把同一 Title ID 下的本体、更新补丁和 DLC 打包进一个 NSP，每个标题保留自己的 CNMT",
        },
        new("--verify", "校验每个生成的 NSP", OptionKind.Toggle, Rebuilds,
            "回读新生成的文件并完整校验，耗时会更长。")
        {
            Brief = "回读每个生成的文件进行校验",
        },
        new("--keepncadir", "同时输出 NCA 文件", OptionKind.Toggle, Rebuilds,
            "同时把生成的 NCA 复制到 NCA 文件夹。")
        {
            CommandHelp = "同时把 NCA 写入 nca 目录",
        },
        new("--ncadir", "NCA 文件夹", OptionKind.Folder, Rebuilds,
            "这些 NCA 的存放位置。")
        {
            Placeholder = "hacbrewpack_nca",
            CommandHelp = "Set output nca directory path used by --keepncadir, default path is .\\hacbrewpack_nca\\",
        },
    ]);

    public static readonly OptionGroup Keys = new("密钥与签名",
    [
        new("--keyset", "密钥文件", OptionKind.File, All,
            "主机密钥文件。程序会自动查找旁边的 prod.keys。")
        {
            Short = "-k", Placeholder = "程序目录下的 prod.keys",
            CommandHelp = "设置密钥文件路径，默认依次查找 keys.dat、keys.txt、keys.ini 和 prod.keys",
        },
        new("--keygeneration", "密钥世代", OptionKind.Number, Rebuilds,
            "用哪个密钥区域密钥加密新生成的 NCA。")
        {
            Min = 1, Max = 32, Placeholder = "1",
            CommandHelp = "设置加密密钥区域所用的密钥世代，默认密钥世代为 1",
        },
        new("--keyareakey", "密钥区密钥 2", OptionKind.Hex, Rebuilds,
            "32 位十六进制字符。")
        {
            Placeholder = "04040404040404040404040404040404",
            CommandHelp = "以十六进制设置密钥区域密钥 2，长度 16 字节",
        },
        new("--sdkversion", "SDK 版本", OptionKind.Choice, Rebuilds,
            "选择沿用源文件则不修改密钥版本，可在下拉框选择需要修改的SDK版本，例：11.0.0、0xA (9.1.0-12.0.3)或000C1100。")
        {
            Choices = FirmwareVersions.Choices,
            Default = "沿用源文件",
            Editable = true,
            Placeholder = "自定义填写，例如 11.0.0、0xA (9.1.0-12.0.3) 或 000C1100",
            CommandHelp = "选择固件范围同时设定 SDK 版本与密钥世代；也可自定义填写 11.0.0 等版本号或 000C1100 等十六进制",
        },
        new("--plaintext", "不加密分区", OptionKind.Toggle, Rebuilds,
            "以不加密方式写入区段，并标记为明文。")
        {
            CommandHelp = "跳过区段加密，并将区段头的加密类型设为明文",
        },
        new("--nosignncasig2", "不签名 NCA", OptionKind.Toggle, Rebuilds,
            "保留原始 ACID 公钥，跳过第二次 NCA 签名。")
        {
            CommandHelp = "跳过修补 npdm 中的 acid 公钥以及用 acid 公钥签名 nca 头",
        },
    ]);

    public static readonly OptionGroup Versions = new("版本",
    [
        new("--titleversion", "游戏版本", OptionKind.Hex, Rebuilds,
            "十六进制。默认 0，转换时沿用源文件的版本。")
        {
            Placeholder = "沿用源文件",
            CommandHelp = "以十六进制设置内容元数据中的游戏版本，默认 0，转换时沿用源文件的版本",
        },
        new("--requiredsystemversion", "所需系统版本", OptionKind.Choice, Rebuilds,
            "不选择则沿用源文件所需系统版本（压缩时最低 12.0.0）；选择后强制写入内容元数据。也可在下拉旁的输入框自定义填写，例如 12.0.0、20.0.0 或 000C1100。")
        {
            Choices = ["自动计算", "11.0.0", "12.0.0", "13.0.0", "14.0.0", "15.0.0", "16.0.0", "17.0.0", "18.0.0", "19.0.0", "20.0.0"],
            Default = "自动计算",
            Editable = true,
            Placeholder = "自定义填写，例如 12.0.0、20.0.0 或 000C1100",
            CommandHelp = "设置内容元数据的所需系统版本，可填写 11.0.0、20.0.0 等版本号或 000C1100 等十六进制；不选择则读取源文件",
        },
    ]);

    public static readonly OptionGroup BuildFolders = new("文件夹",
    [
        new("--exefsdir", "ExeFS 文件夹", OptionKind.Folder, OptionUse.Build, "必须包含 main.npdm。")
        {
            Placeholder = ".\\exefs",
            CommandHelp = "Set program exefs directory path, default path is .\\exefs\\",
        },
        new("--romfsdir", "RomFS 文件夹", OptionKind.Folder, OptionUse.Build, "游戏的数据。")
        {
            Placeholder = ".\\romfs",
            CommandHelp = "Set program romfs directory path, default path is .\\romfs\\",
        },
        new("--logodir", "Logo 文件夹", OptionKind.Folder, OptionUse.Build, "启动画面文件。")
        {
            Placeholder = ".\\logo",
            CommandHelp = "Set program logo directory path, default path is .\\logo\\",
        },
        new("--controldir", "Control 文件夹", OptionKind.Folder, OptionUse.Build, "必须包含 control.nacp。")
        {
            Placeholder = ".\\control",
            CommandHelp = "Set control romfs directory path, default path is .\\control\\",
        },
        new("--htmldocdir", "HtmlDocument 文件夹", OptionKind.Folder, OptionUse.Build, "可选的游戏内手册。")
        {
            Placeholder = "无",
            CommandHelp = "设置 HtmlDocument 的 romfs 目录路径",
        },
        new("--legalinfodir", "LegalInformation 文件夹", OptionKind.Folder, OptionUse.Build, "可选的法律声明。")
        {
            Placeholder = "无",
            CommandHelp = "设置 LegalInformation 的 romfs 目录路径",
        },
        new("--output", "输出文件", OptionKind.File, OptionUse.Build, "要写入的 NSP。默认以标题 ID 命名，放在 NSP 文件夹中。")
        {
            Short = "-o", Placeholder = "NSP 文件夹中的 <标题ID>.nsp",
            CommandHelp = "设置输出 NSP 文件路径（单个游戏），会覆盖 --nspdir",
        },
        new("--noromfs", "不含 RomFS", OptionKind.Toggle, OptionUse.Build, "构建不含 RomFS 区段的 Program NCA。")
        {
            CommandHelp = "跳过创建 program 的 romfs 区段",
        },
        new("--nologo", "不含 Logo", OptionKind.Toggle, OptionUse.Build, "构建不含 logo 区段的 Program NCA。")
        {
            CommandHelp = "跳过创建 program 的 logo 区段",
        },
    ]);

    public static readonly OptionGroup BuildTitle = new("名称",
    [
        new("--titleid", "标题 ID", OptionKind.Hex, OptionUse.Build, "16 位十六进制字符，替换 NCA、NPDM 和 NACP 中的 ID。")
        {
            Placeholder = "取自 main.npdm",
            CommandHelp = "使用指定的 titleid 创建 NCA，并修补 npdm 与 nacp 中的 titleid",
        },
        new("--titlename", "游戏名称", OptionKind.Text, OptionUse.Build, "替换 NACP 中所有语言的名称，最多 512 字节。")
        {
            Placeholder = "取自 control.nacp",
            CommandHelp = "修改 nacp 中所有语言的游戏名称，最大 512 字节",
        },
        new("--titlepublisher", "发行商", OptionKind.Text, OptionUse.Build, "替换 NACP 中所有语言的发行商，最多 256 字节。")
        {
            Placeholder = "取自 control.nacp",
            CommandHelp = "修改 nacp 中所有语言的发行商，最大 256 字节",
        },
        new("--nopatchnacplogo", "保留 NACP 的 Logo 设置", OptionKind.Toggle, OptionUse.Build, "通常会把 logo 处理方式改为自动。")
        {
            CommandHelp = "跳过将 NACP 中的 logo 处理方式改为 auto",
        },
    ]);

    public static readonly OptionGroup Updates = new("更新",
    [
        new("--noautoupdate", "保持程序为最新版本", OptionKind.Toggle, 0,
            "每次启动时检查并安装新版本，下载到的更新必须通过签名校验。")
        {
            Inverted = true, CommandHelp = "打开主界面和 GUI 时不检查新版本",
            Brief = "打开时自动更新程序",
        },
        new(ProblemReports.Flag, "发送问题报告", OptionKind.Toggle, 0,
            "出问题时自动向开发者发送报告以便修复，报告中绝不包含你的密钥。")
        {
            CommandHelp = "When a game can't be done, send a report of it to the developer, with no keys in it, once a version for each problem",
            Brief = "出问题时自动发送报告",
        },
    ]);

    public static readonly OptionGroup[] Groups = [Compression, Worthwhile, Output, Keys, Versions, BuildFolders, BuildTitle, Updates];

    /// <summary>Looks for a newer version there and then. The command line has the update command instead.</summary>
    public static readonly OptionSpec CheckForUpdates = new("check-for-updates", "检查新版本", OptionKind.Action, 0,
        "立即检查新版本，如有则安装。")
    {
        Verb = "立即检查",
    };

    /// <summary>What came up the first time it opened, again, as on the Switch.</summary>
    public static readonly OptionSpec ShowWelcome = new("show-welcome", "欢迎界面", OptionKind.Action, 0,
        "显示首次打开时的引导内容，把最重要的设置集中在一处。")
    {
        Verb = "显示",
    };

    /// <summary>
    /// 只在命令行里还有意义、GUI 不显示的项。v18 按用户要求恢复「系统版本 / 游戏版本 / 系统密钥」，
    /// 故把 --keygeneration / --keyareakey / --sdkversion / --plaintext / --nosignncasig2 /
    /// --titleversion / --requiredsystemversion 从隐藏集合移除；它们现在会出现在 GUI 选项里并参与设置保存。
    /// </summary>
    public static readonly HashSet<string> CommandLineOnly =
    [
        "--keepncadir", "--ncadir", "--blocksize", "--tempdir",
    ];

    /// <summary>What only means anything while compressing: with compression off nothing is compressed or measured, nor saves anything to go
    /// by. These go while it's off, on every version, as on the Switch.</summary>
    public static readonly HashSet<string> CompressionOnly = ["--level", "--minsaving", "--measure"];

    /// <summary>Whether an option is shown, by what's set: one that only means anything while compressing isn't while compression is off.</summary>
    public static bool Shown(OptionSpec option, Func<string, string> get) => !(CompressionOnly.Contains(option.Flag) && get("--nocompress") == "1");

    /// <summary>Whether changing this option can change which others are shown.</summary>
    public static bool DecidesShown(OptionSpec option) => option.Flag == "--nocompress";

    private static OptionGroup WithoutCommandLineOnly(OptionGroup group) =>
        group with { Options = group.Options.Where(o => !CommandLineOnly.Contains(o.Flag)).ToArray() };

    /// <summary>The groups a list of titles uses, with checking for updates and the welcome beside the setting that updates by itself.</summary>
    public static readonly OptionGroup[] GeneralGroups =
    [
        WithoutCommandLineOnly(Compression), Worthwhile, WithoutCommandLineOnly(Output),
        Keys, Versions,
        Updates with { Title = "本程序", Options = [.. Updates.Options, CheckForUpdates, ShowWelcome] },
    ];

    public static IEnumerable<OptionSpec> AllOptions => Groups.SelectMany(g => g.Options);

    private static OptionSpec Find(string flag) => AllOptions.First(o => o.Flag == flag);

    /// <summary>
    /// What most people will want a say in, all in one place, the first time it opens, as on the Switch. Everything else, and all
    /// of this again, is under Options.
    /// </summary>
    public static readonly OptionGroup Welcome = new("欢迎使用 hacBrewPackLZ",
        [Find("--nspdir"), Find("--verify"), Find("--minsaving"), Find("--noautoupdate"), Find(ProblemReports.Flag)]);

    /// <summary>What it is, in a sentence. The welcome says this, then what most people don't know, then what to watch out for.</summary>
    public const string WelcomeIntro =
        "以压缩形式保存游戏，Switch 本机可直接读取：占用空间更小，游玩体验不变。原始文件永远不会被修改。";

    /// <summary>What most people don't know, and the one thing the welcome makes stand out.</summary>
    public const string WelcomeTip = "要把游戏发送到 Switch？无需先压缩：Switch 端会在安装时自动完成。";

    public const string WelcomeWarning =
        "压缩游戏需要签名补丁（sigpatches）及 12.0.0 或更高系统版本。官方更新无法覆盖安装到这类游戏上。";

    public const string WelcomeLater = "以上设置以后都可以在「选项」中修改。";

    /// <summary>What About says it is, on every version: the GUI's and Android's About, and the home screen's Help and --help.</summary>
    public const string License = "基于 GNU 通用公共许可证第 2 版发布的自由软件。";

    /// <summary>Asked the once, of whoever had the welcome before sending problems was on it.</summary>
    public const string ProblemsQuestion = "是否发送问题报告？";

    public const string ProblemsExplained = "出问题时可自动向开发者发送报告以便修复，报告中绝不包含你的密钥。以后可在「选项」中修改。";

    /// <summary>What a report has in it, said where one is sent from.</summary>
    public const string ReportHolds = "程序版本、你的设置和列表中的游戏，不含任何密钥，全部列在下方";
}

/// <summary>A wrapped help text, laid out for whatever width the window has right now.</summary>
internal static class HelpText
{
    private const int FlagColumn = 26;

    public static List<(string text, UiLevel level)> Lines(int width)
    {
        var lines = new List<(string, UiLevel)>();
        void Heading(string text)
        {
            lines.Add(("", UiLevel.Info));
            lines.Add((text, UiLevel.Heading));
        }
        void Paragraph(string text)
        {
            foreach (string row in Terminal.Wrap(text, width))
                lines.Add((row, UiLevel.Info));
        }
        void Command(string command, string text)
        {
            foreach (string row in Terminal.Wrap($"  {command}", width, 4))
                lines.Add((row, UiLevel.Info));
            foreach (string row in Terminal.Wrap($"    {text}", width, 4))
                lines.Add((row, UiLevel.Detail));
        }
        void Flag(string flag, string text)
        {
            int column = Math.Min(FlagColumn, Math.Max(width / 3, 12));
            bool fits = flag.Length + 2 < column;
            string start = fits ? flag.PadRight(column) : flag;
            if (!fits)
                lines.Add((flag, UiLevel.Info));
            string body = fits ? start + text : new string(' ', column) + text;
            foreach (string row in Terminal.Wrap(body, width, column))
                lines.Add((row, UiLevel.Info));
        }

        lines.Add(("用法", UiLevel.Heading));
        Command("hacbrewpacklz", "双击打开主界面，或在命令行窗口中从文件夹构建 NSP。");
        Command("hacbrewpacklz compress [options] <file or folder>...", "Rebuild NSP, NSZ, XCI or XCZ files as compressed NSPs. convert does the same.");
        Command("hacbrewpacklz analyze [options] <file or folder>...", "只显示结果体积，不写入任何文件。未指定输入时，使用构建用的文件夹。");
        Command("hacbrewpacklz extract [options] <file or folder>...", "把 NSP、NSZ、XCI 或 XCZ 解包到文件夹。");
        Command("hacbrewpacklz check [options] <file or folder>...", "Check games for damage by reading all of each back through its hashes, writing nothing, as the Switch checks a game.");
        Command("hacbrewpacklz details [-k <keyset>] [--measure] <game file> [its update and DLC]...", "What there is to know about a game, as Details shows it on the screens: what compressing it would make of it, what's in it, the file and what it needs.");
        Command("hacbrewpacklz look [-k <keyset>] <game file> [folder or file inside it] [--all] [--save [--to <folder>]]", "逐层查看游戏内部文件：文本文件显示内容，图片会标注类型，其它文件显示开头字节。--all 逐行列出其下所有文件；--save 把选中内容或整个游戏保存到游戏旁的「游戏文件」文件夹，也可用 --to 指定位置。");
        Command("hacbrewpacklz verify -k <keyset> <file.nsp> [--romfsdir <dir>] [--exefsdir <dir>]", "校验已生成的 NSP，如提供文件夹则与之比对。");
        Command("hacbrewpacklz send <file or folder>...", "Share NSP, NSZ, XCI or XCZ files, or folders of games, with hacBrewPackLZ on a Switch, over the network or a USB cable, so it can install them. Takes no options. Runs until Ctrl+C.");
        Command("hacbrewpacklz menu", "Open the home screen. Running it with nothing after it does too.");
        Command("hacbrewpacklz update", "如有比当前更新的版本则安装。");
        Command("hacbrewpacklz report [what you were doing]", "Send a report to the developer: the version, the system and the settings, with what you say and no keys. If it can't be sent, it's saved on your desktop instead.");

        Heading("关于");
        Paragraph($"{AppInfo.Name} {AppInfo.Version}. {OptionCatalog.WelcomeIntro} {OptionCatalog.License}");
        lines.Add(("", UiLevel.Info));
        Paragraph("像 hacBrewPack 一样构建可安装的 NSP，通过 NCA 压缩层以 LZ4 压缩 Program NCA 的 RomFS 区段。输入文件与文件夹永远不会被修改。");
        lines.Add(("", UiLevel.Info));
        Paragraph("把游戏文件或文件夹拖到程序上即可在主界面打开。将 prod.keys 放在程序旁，或使用 -k 指定。");

        Heading("转换、分析与解包");
        Paragraph("Folders are searched for .nsp, .nsz, .xci and .xcz files, including subfolders, and for games extracted to folders, which convert builds into NSPs as the home screen does. An update given with its base game is applied to it, so the result is one title: the newest update wins and a repacked title holds no ticket. DLC stays separate. With --mergedlc a game, its update and all its DLC are packed into one NSP, each with its own content meta. An update without its base game can't be processed. Output goes beside the input unless you choose otherwise.");
        lines.Add(("", UiLevel.Info));
        Flag("--only <list>", "仅处理这些游戏，按标题 ID 或名称片段指定，用逗号分隔");
        Flag("--skip <list>", "排除这些游戏，按标题 ID 或名称片段指定，用逗号分隔");

        foreach (var group in OptionCatalog.Groups)
        {
            Heading(group.Title);
            foreach (var option in group.Options)
                Flag(option.Short is null ? option.Flag : $"{option.Short}, {option.Flag}", option.CommandHelp ?? option.Help);
        }

        Heading("其他");
        Flag("-h, --help", "显示此帮助");
        Flag("--backupdir", "为兼容而接受，但会被忽略；源文件永远不会被修改");
        return lines;
    }

    public static string Plain(int width)
    {
        var text = new StringBuilder();
        foreach (var (line, _) in Lines(width))
            text.AppendLine(line);
        return text.ToString();
    }
}

/// <summary>The choices made on the console screen or in the GUI, kept between runs as ordinary command line options.</summary>
internal sealed class Settings
{
    private readonly Dictionary<string, string> _values = new(StringComparer.Ordinal);

    private static string FilePath =>
        Environment.GetEnvironmentVariable("HACBREWPACKLZ_SETTINGS")
        ?? Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), AppInfo.Name, "settings.txt");

    /// <summary>Where the settings are kept, which is where anything else this keeps for itself goes too.</summary>
    public static string Folder => Path.GetDirectoryName(Path.GetFullPath(FilePath))!;

    public static Settings Load()
    {
        var settings = new Settings();
        try
        {
            foreach (string line in File.ReadLines(FilePath))
            {
                int eq = line.IndexOf('=');
                string flag = eq > 0 ? line[..eq] : line;
                string value = eq > 0 ? line[(eq + 1)..] : "1";
                // What only the command line goes by is dropped, should an earlier version have saved it.
                if (flag is WelcomedKey or ProblemsAskedKey or ViewKey || (OptionCatalog.AllOptions.Any(o => o.Flag == flag) && !OptionCatalog.CommandLineOnly.Contains(flag)))
                    settings._values[flag] = value;
                // Hide compressed games, which said which view the list started on, has given way to the view it was last on: turned off, the
                // list started on everything, and does the once more.
                else if (flag == "--showcompressed" && value == "1")
                    settings._values.TryAdd(ViewKey, "everything");
            }
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
            // No settings yet, or they can't be read: start from the defaults.
        }
        return settings;
    }

    public void Save()
    {
        try
        {
            Directory.CreateDirectory(Path.GetDirectoryName(FilePath)!);
            // Written beside the old one and then put in its place, as on the Switch: written over in place, the power going or the program
            // being stopped in that moment left it empty, or cut off part of the way through a folder's name.
            string fresh = FilePath + ".new";
            using (var file = new FileStream(fresh, FileMode.Create, FileAccess.Write, FileShare.None))
            using (var writer = new StreamWriter(file))
            {
                foreach (var (flag, value) in _values)
                    writer.WriteLine($"{flag}={value}");
                writer.Flush();
                file.Flush(true);
            }
            File.Move(fresh, FilePath, overwrite: true);
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
            Ui.Warn($"Couldn't save the settings: {e.Message}");
        }
    }

    /// <summary>The value as entered, with a toggle's being "1" when its flag is on. Empty means the default.</summary>
    public string Get(string flag) => _values.GetValueOrDefault(flag, "");

    /// <summary>What's set, as it's saved.</summary>
    public IEnumerable<string> Lines() => _values.Select(v => $"{v.Key}={v.Value}");

    /// <summary>The welcome has been and gone. Kept beside the options, though it isn't one and never goes to a job. As sending problems to
    /// the developer is on the welcome, having been through it counts as having been asked about that too.</summary>
    public bool Welcomed
    {
        get => _values.ContainsKey(WelcomedKey);
        set
        {
            if (value)
            {
                _values[WelcomedKey] = "1";
                _values[ProblemsAskedKey] = "1";
            }
            else
            {
                _values.Remove(WelcomedKey);
            }
        }
    }

    /// <summary>
    /// Sending problems to the developer has been asked about: on the welcome, or the once, for whoever had the welcome before that was on
    /// it. Asked only if the welcome has been, as the welcome asks it itself.
    /// </summary>
    public bool ProblemsAsked
    {
        get => _values.ContainsKey(ProblemsAskedKey);
        set
        {
            if (value)
                _values[ProblemsAskedKey] = "1";
            else
                _values.Remove(ProblemsAskedKey);
        }
    }

    public bool AskAboutProblems => Welcomed && !ProblemsAsked;

    private const string WelcomedKey = "welcomed";
    private const string ProblemsAskedKey = "problemsasked";
    private const string ViewKey = "view";

    /// <summary>The view the list was last on, which it opens on next time, as on the Switch: "todo" to begin with, "done" or "everything".</summary>
    public string View
    {
        get => _values.GetValueOrDefault(ViewKey, "todo");
        set => _values[ViewKey] = value;
    }

    /// <summary>The answer to the question asked the once: sending them, or not, and not asking again.</summary>
    public void AnswerAboutProblems(bool send)
    {
        if (send)
            Set(ProblemReports.Flag, "1");
        ProblemsAsked = true;
        Save();
    }

    public void Set(string flag, string value)
    {
        if (value.Length == 0)
            _values.Remove(flag);
        else
            _values[flag] = value;
    }

    /// <summary>Parses just the options a job reads, so it reports problems in the same words as the command line.</summary>
    public Options ToOptions(OptionUse use) => Options.Parse(ToArguments(use));

    public string[] ToArguments(OptionUse use)
    {
        var args = new List<string>();
        foreach (var option in OptionCatalog.AllOptions)
        {
            if ((option.Use & use) == 0 || OptionCatalog.CommandLineOnly.Contains(option.Flag))
                continue;
            if (!_values.TryGetValue(option.Flag, out string? value))
            {
                if (!option.DefaultApplies)
                    continue;
                value = option.Default;
            }
            if (option.Kind != OptionKind.Toggle)
                args.Add($"{option.Flag}={value}");
            else if (value == "1")
                args.Add(option.Flag);
        }
        return args.ToArray();
    }
}
