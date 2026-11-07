using System.Runtime.InteropServices;
#if !WINDOWS
using System.ComponentModel;
using System.Diagnostics;
#endif

namespace HacBrewPackLz;

/// <summary>Shows where something was written, in the system's file manager.</summary>
internal static partial class Explorer
{
    private const int ShowNormal = 1;

    /// <summary>Opens the folder a file is in with the file selected where that can be done, or just the folder.</summary>
    public static void Show(string path)
    {
        bool folder = Directory.Exists(path);
        if (OperatingSystem.IsWindows())
        {
            if (folder)
                ShellExecuteW(IntPtr.Zero, "open", path, null, null, ShowNormal);
            else
                ShellExecuteW(IntPtr.Zero, "open", "explorer.exe", $"/select,\"{path}\"", null, ShowNormal);
            return;
        }

#if !WINDOWS
        try
        {
            if (OperatingSystem.IsMacOS())
                Process.Start("open", folder ? [path] : ["-R", path])?.Dispose();
            else
                Process.Start("xdg-open", [folder ? path : Path.GetDirectoryName(path) ?? path])?.Dispose();
        }
        catch (Win32Exception)
        {
            // Nothing there to open it with, as on a Linux without a desktop.
        }
#endif
    }

    /// <summary>Opens a web address in the default browser.</summary>
    public static void OpenUrl(string url)
    {
        if (OperatingSystem.IsWindows())
        {
            ShellExecuteW(IntPtr.Zero, "open", url, null, null, ShowNormal);
            return;
        }
#if !WINDOWS
        try
        {
            Process.Start(new ProcessStartInfo(url) { UseShellExecute = true })?.Dispose();
        }
        catch (Win32Exception)
        {
        }
#endif
    }

    [LibraryImport("shell32.dll", StringMarshalling = StringMarshalling.Utf16)]
    private static partial IntPtr ShellExecuteW(IntPtr owner, string operation, string file, string? parameters, string? directory, int show);
}
