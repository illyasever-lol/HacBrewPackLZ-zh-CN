using System.Buffers.Binary;
using System.Globalization;
using System.Security.Cryptography;

namespace HacBrewPackLz;

internal sealed class Keyset
{
    private readonly Dictionary<string, byte[]> _keys = new(StringComparer.OrdinalIgnoreCase);
    private readonly Dictionary<string, byte[]> _titleKeys = new(StringComparer.OrdinalIgnoreCase);

    public byte[] HeaderKey => Require("header_key", 32);

    public byte[]? TryKeyAreaKeyApplication(int generationIndex) =>
        _keys.GetValueOrDefault($"key_area_key_application_{generationIndex:x2}");

    public byte[]? TryKeyAreaKey(int keyAreaIndex, int generationIndex) => keyAreaIndex switch
    {
        0 => _keys.GetValueOrDefault($"key_area_key_application_{generationIndex:x2}"),
        1 => _keys.GetValueOrDefault($"key_area_key_ocean_{generationIndex:x2}"),
        2 => _keys.GetValueOrDefault($"key_area_key_system_{generationIndex:x2}"),
        _ => null,
    };

    public byte[]? TryTitleKek(int generationIndex) => _keys.GetValueOrDefault($"titlekek_{generationIndex:x2}");

    public byte[]? TryTitleKey(string rightsId) => _titleKeys.GetValueOrDefault(rightsId);

    private static readonly string[] DefaultNames = ["keys.dat", "keys.txt", "keys.ini", "prod.keys"];

    /// <summary>Whether a dropped file is meant as a keyset rather than as something to convert.</summary>
    public static bool LooksLikeKeyFile(string path)
    {
        string name = Path.GetFileName(path);
        return !name.Equals("title.keys", StringComparison.OrdinalIgnoreCase)
            && (Path.GetExtension(name).Equals(".keys", StringComparison.OrdinalIgnoreCase) || DefaultNames.Contains(name, StringComparer.OrdinalIgnoreCase));
    }

    /// <summary>The given file, or the first keyset found in the current folder, beside the program, or in ~/.switch.</summary>
    public static string Find(string? path)
    {
        if (path is not null)
        {
            if (!File.Exists(path))
                throw new ToolException($"Keyset file '{path}' not found");
            return path;
        }

        string home = Environment.GetFolderPath(Environment.SpecialFolder.UserProfile);
        var candidates = DefaultNames.Select(name => Path.GetFullPath(name))
            .Concat(DefaultNames.Select(name => Path.Combine(AppContext.BaseDirectory, name)))
            .Append(Path.Combine(home, ".switch", "prod.keys"));
        foreach (var candidate in candidates)
        {
            if (File.Exists(candidate))
                return candidate;
        }
        throw new ToolException("未找到密钥文件。请把 prod.keys 放在本程序旁边，或用 -k 指定路径");
    }

    /// <summary>Also reads title.keys from the same folder when it exists.</summary>
    public static Keyset Load(string path)
    {
        var keyset = new Keyset();
        ReadKeyFile(path, keyset._keys);
        string titleKeys = Path.Combine(Path.GetDirectoryName(Path.GetFullPath(path))!, "title.keys");
        if (File.Exists(titleKeys))
            ReadKeyFile(titleKeys, keyset._titleKeys);
        return keyset;
    }

    private static void ReadKeyFile(string path, Dictionary<string, byte[]> keys)
    {
        foreach (var raw in File.ReadLines(path))
        {
            var line = raw.Trim();
            if (line.Length == 0 || line[0] is ';' or '#')
                continue;
            int eq = line.IndexOf('=');
            if (eq <= 0)
                continue;
            var value = line[(eq + 1)..].Trim();
            if (value.Length % 2 != 0 || !value.All(Uri.IsHexDigit))
                continue;
            keys[line[..eq].Trim()] = Convert.FromHexString(value);
        }
    }

    private byte[] Require(string name, int size)
    {
        if (!_keys.TryGetValue(name, out var key))
            throw new ToolException($"{name} is not present in the keyset file");
        if (key.Length != size)
            throw new ToolException($"{name} in the keyset file must be {size} bytes");
        return key;
    }
}

internal static class NcaCrypto
{
    public static byte[] EcbEncrypt(byte[] key, byte[] data)
    {
        using var aes = Aes.Create();
        aes.Key = key;
        return aes.EncryptEcb(data, PaddingMode.None);
    }

