using System.Globalization;

namespace HacBrewPackLz;

internal sealed class Options
{
    /// <summary>12.0.0 is the first firmware whose NCA format has a compression layer.</summary>
    public const uint CompressionMinSystemVersion = 12u << 26;

    public string? KeysetPath;
    public List<string> Inputs = [];
    public string NspDir = "hacbrewpack_nsp";
    public bool NspDirSet;
    public string NcaDir = "hacbrewpack_nca";
    public string? TempDir;
    public string? OutputPath;
    public string? OutDir;
    public string ExefsDir = "exefs";
    public string RomfsDir = "romfs";
    public string LogoDir = "logo";
    public string ControlDir = "control";
    public string? HtmlDocDir;
    public string? LegalInfoDir;
    public bool NoRomfs;
    public bool NoLogo;
    public bool Plaintext;
    public bool KeepNcaDir;
    public bool NoSignNcaSig2;
    public bool NoPatchNacpLogo;
    public int KeyGeneration = 1;
    public bool KeyGenerationSet;
    public byte[] KeyAreaKey = Enumerable.Repeat((byte)0x04, 16).ToArray();
    public uint SdkVersion = 0x000C1100;
    public bool SdkVersionSet;
    public uint? TitleVersion;
    public uint? RequiredSystemVersion;
    public ulong TitleId;
    public string? TitleName;
    public string? TitlePublisher;

    public bool Compress = true;
    public int Level = 9;
    public int BlockSize = 0x10000;
    public int Threads = Environment.ProcessorCount;
    public bool VerifyAfterBuild;

    /// <summary>Pack a base game, its update and all its DLC into one NSP holding several content metas.</summary>
    public bool MergeDlc;

    /// <summary>Titles that would save less than this percentage aren't worth the time. Zero means none are left out.</summary>
    public int MinSavingPercent;

    /// <summary>Games under 1 MB with nothing else to them, like forwarders, are left out.</summary>
    public bool HideSmall;

    public List<string> OnlyTitles = [];
    public List<string> SkipTitles = [];

    public static Options Parse(string[] args)
    {
        var o = new Options();
        for (int i = 0; i < args.Length; i++)
        {
            string arg = args[i];
            string? inline = null;
            int eq = arg.IndexOf('=');
            if (arg.StartsWith("--", StringComparison.Ordinal) && eq > 0)
            {
                inline = arg[(eq + 1)..];
                arg = arg[..eq];
            }

            string Value()
            {
                if (inline is not null)
                    return inline;
                if (i + 1 >= args.Length)
                    throw new ToolException($"{arg} needs a value");
                return args[++i];
            }

            if (!arg.StartsWith('-'))
            {
                o.Inputs.Add(args[i]);
                continue;
            }

            switch (arg)
            {
                case "-k" or "--keyset": o.KeysetPath = Value(); break;
                case "--nspdir": o.NspDir = Value(); o.NspDirSet = true; break;
                case "--outdir": o.OutDir = Value(); break;
                case "--ncadir": o.NcaDir = Value(); break;
                case "--tempdir": o.TempDir = Value(); break;
                case "--backupdir": Value(); break; // accepted for compatibility: source files are never modified, so nothing needs a backup
                case "-o" or "--output": o.OutputPath = Value(); break;
                case "--exefsdir": o.ExefsDir = Value(); break;
                case "--romfsdir": o.RomfsDir = Value(); break;
                case "--logodir": o.LogoDir = Value(); break;
                case "--controldir": o.ControlDir = Value(); break;
                case "--htmldocdir": o.HtmlDocDir = Value(); break;
                case "--legalinfodir": o.LegalInfoDir = Value(); break;
                case "--noromfs": o.NoRomfs = true; break;
                case "--nologo": o.NoLogo = true; break;
                case "--plaintext": o.Plaintext = true; break;
                case "--keepncadir": o.KeepNcaDir = true; break;
                case "--nosignncasig2": o.NoSignNcaSig2 = true; break;
                case "--nopatchnacplogo": o.NoPatchNacpLogo = true; break;
                case "--keygeneration": o.KeyGeneration = ParseInt(Value(), 1, 32, arg); o.KeyGenerationSet = true; break;
                case "--keyareakey": o.KeyAreaKey = NcaCrypto.ParseHex(Value(), 16, arg); break;
                case "--sdkversion": o.SdkVersion = ParseSdkVersion(Value(), arg, o); o.SdkVersionSet = true; break;
                case "--titleid": o.TitleId = NcaCrypto.ParseHexU64(Value(), arg); break;
                case "--titlename": o.TitleName = Value(); break;
                case "--titlepublisher": o.TitlePublisher = Value(); break;
                case "--titleversion": o.TitleVersion = (uint)NcaCrypto.ParseHexU64(Value(), arg); break;
                case "--requiredsystemversion": o.RequiredSystemVersion = ParseSystemVersion(Value(), arg); break;
                case "--nocompress": o.Compress = false; break;
                case "--level": o.Level = ParseInt(Value(), Lz4Encoder.MinLevel, Lz4Encoder.MaxLevel, arg); break;
                case "--blocksize": o.BlockSize = ParseBlockSize(Value()); break;
                case "--threads": o.Threads = ParseInt(Value(), 1, 1024, arg); break;
                case "--verify": o.VerifyAfterBuild = true; break;
                case "--mergedlc": o.MergeDlc = true; break;
                case "--minsaving": o.MinSavingPercent = ParsePercent(Value(), arg); break;
                case "--hidesmall": o.HideSmall = true; break;
                case "--noautoupdate": break; // only means something to the home screen and the GUI
                case "--showcompressed": break; // the same
                case "--only": o.OnlyTitles.AddRange(ParseList(Value())); break;
                case "--skip": o.SkipTitles.AddRange(ParseList(Value())); break;
                default: throw new ToolException($"Unknown option {arg} (use --help)");
            }
        }

        if (o.SdkVersion is < 0x000B0000 or > 0x00FFFFFF)
            throw new ToolException($"Invalid SDK version {o.SdkVersion:X8}, the valid range is 000B0000 - 00FFFFFF");
        return o;
    }

