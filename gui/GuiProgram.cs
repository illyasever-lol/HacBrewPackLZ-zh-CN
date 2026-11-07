using System.Runtime.InteropServices;

namespace HacBrewPackLz;

internal static class GuiProgram
{
    private const uint InitStandardClasses = 0x4000;

    [STAThread]
    private static int Main(string[] args)
    {
        try
        {
            Win32.SetProcessDpiAwarenessContext(Win32.DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
            Win32.OleInitialize(IntPtr.Zero);
            var controls = new INITCOMMONCONTROLSEX { Size = (uint)Marshal.SizeOf<INITCOMMONCONTROLSEX>(), Classes = InitStandardClasses };
            Win32.InitCommonControlsEx(ref controls);
            var gdiPlus = new GdiplusStartupInput { Version = 1 };
            Win32.GdiplusStartup(out _, ref gdiPlus, IntPtr.Zero);
            Theme.Refresh();

            using var session = new Session();
            Ui.Sink = session;
            var window = new MainWindow(session);
            Updater.Tidy();
            window.Show();
            if (args.Length > 0)
                session.AddInputs(args);

            while (Win32.GetMessageW(out var message, IntPtr.Zero, 0, 0) > 0)
            {
                Win32.TranslateMessage(ref message);
                Win32.DispatchMessageW(ref message);
            }
            return 0;
        }
        catch (Exception e)
        {
            Fatal(e);
            return 1;
        }
    }

    /// <summary>Exceptions can't cross back into Windows, so anything unexpected is shown here rather than vanishing.</summary>
    public static void Fatal(Exception e)
    {
        Win32.MessageBoxW(IntPtr.Zero, $"Something went wrong and the program has to close.\n\n{e.GetType().Name}: {e.Message}\n\n{e.StackTrace}", AppInfo.Name, Win32.MB_OK | Win32.MB_ICONERROR);
        Environment.Exit(1);
    }
}
