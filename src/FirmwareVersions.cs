using System.Linq;

namespace HacBrewPackLz;

/// <summary>
/// NCA 的主密钥版本（Master Key Revision）与对应系统固件范围。
/// 锚点取自实测数据：0x3=4.0.0-4.1.0、0xA=9.1.0-12.0.3、0xD=14.0.0-14.1.2、0x11=18.0.0-18.1.0，
/// 其余按任天堂密钥世代表补全。选择某个固件范围时，新生成的 NCA 会写入相符的密钥世代，
/// 外部工具便不会再把该字段显示成 0xFF (Unknown)。
///
/// 关键：表里的 KeyGeneration（以及下拉 Label 里的 0xD）是外部工具显示的「主密钥版本」，0 起；
/// 而 NCA 头部 0x206 / 0x220 的字段是 1 起值 —— 本项目 NcaReader.GenerationIndex、
/// Verifier 以及外部工具都是先减 1 再查 key_area_key_application_XX。
/// 因此传给 Options.KeyGeneration、由 NcaWriter 写进头部的必须是 revision + 1，
/// 否则选 0xD 会写出 0xC、选 0xE 会写出 0xD，且加密密钥也错用一代。
/// </summary>
internal static class FirmwareVersions
{
    /// <summary>NCA 头部 SDK 版本字段允许的最小值，见 Options.Parse 的范围校验。</summary>
    public const uint MinSdkVersion = 0x000B0000;

    private readonly record struct Entry(int KeyGeneration, string Range, uint Firmware);

    // Firmware 用 (major<<16)|(minor<<8)|micro 表示，便于按版本比较。
    // 每行的 Firmware 是该固件范围的起始版本。
    private static readonly Entry[] Table =
    [
        new(0x00, "1.0.0-2.3.0", 0x00010000),
        new(0x01, "3.0.0", 0x00030000),
        new(0x02, "3.0.1-3.0.2", 0x00030001),
        new(0x03, "4.0.0-4.1.0", 0x00040000),
        new(0x04, "5.0.0-5.1.0", 0x00050000),
        new(0x05, "6.0.0-6.1.0", 0x00060000),
        new(0x06, "6.2.0", 0x00060200),
        new(0x07, "7.0.0-8.0.1", 0x00070000),
        new(0x08, "8.1.0-8.1.1", 0x00080100),
        new(0x09, "9.0.0-9.0.1", 0x00090000),
        new(0x0A, "9.1.0-12.0.3", 0x00090100),
        new(0x0B, "12.1.0", 0x000C0100),
        new(0x0C, "13.0.0-13.2.1", 0x000D0000),
        new(0x0D, "14.0.0-14.1.2", 0x000E0000),
        new(0x0E, "15.0.0-15.0.1", 0x000F0000),
        new(0x0F, "16.0.0-16.0.3", 0x00100000),
        new(0x10, "16.1.0-17.0.1", 0x00100100),
        new(0x11, "18.0.0-18.1.0", 0x00120000),
    ];

    /// <summary>下拉框选项。0x00 不单列：程序加密最小密钥世代为 1。</summary>
    public static readonly string[] Choices =
        ["沿用源文件", .. Table.Where(e => e.KeyGeneration > 0).Select(e => Label(e))];

    private static string Label(Entry e) => $"0x{e.KeyGeneration:X} ({e.Range})";

    /// <summary>把写入 NCA 头部的 1 起世代值，格式化成外部工具显示的主密钥版本，如 0xA (9.1.0-12.0.3)。</summary>
    public static string Describe(int keyGeneration)
    {
        int revision = Math.Max(keyGeneration - 1, 0);
        foreach (var e in Table)
            if (e.KeyGeneration == revision)
                return Label(e);
        return $"0x{revision:X} (未知)";
    }

    /// <summary>解析下拉项、单独的数字或固件范围文字，并给出应写入的 SDK 版本。
    /// keyGeneration 返回的是 NCA 头部 0x206/0x220 使用的 1 起值（主密钥版本 + 1）。</summary>
    public static bool TryFromChoice(string text, out int keyGeneration, out uint sdkVersion)
    {
        keyGeneration = 0;
        sdkVersion = MinSdkVersion;
        text = text.Trim();
        foreach (var e in Table)
        {
            if (text != Label(e) && text != $"0x{e.KeyGeneration:X}" && text != e.Range)
                continue;
            // 下拉里的 0xD 是主密钥版本（0 起）；头部字段是 1 起，读取方一律减 1。
            // 写入头部必须 +1，否则选 0xD 生成的文件被显示成 0xC（0xE 显示成 0xD），
            // key_area_key_application_XX 也会错用上一代。
            keyGeneration = e.KeyGeneration + 1;
            sdkVersion = Math.Max(e.Firmware, MinSdkVersion);
            return true;
        }
        return false;
    }

    /// <summary>按可读固件版本查它所属的密钥世代，例如 11.0.0 落在 9.1.0-12.0.3 里，得 0xA；
    /// 返回写入 NCA 头部的 1 起值（0xB）。</summary>
    public static int KeyGenerationFor(uint major, uint minor, uint micro)
    {
        uint firmware = (major << 16) | (minor << 8) | micro;
        int found = 0;
        foreach (var e in Table)
            if (firmware >= e.Firmware)
                found = e.KeyGeneration;
        return found + 1;
    }

    public static uint SdkForFirmware(uint major, uint minor, uint micro) =>
        Math.Max((major << 16) | (minor << 8) | micro, MinSdkVersion);
}
