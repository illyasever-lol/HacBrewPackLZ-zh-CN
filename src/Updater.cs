using System.Security.Cryptography;
using System.Text;

namespace HacBrewPackLz;

/// <summary>
/// Looks for a newer release and puts it in place of this program. Everything from the release is checked against a
/// signature that only whoever holds the private key can have made, so it doesn't matter what happens to it on the way
/// here. The program is only replaced once all of it has been checked.
/// </summary>
internal static class Updater
{
    private const string ReleasesUrl = "https://gitlab.com/api/v4/projects/ismaellozanoperezserrano%2Fhacbrewpacklz/releases?per_page=1";
    private const string DownloadsUrl = "https://gitlab.com/ismaellozanoperezserrano/hacbrewpacklz/-/releases/";
    private const string ManifestName = "hacbrewpacklz-update.txt";
    private const string SignatureName = "hacbrewpacklz-update.sig";
    private const string TagField = "\"tag_name\":\"";

    /// <summary>The public half of the key releases are signed with. See tools\\sign_release.py.</summary>
    private static readonly byte[] PublicModulus = Convert.FromHexString("ac8a7491402c6f84a0769a0701a365fd7ae0210defa99e45363e5e5d56963fec5ce9ef63ac7f3cf8ecf32423f989dfa5bb2e7c1c404868a5c13f7b8ecb98bbf33bb56ffe6c27e62e4300280f62f0a212321ea9fcd087164cf49c169af1bc7c742915eda9717e12043a6879bedbf092d740d0724994a7f6e98ba01fdf0eab6e483dd9b7ceea243c1a8659ab89c4623756ff2c7fd628aaa8f00839dbdd934f00c55c5163b7eea1383b83bbfdc9ba4706c1ae04e28b1d6e4ee658aba0d64da7187c7c665d3026bc32fd01fdd1746dcb243c4ed37c2010f573428c32539b6ac4e241f0d860b39d3902890392dd24504b75e54bcc40b5d6f08ee76f6320b988d5eedf");

    /// <summary>Somewhere else to get releases from, for testing. What comes from there still has to carry the right signature.</summary>
    private static string? TestSite => Environment.GetEnvironmentVariable("HACBREWPACKLZ_UPDATE_SITE");

    /// <summary>What this program is called among the files of a release.</summary>
    private static string ReleaseFileName()
    {
        if (OperatingSystem.IsLinux())
            return "hacbrewpacklz-linux-x64";
        if (OperatingSystem.IsMacOS())
            return System.Runtime.InteropServices.RuntimeInformation.ProcessArchitecture == System.Runtime.InteropServices.Architecture.Arm64 ? "hacbrewpacklz-macos-arm64" : "hacbrewpacklz-macos-x64";
        return Path.GetFileName(Environment.ProcessPath ?? "").Contains("gui", StringComparison.OrdinalIgnoreCase) ? "hacbrewpacklz-gui.exe" : "hacbrewpacklz.exe";
    }

    /// <summary>Windows won't delete a program that's running, so the old one goes the next time this starts.</summary>
    public static void Tidy()
    {
        if (Environment.ProcessPath is not { } path)
            return;
        TryDelete(path + ".old");
    }

    /// <summary>Whether a newer version got installed, and what came of it, in words.</summary>
    public static (bool updated, string message) Run()
    {
        try
        {
            return Install();
        }
        catch (Exception e) when (e is ToolException or HttpRequestException or TaskCanceledException or IOException or UnauthorizedAccessException or CryptographicException)
        {
            return (false, $"Couldn't update: {e.Message}");
        }
    }

    private static (bool, string) Install()
    {
        string path = Environment.ProcessPath ?? throw new ToolException("当前副本无法确认自身存放位置");
        using var http = new HttpClient { Timeout = TimeSpan.FromMinutes(10) };
        http.DefaultRequestHeaders.UserAgent.ParseAdd(AppInfo.Name);
        byte[] Get(string url) => http.GetByteArrayAsync(url).GetAwaiter().GetResult();

        string listing = Encoding.UTF8.GetString(Get(TestSite is { } site ? $"{site}/releases" : ReleasesUrl));
        int at = listing.IndexOf(TagField, StringComparison.Ordinal);
        if (at < 0)
            throw new ToolException("获取的版本列表格式与预期不符");
        at += TagField.Length;
        int end = listing.IndexOf('"', at);
        if (end < 0)
            throw new ToolException("获取的版本列表格式与预期不符");
        string tag = listing[at..end];
        string version = tag.TrimStart('v');
        if (!IsNewer(version, AppInfo.Version))
            return (false, $"This is the latest version ({AppInfo.Version}).");

        string source = $"{(TestSite is { } other ? $"{other}/" : DownloadsUrl)}{tag}/downloads/";
        byte[] manifest = Get(source + ManifestName);
        byte[] signature = Get(source + SignatureName);
        if (!IsSigned(manifest, signature))
            throw new ToolException("该版本的签名校验未通过，因此未安装");

        string name = ReleaseFileName();
        string? signedVersion = null, expected = null;
        foreach (string line in Encoding.UTF8.GetString(manifest).Split('\n'))
        {
            if (line.StartsWith("version ", StringComparison.Ordinal))
                signedVersion = line[8..];
            else if (line.Length > 66 && line.AsSpan(64, 2).SequenceEqual("  ") && line[66..] == name)
                expected = line[..64];
        }
        if (signedVersion != version)
            throw new ToolException("该版本自报的版本号与列表中的不一致，因此未安装");
        if (expected is null)
            throw new ToolException($"that release has nothing called {name}");

        byte[] program = Get(source + name);
        if (Convert.ToHexString(SHA256.HashData(program)).ToLowerInvariant() != expected)
            throw new ToolException("下载内容与该版本声明的不一致，因此未安装");

        string fresh = path + ".new", old = path + ".old";
        File.WriteAllBytes(fresh, program);
        if (!OperatingSystem.IsWindows())
            File.SetUnixFileMode(fresh, File.GetUnixFileMode(path));
        TryDelete(old);
        File.Move(path, old);
        try
        {
            File.Move(fresh, path);
        }
        catch
        {
            File.Move(old, path);
            throw;
        }
        TryDelete(old);
        return (true, $"Updated to version {version}. It'll be what opens from now on.");
    }

    private static bool IsNewer(string candidate, string current) =>
        Version.TryParse(candidate, out var a) && Version.TryParse(current, out var b) && a > b;

    private static bool IsSigned(byte[] data, byte[] signature)
    {
        using var rsa = RSA.Create(new RSAParameters { Modulus = PublicModulus, Exponent = [1, 0, 1] });
        return rsa.VerifyData(data, signature, HashAlgorithmName.SHA256, RSASignaturePadding.Pss);
    }

    private static void TryDelete(string path)
    {
        try
        {
            File.Delete(path);
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
            // It's still running, or was never there.
        }
    }
}
