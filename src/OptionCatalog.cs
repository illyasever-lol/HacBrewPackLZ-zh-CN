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

    /// <summary>Choice 也允许直接输入自定义值：GUI 会同时给出输入框与预置下拉。</summary>
    public bool Editable { get; init; }
    public int Min { get; init; }
    public int Max { get; init; }

    /// <summary>Starting value for a slider or a choice.</summary>
    public string Default { get; init; } = "";

    /// <summary>The home screen and the GUI go by the default as if it had been chosen, even though the command line doesn't.</summary>
    public bool DefaultApplies { get; init; }

    /// <summary>The toggle reads "on" when the flag is absent.</summary>
    public bool Inverted { get; init; }

    /// <summary>Help for the command line when it has to say more than the GUI does.</summary>
    public string? CommandHelp { get; init; }

    /// <summary>What an action's button says.</summary>
    public string Verb { get; init; } = "";
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
            "将 RomFS 以 LZ4 压缩形式存储；关闭后像 hacBrewPack 一样不压缩存储。")
        {
            Inverted = true, CommandHelp = "像 hacBrewPack 一样不压缩存储 RomFS",
        },
        new("--level", "压缩级别", OptionKind.Slider, Rebuilds,
            "1 最快，12 压缩率最高，9 是比较均衡的选择。")
        {
            Min = Lz4Encoder.MinLevel, Max = Lz4Encoder.MaxLevel, Default = "9",
            CommandHelp = "LZ4 级别，从 1（最快）到 12（最小），默认 9",
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
            "节省比例低于此值的游戏会被排除，把时间用在收益最大的游戏上。你仍可手动勾选它们。")
        {
            Choices = ["Off", "5%", "10%", "15%", "20%", "25%", "30%", "40%", "50%"], Default = "5%", DefaultApplies = true,
            CommandHelp = "跳过节省比例低于该百分比的游戏，每个游戏都会先分析。默认关闭",
        },
        new("--hidesmall", "隐藏 1 MB 以下的游戏", OptionKind.Toggle, OptionUse.Convert | OptionUse.Extract,
            "排除没有升级补丁或 DLC 的小体积游戏（如启动器），这类不值得列出。")
        {
            Default = "1", DefaultApplies = true,
            CommandHelp = "Leave out games under 1 MB with no update or DLC, like forwarders. On to begin with on the home screen and in the GUI",
        },
        new("--showcompressed", "隐藏已压缩游戏", OptionKind.Toggle, 0,
            "已压缩的游戏不会出现在列表中（例如本工具生成的副本与原始文件放在一起时）。列表底部会显示隐藏了多少个。")
        {
            Inverted = true, CommandHelp = "在主界面和 GUI 中也列出已压缩的游戏",
        },
    ]);

    public static readonly OptionGroup Output = new("输出",
    [
        new("--nspdir", "NSP 输出文件夹", OptionKind.Folder, Rebuilds,
            "生成的 NSP 存放位置。")
        {
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
        new("--mergedlc", "是否整合UPD+DLC+本体 为单一文件", OptionKind.Toggle, Rebuilds,
            "开启后，同一游戏的本体、更新补丁与全部 DLC 合并为一个 NSP（内含各自的内容元数据）；关闭时 DLC 仍单独输出。")
        {
            CommandHelp = "把同一 Title ID 下的本体、更新补丁和 DLC 打包进一个 NSP，每个标题保留自己的 CNMT",
        },
        new("--verify", "校验每个生成的 NSP", OptionKind.Toggle, Rebuilds,
            "回读 NSP 并校验每一个哈希、块和表，耗时会更长。"),
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
            "主机密钥文件。程序会自动查找配置的 prod.keys。")
        {
            Short = "-k", Placeholder = "程序目录下的 prod.keys",
            CommandHelp = "设置密钥文件路径，默认依次查找 keys.dat、keys.txt、keys.ini 和 prod.keys",
        },
        new("--keygeneration", "密钥版本", OptionKind.Number, Rebuilds,
            "用哪个密钥区域密钥加密新生成的 NCA。")
        {
            Min = 1, Max = 32, Placeholder = "1",
            CommandHelp = "设置加密密钥区域所用的密钥版本，默认密钥版本为 1",
        },
        new("--keyareakey", "密钥区密钥 2", OptionKind.Hex, Rebuilds,
            "32 位十六进制字符。")
        {
            Placeholder = "04040404040404040404040404040404",
            CommandHelp = "以十六进制设置密钥区域密钥 2，长度 16 字节",
        },
        new("--sdkversion", "SDK 版本", OptionKind.Choice, Rebuilds,
            "按实测的密钥世代 / 固件范围选择；不选择则沿用各源文件自身的 SDK 版本。也可在下拉旁的输入框自定义填写，例如 11.0.0、0xA (9.1.0-12.0.3) 或 000C1100。下拉中的 0xD 是主密钥版本，NCA 头部会写入 1 起值 0xE，外部工具便显示为 0xD，同时避免 0xFF (Unknown)。")
        {
            Choices = FirmwareVersions.Choices,
            Default = "沿用源文件",
            Editable = true,
            Placeholder = "自定义填写，例如 11.0.0、0xA (9.1.0-12.0.3) 或 000C1100",
            CommandHelp = "选择固件范围（如 0xA (9.1.0-12.0.3)）同时设定 SDK 版本与密钥世代；也可自定义填写 11.0.0 等版本号或 000C1100 等十六进制",
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
            CommandHelp = "设置内容元数据的所需系统版本，可自定义填写 11.0.0、20.0.0 等版本号或 000C1100 等十六进制；不选择则读取源文件",
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
        new("--noautoupdate", "自动更新", OptionKind.Toggle, 0,
            "每次启动时会检查并安装新版本，所获取的版本必须带有正确的签名。")
        {
            Inverted = true, CommandHelp = "打开主界面和 GUI 时不检查新版本",
        },
    ]);

    public static readonly OptionGroup[] Groups = [Compression, Worthwhile, Output, Keys, Versions, BuildFolders, BuildTitle, Updates];

    /// <summary>Looks for a newer version there and then. The command line has the update command instead.</summary>
    public static readonly OptionSpec CheckForUpdates = new("check-for-updates", "检查更新", OptionKind.Action, 0,
        "立即检查新版本，如有则安装。")
    {
        Verb = "立即检查",
    };

    /// <summary>
    /// The groups a list of titles uses, leaving out what only building from folders needs, with checking for updates
    /// beside the setting that does it by itself.
    /// </summary>
    public static readonly OptionGroup[] GeneralGroups =
        [Compression, Worthwhile, Output, Keys, Versions, Updates with { Options = [.. Updates.Options, CheckForUpdates] }];

    public static IEnumerable<OptionSpec> AllOptions => Groups.SelectMany(g => g.Options);
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
        Command("hacbrewpacklz convert [选项] <文件或文件夹>...", "把 NSP、NSZ、XCI 或 XCZ 重建为压缩 NSP。");
        Command("hacbrewpacklz analyze [选项] <文件或文件夹>...", "只显示结果体积，不写入任何文件。未指定输入时，使用构建用的文件夹。");
        Command("hacbrewpacklz extract [选项] <文件或文件夹>...", "把 NSP、NSZ、XCI 或 XCZ 解包到文件夹。");
        Command("hacbrewpacklz verify -k <密钥文件> <文件.nsp> [--romfsdir <目录>] [--exefsdir <目录>]", "校验已生成的 NSP，如提供文件夹则与之比对。");
        Command("hacbrewpacklz send <文件或文件夹>...", "Share NSP, NSZ, XCI or XCZ files, or folders of games, with hacBrewPackLZ on a Switch, over the network or a USB cable, so it can install them. Takes no options. Runs until Ctrl+C.");
        Command("hacbrewpacklz menu", "打开主界面。");
        Command("hacbrewpacklz update", "如有比当前更新的版本则安装。");

        Heading("关于");
        Paragraph("Builds an installable NSP like hacBrewPack, with the RomFS section of the Program NCA stored LZ4 compressed through the NCA compression layer. Input files and folders are never modified.");
        lines.Add(("", UiLevel.Info));
        Paragraph("Dropping game files or folders onto the program opens them on the home screen. Put prod.keys beside the program, or use -k.");

        Heading("转换、分析与解包");
        Paragraph("会递归搜索文件夹中的 .nsp、.nsz、.xci 和 .xcz 文件。本体与更新补丁会合并为一个标题（最新版本生效，重打包后不再需要票据）；默认 DLC 单独输出，开启「是否整合UPD+DLC+本体 为单一文件」后，同一游戏的本体、更新补丁和 DLC 会一起打进一个 NSP。没有本体的更新补丁无法处理。输出默认与源文件同目录。");
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
                if (OptionCatalog.AllOptions.Any(o => o.Flag == flag))
                    settings._values[flag] = value;
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
            File.WriteAllLines(FilePath, _values.Select(v => $"{v.Key}={v.Value}"));
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
            Ui.Warn($"Couldn't save the settings: {e.Message}");
        }
    }

    /// <summary>The value as entered, with a toggle's being "1" when its flag is on. Empty means the default.</summary>
    public string Get(string flag) => _values.GetValueOrDefault(flag, "");

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
            if ((option.Use & use) == 0)
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
