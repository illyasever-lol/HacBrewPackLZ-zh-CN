using Android.App;
using Android.Content;
using Android.Content.PM;
using Android.Net.Wifi;
using Android.OS;

namespace HacBrewPackLz;

/// <summary>
/// Keeps a job going with the screen off or the app in the background, which Android otherwise stops, and shows how
/// it's going in a notification. It runs only while there's a job, and goes when it's done.
/// </summary>
[Android.Runtime.Register("hacbrewpacklz.WorkService")]
internal sealed class WorkService : Service
{
    private const int NotificationId = 1;
    private const string Channel = "work";
    private const string StopAction = "stop";

    private Session? _session;
    private PowerManager.WakeLock? _awake;
    private WifiManager.WifiLock? _wifi;
    private Handler? _main;
    private int _pending;
    private long _lastShown;

    /// <summary>Called by the screen when a job starts. Starting it again while it runs does nothing more.</summary>
    public static void Start(Context context)
    {
        var intent = new Intent(context, typeof(WorkService));
        try
        {
            context.StartForegroundService(intent);
        }
        catch (Java.Lang.Exception)
        {
            // Android can refuse while the app is in the background; the job still runs while it's open.
        }
    }

    public override IBinder? OnBind(Intent? intent) => null;

    public override StartCommandResult OnStartCommand(Intent? intent, StartCommandFlags flags, int startId)
    {
        _session ??= AppState.Session(this);
        if (intent?.Action == StopAction)
        {
            _session.Stop();
            return StartCommandResult.NotSticky;
        }

        var manager = (NotificationManager)GetSystemService(NotificationService)!;
        manager.CreateNotificationChannel(new NotificationChannel(Channel, "Jobs", NotificationImportance.Low)
        {
            Description = "How compressing or sending to the Switch is going",
        });
        var notification = Build(_session.Snapshot());
        if (OperatingSystem.IsAndroidVersionAtLeast(34))
            StartForeground(NotificationId, notification, ForegroundService.TypeSpecialUse);
        else
            StartForeground(NotificationId, notification);

        if (_awake is null)
        {
            _main = new Handler(Looper.MainLooper!);
            var power = (PowerManager)GetSystemService(PowerService)!;
            _awake = power.NewWakeLock(WakeLockFlags.Partial, "hacbrewpacklz:job");
            _awake!.Acquire();
            var wifi = (WifiManager)ApplicationContext!.GetSystemService(WifiService)!;
#pragma warning disable CA1422 // Still what keeps Wi-Fi at full speed with the screen off.
            _wifi = wifi.CreateWifiLock(OperatingSystem.IsAndroidVersionAtLeast(29) ? Android.Net.WifiMode.FullLowLatency : Android.Net.WifiMode.FullHighPerf, "hacbrewpacklz:send");
#pragma warning restore CA1422
            _wifi!.Acquire();
            _session.Changed += OnChanged;
        }
        Update();
        return StartCommandResult.NotSticky;
    }

    private void OnChanged()
    {
        if (Interlocked.Exchange(ref _pending, 1) == 0)
            _main?.Post(Update);
    }

    private void Update()
    {
        Interlocked.Exchange(ref _pending, 0);
        if (_session is null)
            return;
        var state = _session.Snapshot();
        if (!state.IsBusy)
        {
            StopForeground(StopForegroundFlags.Remove);
            StopSelf();
            return;
        }
        // Notifications that change too often are dropped by Android.
        long now = SystemClock.ElapsedRealtime();
        if (now - _lastShown < 1000)
        {
            _main?.PostDelayed(() => { if (Interlocked.Exchange(ref _pending, 1) == 0) Update(); }, 1000 - (now - _lastShown));
            return;
        }
        _lastShown = now;
        ((NotificationManager)GetSystemService(NotificationService)!).Notify(NotificationId, Build(state));
    }

    private Notification Build(SessionSnapshot state)
    {
        var (first, second, fraction) = Status.Of(state, _session!.Settings);
        var open = PendingIntent.GetActivity(this, 0, new Intent(this, typeof(MainActivity)).AddFlags(ActivityFlags.SingleTop), PendingIntentFlags.Immutable);
        var stop = PendingIntent.GetService(this, 1, new Intent(this, typeof(WorkService)).SetAction(StopAction), PendingIntentFlags.Immutable);
        var builder = new Notification.Builder(this, Channel)
            .SetSmallIcon(Resource.Drawable.icon_foreground)
            .SetContentTitle(first.Length > 0 ? first : AppInfo.Name)
            .SetContentText(second)
            .SetStyle(new Notification.BigTextStyle().BigText(second))
            .SetOngoing(true)
            .SetOnlyAlertOnce(true)
            .SetContentIntent(open)
            .AddAction(new Notification.Action.Builder(null, "Stop", stop).Build());
        builder.SetProgress(1000, (int)((fraction ?? 0) * 1000), fraction is null);
        if (OperatingSystem.IsAndroidVersionAtLeast(31))
            builder.SetForegroundServiceBehavior((int)NotificationForegroundService.Immediate);
        return builder.Build();
    }

    public override void OnDestroy()
    {
        if (_session is not null)
            _session.Changed -= OnChanged;
        if (_awake?.IsHeld == true)
            _awake.Release();
        if (_wifi?.IsHeld == true)
            _wifi.Release();
        _awake = null;
        _wifi = null;
        base.OnDestroy();
    }
}