    public static byte[] EcbDecrypt(byte[] key, byte[] data)
    {
        using var aes = Aes.Create();
        aes.Key = key;
        return aes.DecryptEcb(data, PaddingMode.None);
    }

    /// <summary>AES-XTS over 0x200 byte sectors, with the big endian sector number tweak NCA headers use.</summary>
    public static void XtsTransform(byte[] headerKey, byte[] data, int count, bool encrypt)
    {
        using var dataAes = Aes.Create();
        dataAes.Key = headerKey[..16];
        using var tweakAes = Aes.Create();
        tweakAes.Key = headerKey[16..32];

        Span<byte> tweak = stackalloc byte[16];
        var sectorMask = new byte[0x200];
        var cipher = new byte[0x200];

        for (int sector = 0; sector * 0x200 < count; sector++)
        {
            tweak.Clear();
            BinaryPrimitives.WriteUInt64BigEndian(tweak[8..], (ulong)sector);
            tweakAes.EncryptEcb(tweak, tweak, PaddingMode.None);

            for (int block = 0; block < 0x200; block += 16)
            {
                tweak.CopyTo(sectorMask.AsSpan(block));
                int carry = 0;
                for (int i = 0; i < 16; i++)
                {
                    int next = tweak[i] >> 7;
                    tweak[i] = (byte)((tweak[i] << 1) | carry);
                    carry = next;
                }
                if (carry != 0)
                    tweak[0] ^= 0x87;
            }

            var sectorData = data.AsSpan(sector * 0x200, 0x200);
            for (int i = 0; i < 0x200; i++)
                cipher[i] = (byte)(sectorData[i] ^ sectorMask[i]);
            if (encrypt)
                dataAes.EncryptEcb(cipher, cipher, PaddingMode.None);
            else
                dataAes.DecryptEcb(cipher, cipher, PaddingMode.None);
            for (int i = 0; i < 0x200; i++)
                sectorData[i] = (byte)(cipher[i] ^ sectorMask[i]);
        }
    }

    public static byte[] ParseHex(string text, int size, string what)
    {
        if (text.Length != size * 2 || !text.All(Uri.IsHexDigit))
            throw new ToolException($"{what} must be {size} bytes of hex");
        return Convert.FromHexString(text);
    }

    public static ulong ParseHexU64(string text, string what)
    {
        var digits = text.StartsWith("0x", StringComparison.OrdinalIgnoreCase) ? text[2..] : text;
        if (!ulong.TryParse(digits, NumberStyles.HexNumber, CultureInfo.InvariantCulture, out var value))
            throw new ToolException($"{what} must be a hex number");
        return value;
    }
}

/// <summary>AES-CTR as NCA sections use it: the counter is the upper IV followed by the big endian file offset / 16.</summary>
internal sealed class AesCtrTransform : IDisposable
{
    private const int ChunkSize = 1 << 20;

    private readonly Aes _aes;
    private readonly ulong _upperIv;
    private byte[] _counters = [];

    public AesCtrTransform(byte[] key, ulong upperIv)
    {
        _aes = Aes.Create();
        _aes.Key = key;
        _upperIv = upperIv;
    }

    public void Transform(byte[] buffer, int offset, int count, long fileOffset) =>
        Transform(buffer, offset, count, fileOffset, _upperIv);

    public void Transform(byte[] buffer, int offset, int count, long fileOffset, ulong upperIv)
    {
        if (fileOffset % 16 != 0 || count % 16 != 0)
            throw new ArgumentException("AES-CTR 区间必须按 16 字节对齐");

        ulong block = (ulong)fileOffset >> 4;
        while (count > 0)
        {
            int n = Math.Min(count, ChunkSize);
            if (_counters.Length < n)
                _counters = new byte[n];
            var counters = _counters.AsSpan(0, n);
            for (int i = 0; i < n; i += 16)
            {
                BinaryPrimitives.WriteUInt64BigEndian(counters[i..], upperIv);
                BinaryPrimitives.WriteUInt64BigEndian(counters[(i + 8)..], block++);
            }
            _aes.EncryptEcb(counters, counters, PaddingMode.None);

            var data = buffer.AsSpan(offset, n);
            var dataWords = System.Runtime.InteropServices.MemoryMarshal.Cast<byte, ulong>(data);
            var maskWords = System.Runtime.InteropServices.MemoryMarshal.Cast<byte, ulong>(counters);
            for (int i = 0; i < dataWords.Length; i++)
                dataWords[i] ^= maskWords[i];

            offset += n;
            count -= n;
        }
    }

    public void Dispose() => _aes.Dispose();
}
