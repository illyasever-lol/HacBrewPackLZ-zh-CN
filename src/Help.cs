using System.Text;

namespace HacBrewPackLz;

/// <summary>
/// A few lines on what people most often need to know, each with where to read the rest, what to try next when something fails,
/// and a report of what happened that can be sent to someone without any keys in it. The Switch app has the same, in app_help.cpp.
/// </summary>
internal static class Help
{
    public const string Page = "https://gitlab.com/ismaellozanoperezserrano/hacbrewpacklz/-/blob/main/docs/help.md";
    public const string NewIssue = "https://gitlab.com/ismaellozanoperezserrano/hacbrewpacklz/-/issues/new";
    public const string ReportFileName = "hacBrewPackLZ report.txt";

    public sealed record Topic(string Title, string Text, string Anchor)
    {
        public string Url => $"{Page}#{Anchor}";
    }

    /// <summary>The same topics as docs/help.md, which says more.</summary>
    public static readonly Topic[] Topics =
    [
        new("游戏无法启动",
            "压缩游戏需要签名补丁（sigpatches），且 Switch 系统版本需为 12.0.0 或更高。缺少这些条件时，主机会提示游戏已损坏或要求系统更新。\n\n" +
            "在压缩游戏上覆盖安装官方更新会导致其无法启动，因为更新只适用于它所针对的原始游戏。可在 Switch 端的「已安装游戏」中把更新并入本体；或删除该更新，把它与压缩游戏一起交给本程序处理。\n\n" +
            "由「必须配合更新补丁才完整」的游戏、用 1.4.3 或更早版本生成的成品，即便内容完整也可能提示需要更新且无法启动。Switch 端的概览会说明此情况，用「修复」即可解决；也可把原始本体与更新补丁重新压缩后再安装。\n\n" +
            "若某个游戏曾被 DBI、sphaira 等安装器在较旧系统上启动过，再用 1.5.0 测试版 2 或更早的 Switch 端压缩，可能会提示需要系统更新。概览同样会说明，用「修复」即可解决。\n\n" +
            "若都不属于以上情况，请报告问题，并附上该游戏和错误代码。",
            "a-game-wont-start"),
        new("为压缩游戏添加更新补丁",
            "更新补丁必须整合进经过压缩或重新打包的游戏文件中（hacBrewPackLZ 工具可实现此操作）。如果直接单独安装更新补丁，会导致游戏无法启动。\n\n" +
            "在电脑或手机上：将压缩或重新打包后的游戏文件与新的更新补丁放在一起，然后点击“压缩”（Compress）。完成后，安装这个新的压缩版游戏即可。\n\n" +
            "在 Switch 主机上：按常规方式安装更新补丁。此时，该游戏会出现在“已安装游戏”（Installed games）列表中；按下 X 键，即可将更新补丁整合进游戏文件中。此外，你也可以将更新补丁文件放入 SD 卡，或通过电脑共享该文件；系统会在“游戏文件”（Game files）中识别出该补丁，按下 X 键即可将其整合进已安装的游戏中，替换掉原有的内容。\n\n" +
            "通常情况下无需原始游戏文件，但有两种例外情况（hacBrewPackLZ 会在必要时提示你提供）：\n\n" +
            "若游戏是在 1.5 版本之前的工具中进行压缩且内含更新补丁：此时需要同时提供该补丁文件或原始游戏文件。部分游戏仍需原始文件才能正常处理。\n\n" +
            "若游戏由开发者直接压缩（常见于许多较新的游戏）：此时必须提供原始游戏文件。这类游戏本身已处于极度精简的状态。\n\n" +
            "你的存档数据不会丢失。若要防止主机自动下载更新，请在 Switch 的“系统设置”（System Settings）->“系统”（System）菜单中，关闭“软件自动更新”（Auto-Update Software）功能。",
            "updating-a-game"),
        new("缺少密钥",
            "「密钥集中不存在」表示你的密钥比该游戏更旧。请升级 Switch 系统版本，重新运行 Lockpick_RCM，并使用新导出的 prod.keys.",
            "missing-keys"),
        new("缺少票据",
            "「需要标题密钥」或「个性化票据」表示该文件中没有游戏票据。请使用含票据的游戏副本，多数转储工具都可包含它.",
            "missing-tickets"),
        new("为什么有些游戏几乎压不动",
            "许多新游戏本身已经打包得很紧，或把大部分数据放在压缩覆盖不到的位置。「分析」会在实际处理前算出每个游戏能节省的空间；实测低于选项中「最小节省比例」的游戏会归入「已完成」并说明原因。",
            "games-that-barely-shrink"),
        new("把游戏发送到 Switch",
            "点击「发送到 Switch」，并在同一 Wi-Fi 下打开 Switch 端的 hacBrewPackLZ。游戏会出现在「游戏文件」中：勾选后按 X。也可用 USB 数据线。Windows 需要用 Zadig 安装 WinUSB 驱动；Mac 需要 libusb（brew install libusb）；Linux 一般自带 libusb，另需执行一次以下规则以便当前用户访问 Switch：echo 'SUBSYSTEM==\"usb\", ATTRS{idVendor}==\"057e\", ATTRS{idProduct}==\"3000\", MODE=\"0666\"' | sudo tee /etc/udev/rules.d/70-hacbrewpacklz.rules，然后执行 sudo udevadm control --reload-rules && sudo udevadm trigger",
            "sending-games-to-the-switch"),
        new("文件都放在哪里",
            "压缩后的游戏放在原始文件旁，文件名以 [LZ4].nsp 结尾，除非在选项中指定了其他位置。「查看内部」保存的内容放在游戏旁的「游戏文件」文件夹，Android 上则由「保存到…」决定。报告会保存到桌面，Android 上则通过分享发送。",
            "where-things-are"),
    ];

