namespace HacBrewPackLz;

/// <summary>One fact on a game's Details: what it is, its value, and a word more beside it.</summary>
internal sealed record DetailsLine(string Label, string Value, string Note = "", bool Good = false, bool Bad = false);

internal sealed record DetailsSection(string Title, List<DetailsLine> Lines);

/// <summary>The one figure that decides, as Details shows it first: what it's said to be, the figure, and a line saying what it's beside. With
/// what it comes to as a share of what it is now, for the bar, when there's a saving to show.</summary>
internal sealed record DetailsFigure(string Label, string Big, string Sub, double? Share);

/// <summary>
/// What a game's Details say, the same in every version: the figure that decides, and the facts in four places that are always the same, what's
/// in it, the file, what it needs and compressing it, and then what's good to know. As the Switch app's (App::detailsOf), for game files.
/// </summary>
internal static class GameDetails
{
    /// <param name="inside">The game opened, or null while it's opening, or when it couldn't be, which <paramref name="problem"/> then says why.</param>
    public static List<DetailsSection> Sections(TitleRow row, GameInside? inside, string? problem, bool compressing)
    {
        // What's wrong comes first, in a few words with what to do, with the full error under it.
        var wrong = new DetailsSection("问题所在", []);
        if (problem is not null)
            wrong.Lines.Add(new("正在打开", Help.PlainReason(problem, null), problem, Bad: true));
        if (inside is { KeysNewEnough: false })
            wrong.Lines.Add(new("密钥", $"你的密钥太旧：需要 {FirstSystemWith(inside.Generation)} 及更高系统版本的密钥",
                "请升级主机系统版本，然后重新运行 Lockpick_RCM", Bad: true));
        var inIt = new DetailsSection("内容", []);
        if (inside is not null)
        {
            string version = inside.DisplayVersion ?? $"v{inside.GameVersion}";
            inIt.Lines.Add(new("游戏本体", version, inside.WithUpdate ? "含升级补丁" : ""));
            inIt.Lines.Add(new("DLC", inside.Dlcs == 0 ? "无" : $"{inside.Dlcs}"));
            if (inside.Languages.Count > 0)
                inIt.Lines.Add(new("语言", Listed(inside.Languages)));
            inIt.Lines.Add(new("游戏 ID", $"{inside.GameId:X16}"));
        }
        else
        {
            inIt.Lines.Add(new("游戏本体", row.Detail));
        }

        DetailsSection place;
        if (row.IsFolder)
        {
            string folder = row.Files.FirstOrDefault() ?? "";
            place = new DetailsSection("文件夹", [
                new("类型", "游戏文件夹", "压缩时会打包为一个 NSP"),
                new("大小", ProgressReporter.Format(row.InputBytes)),
                new("位置", Path.GetDirectoryName(folder) ?? folder, Path.GetFileName(folder)),
            ]);
        }
        else
        {
            string file = row.Files[0];
            var (kind, what) = Path.GetExtension(file).ToLowerInvariant() switch
            {
                ".nsz" => ("NSZ", "打包存放，安装时解包"),
                ".xcz" => ("XCZ", "打包存放，安装时解包"),
                ".xci" => ("XCI", "游戏卡的副本"),
                ".nsp" => ("NSP", "与零售版相同"),
                _ => ("游戏文件", ""),
            };
            place = new DetailsSection("文件", [
                new("类型", kind, what),
                new("大小", ProgressReporter.Format(row.FileBytes), row.PackedBytes > 0 ? $"{ProgressReporter.Format(row.InputBytes)}，按原样安装" : ""),
                new("位置", Path.GetDirectoryName(Path.GetFullPath(file)) ?? file, Path.GetFileName(file)),
            ]);
            if (row.Files.Length > 1)
                place.Lines.Add(new("附带", row.Files.Length == 2 ? "另有 1 个文件" : $"另有 {row.Files.Length - 1} 个文件",
                    string.Join(", ", row.Files.Skip(1).Select(Path.GetFileName))));
        }

        var needs = new DetailsSection("运行要求", []);
        if (inside is not null)
        {
            needs.Lines.Add(new("系统版本", inside.RequiredSystem == 0 ? "任意版本" : $"{VersionName(inside.RequiredSystem)} 或更高"));
        }
        else
        {
            needs.Lines.Add(new("未知", problem is null ? "正在打开以获取信息" : "无法打开，因此无法获知"));
        }
        if (row.IsCompressed && row.Estimate?.ByMakers != true)
            needs.Lines.Add(new("启动所需", "签名补丁", "所有压缩游戏皆如此"));

        return wrong.Lines.Count > 0 ? [wrong, inIt, place, needs, Doing(row, compressing)] : [inIt, place, needs, Doing(row, compressing)];
    }