    /// <summary>A version like 12.0.0 is easier to choose than its hex form; the old hex form stays accepted.</summary>
    private static uint ParseSdkVersion(string text, string what, Options o)
    {
        if (FirmwareVersions.TryFromChoice(text, out int keyGeneration, out uint sdk))
        {
            if (!o.KeyGenerationSet)
                o.KeyGeneration = keyGeneration;
            return sdk;
        }
        if (TryFirmware(text, out uint major, out uint minor, out uint micro))
        {
            if (!o.KeyGenerationSet)
                o.KeyGeneration = Math.Max(FirmwareVersions.KeyGenerationFor(major, minor, micro), 1);
            return FirmwareVersions.SdkForFirmware(major, minor, micro);
        }
        return (uint)NcaCrypto.ParseHexU64(text, what);
    }

    /// <summary>The content meta keeps the major version in the top six bits.</summary>
    private static uint ParseSystemVersion(string text, string what)
    {
        if (TryFirmware(text, out uint major, out uint minor, out uint micro))
            return (major << 26) | (minor << 20) | (micro << 16);
        return (uint)NcaCrypto.ParseHexU64(text, what);
    }

    private static bool TryFirmware(string text, out uint major, out uint minor, out uint micro)
    {
        major = minor = micro = 0;
        string[] parts = text.Trim().Split('.');
        if (parts.Length is < 2 or > 3)
            return false;
        var numbers = new uint[3];
        for (int i = 0; i < parts.Length; i++)
        {
            if (!uint.TryParse(parts[i], NumberStyles.None, CultureInfo.InvariantCulture, out numbers[i]))
                return false;
        }
        if (numbers[0] is 0 or > 63 || numbers[1] > 63 || numbers[2] > 15)
            return false;
        (major, minor, micro) = (numbers[0], numbers[1], numbers[2]);
        return true;
    }

    private static int ParseInt(string text, int min, int max, string what)
    {
        if (!int.TryParse(text, NumberStyles.Integer, CultureInfo.InvariantCulture, out int value) || value < min || value > max)
            throw new ToolException($"{what} must be a number from {min} to {max}");
        return value;
    }

    private static int ParsePercent(string text, string what)
    {
        text = text.Trim();
        if (text.Equals("off", StringComparison.OrdinalIgnoreCase))
            return 0;
        return ParseInt(text.TrimEnd('%', ' '), 0, 99, what + "（百分比）");
    }

    private static IEnumerable<string> ParseList(string text) =>
        text.Split(',', StringSplitOptions.TrimEntries | StringSplitOptions.RemoveEmptyEntries);

    private static int ParseBlockSize(string text)
    {
        long multiplier = 1;
        if (text.EndsWith("k", StringComparison.OrdinalIgnoreCase))
        {
            multiplier = 1024;
            text = text[..^1];
        }
        long value = text.StartsWith("0x", StringComparison.OrdinalIgnoreCase)
            ? (long)NcaCrypto.ParseHexU64(text, "--blocksize")
            : long.TryParse(text, NumberStyles.Integer, CultureInfo.InvariantCulture, out long parsed) ? parsed : -1;
        value *= multiplier;

        // A compressed entry may never exceed 64 KiB on disk, and stored blocks are the same size as their input.
        if (value is < 0x1000 or > RomFsCompressor.MaxPhysicalBlockSize || (value & (value - 1)) != 0)
            throw new ToolException("--blocksize must be a power of two from 4K to 64K");
        return (int)value;
    }
}