    /// <summary>What went wrong in a few plain words, for a list, where a failure as it's said, with its files, offsets and codes, doesn't tell
    /// people much. The failure itself is kept for where people go for the details, which <paramref name="whereItSays"/> points to when nothing
    /// plainer can be said. The same sentences as the Switch's (plainReason in switch/app/app_help.cpp).</summary>
    public static string PlainReason(string failure, string? whereItSays)
    {
        bool Has(string text) => failure.Contains(text, StringComparison.Ordinal);
        // The card's own trouble first, as some of it says "damaged" too.
        return Has("file system it was writing to is damaged") ? "SD 卡文件系统已损坏，请在电脑上检查。"
            : Has("(2001-0123)") ? "SD 卡无响应，请重新打开程序。"
            : Has("(2001-0106)") || Has("(2001-0105)") ? "SD 卡正忙，请重试。"
            // Before anything the firmware's own error, which follows, could be taken for. The words before 1.6 too, as a report from an older
            // version still says them.
            : Has("can't start compressed games yet") || Has("firmware won't read compressed games") ? "这台主机目前还无法启动压缩游戏。"
            : Has("doesn't read back the same") ? "SD 卡可能即将损坏，请在电脑上检查。"
            : Has("differently from") ? "新生成的文件不正确，因此保留原始文件。"
            : Has("isn't a readable NCA3") || Has("header_key") ? "你的密钥无法打开这个游戏。"
            : Has("not in your keys") || Has("not in the keyset") ? "你的密钥对这个游戏来说太旧了。"
            : Has("needs a title key") || Has("personalized ticket") ? "该游戏缺少票据。"
            // A hash check that fails where an update is laid over its game can be either, as the failure says, so this says so too.
            : Has("doesn't belong to this base game") ? "文件已损坏，或更新补丁与本体版本不匹配：请两者都检查。"
            // A game compressed or repacked that doesn't have a part of the original an update reads. Before 1.5 the error said "the compressed
            // copy", which a saved error can still say.
            : Has("compressed game doesn't have") || Has("repacked game doesn't have") || Has("the compressed copy doesn't have") ? "这个更新补丁需要原始本体游戏。"
            : Has("enough room") || Has("not enough space") ? "可用空间不足。"
            : Has("enough memory") || Has("enough free memory") ? "内存不足。"
            : Has("can't be found any more") ? "共享该文件的电脑已停止共享。"
            // The system's own code, where the words don't say.
            : FileSystemCodeReason(failure) is { } coded ? coded
            // Anything else that stops a DLC going into its game's file (TitleConverter.Run). With the setting off, the game is made without it.
            : failure.StartsWith("Its DLC", StringComparison.Ordinal) ? "某个 DLC 已损坏：请重新获取，或关闭「整合打包」。"
            : Has("damaged") || Has("hash check failed") || Has("doesn't match") || Has("lies outside") || Has("cut short") || Has("BKTR")
                || Has("ended earlier than expected") ? "已损坏：请重新转储或下载。"
            : whereItSays is null ? "出错了。" : $"出错了。{whereItSays}";
    }