    /// <summary>What compressing it does, or did, or with compression off, what a repack does.</summary>
    private static DetailsSection Doing(TitleRow row, bool compressing)
    {
        var estimate = row.Estimate;
        if (!compressing)
        {
            return new DetailsSection("重打包后", [
                row.HasUpdate ? new("正在重打包", "把更新补丁并入本体", "两个文件合为一个") : new("正在重打包", "没有收益", "没有可并入的更新补丁"),
            ]);
        }
        var doing = new DetailsSection("压缩后", []);
        if (row.IsDone(BatchMode.Convert))
            doing.Lines.Add(new("已完成", "已在此压缩", row.OutputPath is { } output ? Path.GetFileName(output) : "", Good: true));
        else if (row.IsCompressed)
            doing.Lines.Add(new("已完成", estimate?.ByMakers == true ? "发行商已自行压缩" : "已经是压缩格式", "发送到 Switch 后按原样安装"));
        if (!row.IsCompressed && !row.IsDone(BatchMode.Convert))
            doing.Lines.Add(new("处理后", "启动需要签名补丁", "所有压缩游戏皆如此"));
        return doing;
    }

    /// <summary>The one figure that decides: what compressing it would make of it, or did, or that it's as small as it gets.</summary>
    public static DetailsFigure Figure(TitleRow row, bool compressing)
    {
        long now = row.InputBytes;
        var estimate = row.Estimate;
        if (row.IsDone(BatchMode.Convert) && row.OutputBytes > 0)
        {
            long freed = row.InputBytes - row.OutputBytes;
            return freed > 0
                ? new(compressing ? "压缩后缩小了" : "重打包后缩小了", ProgressReporter.Format(freed),
                    $"新文件为 {ProgressReporter.Format(row.OutputBytes)}", (double)row.OutputBytes / row.InputBytes)
                : new(compressing ? "压缩后为" : "重打包后为", ProgressReporter.Format(row.OutputBytes), "", null);
        }
        if (compressing && row.IsCompressed)
        {
            if (estimate is { FreedBytes: > 0 } compressed)
                return new("压缩后减少了", ProgressReporter.Format(compressed.FreedBytes), $"现在为 {ProgressReporter.Format(now)}",
                    (double)now / (now + compressed.FreedBytes));
            return new(estimate?.ByMakers == true ? "发行商压缩后为" : "压缩后为", ProgressReporter.Format(now), "", null);
        }
        if (estimate is null)
            return new(compressing ? "压缩可节省" : "重打包可节省", "尚未计算", "", null);
        if (!compressing)
            return new("重打包后约占用", ProgressReporter.Format(estimate.OutputBytes), $"原为 {ProgressReporter.Format(estimate.InputBytes)}",
                estimate.OutputBytes < estimate.InputBytes ? (double)estimate.OutputBytes / estimate.InputBytes : null);
        if (row.NotSure)
            return new("压缩可节省", "尚未确定", "完整分析可测出", null);
        if (estimate.SureSavedBytes > 0 && row.BelowMinimum is null)
            return new("压缩后约占用", ProgressReporter.Format(estimate.MostBytes), $"原为 {ProgressReporter.Format(estimate.InputBytes)}",
                1 - estimate.SureSavedFraction);
        return new("保持原样占用", ProgressReporter.Format(estimate.InputBytes), "压缩节省太少", null);
    }

    /// <summary>What's good to know about it, what went wrong with it first, as the list has only room for the start of it.</summary>
    public static List<string> Notes(TitleRow row)
    {
        var notes = new List<string>();
        if (row.State == RowState.Failed && row.Failure is { Length: > 0 } failure)
            notes.Add(Capitalized(failure));
        if (row.NotSure)
            notes.Add(TitleRow.NotSureLong);
        notes.AddRange(row.Notes);
        return notes;
    }

    /// <summary>A system version as a game's record writes it, the way people know it: 0x30000000 is 12.0.0.</summary>
    public static string VersionName(uint version) => $"{version >> 26}.{(version >> 20) & 0x3F}.{(version >> 16) & 0xF}";

    /// <summary>The first system version each key generation came with, which the keys it takes are dumped from.</summary>
    public static string FirstSystemWith(int generation)
    {
        string[] first = ["1.0.0", "3.0.0", "3.0.1", "4.0.0", "5.0.0", "6.0.0", "6.2.0", "7.0.0", "8.1.0", "9.0.0", "9.1.0", "12.1.0", "13.0.0", "14.0.0",
            "15.0.0", "16.0.0", "17.0.0", "18.0.0", "19.0.0", "20.0.0", "21.0.0"];
        return generation >= 0 && generation < first.Length ? first[generation] : "较新的版本";
    }

    /// <summary>Up to four, then how many more: "英语、法语、德语、日语", "英语、法语、德语、意大利语等 10 种".</summary>
    private static string Listed(List<string> names)
    {
        int shown = Math.Min(names.Count, 4);
        var listed = new System.Text.StringBuilder();
        for (int i = 0; i < shown; i++)
            listed.Append(i == 0 ? "" : i + 1 == shown && shown == names.Count ? " and " : ", ").Append(names[i]);
        if (names.Count > shown)
            listed.Append($" 等 {names.Count - shown} 种");
        return listed.ToString();
    }

    private static string Capitalized(string text) => text.Length > 0 ? char.ToUpperInvariant(text[0]) + text[1..] : text;
}
