using Android.Content;
using Android.Content.PM;
using Android.OS;

namespace HacBrewPackLz;

/// <summary>
/// The one session there is, kept for as long as the app runs rather than for as long as a screen is shown, so turning
/// the phone or leaving the app doesn't lose the list or stop a job.
/// </summary>
internal static class AppState
{
    private static Session? _session;
    private static readonly object Gate = new();

    /// <summary>A newer version that's been downloaded and checked, waiting for the person to install it.</summary>
    public static string? PendingPackage { get; private set; }
    public static string? PendingVersion { get; private set; }

    /// <summary>Raised on whichever thread the download finished on.</summary>
    public static event Action? PackageReady;

    public static Session Session(Context context)
    {
        lock (Gate)
        {
            if (_session is not null)
                return _session;
            var app = context.ApplicationContext!;
            Sender.MachineName = DeviceName(app);
            string cache = app.CacheDir!.AbsolutePath;
            Updater.InstallPackage = (package, version) =>
            {
                string path = System.IO.Path.Combine(cache, "update.apk");
                File.WriteAllBytes(path, package);
                PendingPackage = path;
                PendingVersion = version;
                PackageReady?.Invoke();
            };
            _session = new Session();
            // What the engine says and how far along it is go to the screen, as in the Windows GUI.
            Ui.Sink = _session;
            // Problems the app finds itself go to the developer, when the person agreed to it in the welcome or Options.
            ProblemReports.ReportText = _session.ReportText;
            // On the screen's own thread, and on any other.
            Android.Runtime.AndroidEnvironment.UnhandledExceptionRaiser += (_, e) => ProblemReports.Crashed(e.Exception);
            AppDomain.CurrentDomain.UnhandledException += (_, e) =>
            {
                if (e.ExceptionObject is Exception crash)
                    ProblemReports.Crashed(crash);
            };
            ProblemReports.Opened();
            var screen = new Handler(Looper.MainLooper!);
            ProblemReports.WatchForFreezes(() =>
            {
                screen.Post(ProblemReports.Alive);
                return ProblemReports.AnsweredLately();
            }, _session.Doing);
            return _session;
        }
    }

    /// <summary>What the phone is called in its settings, which is what the Switch lists it as.</summary>
    private static string DeviceName(Context context)
    {
        try
        {
            if (Android.Provider.Settings.Global.GetString(context.ContentResolver, "device_name") is { Length: > 0 } name)
                return name;
        }
        catch (Java.Lang.Exception)
        {
        }
        string model = Build.Model ?? "Android";
        string maker = Build.Manufacturer ?? "";
        return maker.Length > 0 && !model.StartsWith(maker, StringComparison.OrdinalIgnoreCase)
            ? $"{char.ToUpperInvariant(maker[0])}{maker[1..]} {model}"
            : model;
    }

    /// <summary>Hands the downloaded version to Android, which asks the person and installs it over this one.</summary>
    public static void InstallPending(Context context)
    {
        if (PendingPackage is not { } path || !File.Exists(path))
            return;
        var installer = context.PackageManager!.PackageInstaller;
        var parameters = new PackageInstaller.SessionParams(PackageInstallMode.FullInstall);
        parameters.SetAppPackageName(context.PackageName);
        int id = installer.CreateSession(parameters);
        using (var session = installer.OpenSession(id))
        {
            using (var output = session.OpenWrite("package", 0, new FileInfo(path).Length))
            using (var input = File.OpenRead(path))
            {
                input.CopyTo(output!);
                session.Fsync(output!);
            }
            var intent = new Intent(context, typeof(InstallReceiver));
            var flags = OperatingSystem.IsAndroidVersionAtLeast(31) ? PendingIntentFlags.Mutable : 0;
            var pending = Android.App.PendingIntent.GetBroadcast(context, id, intent, flags | PendingIntentFlags.UpdateCurrent)!;
            session.Commit(pending.IntentSender);
        }
    }
}

/// <summary>Hears back from Android while it installs a newer version, which first needs the person's say so.</summary>
[Android.Runtime.Register("hacbrewpacklz.InstallReceiver")]
internal sealed class InstallReceiver : BroadcastReceiver
{
    public override void OnReceive(Context? context, Intent? intent)
    {
        if (context is null || intent is null)
            return;
        var status = (PackageInstallStatus)intent.GetIntExtra(PackageInstaller.ExtraStatus, -999);
        if (status == PackageInstallStatus.PendingUserAction)
        {
            var confirm = OperatingSystem.IsAndroidVersionAtLeast(33)
                ? (Intent?)intent.GetParcelableExtra(Intent.ExtraIntent, Java.Lang.Class.FromType(typeof(Intent)))
                : (Intent?)intent.GetParcelableExtra(Intent.ExtraIntent);
            if (confirm is not null)
            {
                confirm.AddFlags(ActivityFlags.NewTask);
                context.StartActivity(confirm);
            }
        }
        else if (status != PackageInstallStatus.Success)
        {
            string why = intent.GetStringExtra(PackageInstaller.ExtraStatusMessage) ?? status.ToString();
            Android.Widget.Toast.MakeText(context, $"The new version wasn't installed: {why}", Android.Widget.ToastLength.Long)!.Show();
        }
    }
}