    /// <summary>What the file system's own error code (2002-NNNN) in a failure means, where nothing in its words says: each number, or range of
    /// them, as the system's list of them has it. The first code with a meaning goes; null if there's none. As plainForFileSystemCode on the
    /// Switch.</summary>
    private static string? FileSystemCodeReason(string failure)
    {
        for (int at = failure.IndexOf("(2002-", StringComparison.Ordinal); at >= 0; at = failure.IndexOf("(2002-", at + 6, StringComparison.Ordinal))
        {
            if (at + 11 > failure.Length || failure[at + 10] != ')' || !int.TryParse(failure.AsSpan(at + 6, 4), System.Globalization.NumberStyles.None, null, out int code))
                continue;
            string? said = code switch
            {
                1 => "所需的某个文件已不存在。",
                7 => "所需的某个文件被占用，请重试。",
                >= 30 and <= 45 => "可用空间不足。",
                1004 => "该游戏缺少票据。",
                >= 2000 and <= 2499 => "无法访问 SD 卡，请确认已正确插入。",
                >= 2500 and <= 2999 => "无法读取游戏卡，请确认已正确插入。",
                >= 3200 and <= 3499 => "内存不足。",
                >= 3500 and <= 3999 => "无法访问主机自带存储，请重启主机。",
                // The SD card's FAT itself, before the rest of what says data is corrupted.
                >= 4681 and <= 4699 => "SD 卡文件系统已损坏，请在电脑上检查。",
                >= 4000 and <= 4999 => "已损坏：请重新转储或下载。",
                _ => null,
            };
            if (said is not null)
                return said;
        }
        return null;
    }

    /// <summary>What's worth trying next, after a failure people can do something about. Just the failure, otherwise.</summary>
    public static string WithNextStep(string failure)
    {
        bool Has(string text) => failure.Contains(text, StringComparison.Ordinal);
        string? next =
            Has("isn't a readable NCA3") || Has("header_key") ? "你的密钥可能不属于这台主机，或已损坏：请用 Lockpick_RCM 重新导出。"
            : Has("not in the keyset") || Has("not in your keys") ? "请升级 Switch 系统版本，然后用 Lockpick_RCM 重新导出密钥。"
            : Has("needs a title key") || Has("personalized ticket") ? "请使用含票据的游戏副本。"
            : Has("doesn't belong to this base game") ? "若该游戏在压缩或重打包时并入过较早的更新补丁，请同时提供那个更新补丁文件，或原始本体游戏。"
            : Has("BKTR") || Has("damaged") || Has("doesn't match") || Has("lies outside the file") || Has("cut short")
                ? "请重新转储或下载，并确认更新补丁与该游戏版本匹配。"
            : Has("enough free memory") ? "请关闭其他程序后重试。"
            : Has("Something unexpected went wrong") ? "请通过「报告问题」反馈此情况。"
            : null;
        return next is null || Has(next) ? failure : $"{failure.TrimEnd('.', ' ')}。{next}";
    }

    /// <summary>Every key's value taken out, whatever the case it's in. Nothing here writes keys down, but a report goes to someone
    /// else, so it's made sure of. The user's own folder is shortened too, as it holds their name.</summary>
    public static string Clean(string text, Keyset? keyset)
    {
        if (keyset is not null)
        {
            var secrets = keyset.Secrets().ToHashSet(StringComparer.OrdinalIgnoreCase);
            var lengths = secrets.Select(s => s.Length).Distinct().OrderBy(l => l).ToArray();
            var output = new StringBuilder(text.Length);
            int i = 0;
            while (i < text.Length)
            {
                if (!Uri.IsHexDigit(text[i]))
                {
                    output.Append(text[i++]);
                    continue;
                }
                int end = i;
                while (end < text.Length && Uri.IsHexDigit(text[end]))
                    end++;
                string run = text[i..end];
                bool secret = lengths.Any(length => Enumerable.Range(0, Math.Max(run.Length - length + 1, 0)).Any(at => secrets.Contains(run.Substring(at, length))));
                output.Append(secret ? "[已省略密钥]" : run);
                i = end;
            }
            text = output.ToString();
        }
        string home = Environment.GetFolderPath(Environment.SpecialFolder.UserProfile);
        return home.Length > 3 ? text.Replace(home, "~", StringComparison.OrdinalIgnoreCase) : text;
    }

    /// <summary>Somewhere else for reports to go, for testing, so that a test leaves the desktop alone.</summary>
    private const string FolderVariable = "HACBREWPACKLZ_REPORT_FOLDER";

    /// <summary>Saved where people will find it: on the desktop, or in their own folder when there isn't one.</summary>
    public static string SaveReport(string text)
    {
        string folder = Environment.GetEnvironmentVariable(FolderVariable) ?? Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory);
        if (folder.Length == 0 || !Directory.Exists(folder))
            folder = Environment.GetFolderPath(Environment.SpecialFolder.UserProfile);
        string path = Path.Combine(folder, ReportFileName);
        File.WriteAllText(path, text);
        return path;
    }

    /// <summary>Shown in the file manager, other than in a test, which would leave a window open on someone's screen.</summary>
    public static void ShowReport(string path)
    {
        if (Environment.GetEnvironmentVariable(FolderVariable) is null)
            Explorer.Show(path);
    }
}
