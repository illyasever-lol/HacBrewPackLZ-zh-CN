using System.Diagnostics;

namespace HacBrewPackLz;

/// <summary>
/// Movement on the screen, as on the Switch: short, only ever drawn, so whatever's clicked is done at once, and slowing towards its end. A page
/// fades in, the view shown slides to the next, a game's bar glides to a new figure and a menu comes out of what opened it. Nothing moves when
/// Windows is set not to show animations, as it is by people who find movement on the screen bothersome.
/// </summary>
internal static class Motion
{
    public const double FadeTime = 0.14, RiseTime = 0.16, GlideTime = 0.16;
    private static readonly Stopwatch Clock = Stopwatch.StartNew();
    private static bool? s_on;

    public static double Now => Clock.Elapsed.TotalSeconds;

    /// <summary>Windows' own "Show animations" setting.</summary>
    public static bool On => s_on ??= !Win32.SystemParametersInfoW(Win32.SPI_GETCLIENTAREAANIMATION, 0, out int on, 0) || on != 0;

    /// <summary>Windows' settings changed, and this may be one of them.</summary>
    public static void Refresh() => s_on = null;

    /// <summary>How far along a movement that began at that time has got, slowing towards its end: 1 once it's over, or when there's none.</summary>
    public static double Along(double since, double time)
    {
        if (since < 0 || !On)
            return 1;
        double t = Math.Clamp((Now - since) / time, 0, 1);
        return 1 - (1 - t) * (1 - t) * (1 - t);
    }
}

/// <summary>A figure as it's drawn, which goes to a new one over a short while: where the view shown is filled in, or a game's bar.</summary>
internal sealed class Glide
{
    private double _from, _to, _since = -1;
    private bool _set;

    /// <summary>Where it's drawn now on its way to the figure, which keeps the window drawing until it's there. The first time, it's there.</summary>
    public double Towards(double target, AppWindow window)
    {
        if (!_set || !Motion.On)
        {
            _set = true;
            _from = _to = target;
            _since = -1;
            return target;
        }
        if (target != _to)
        {
            _from = Shown();
            _to = target;
            _since = Motion.Now;
        }
        double shown = Shown();
        if (_since >= 0)
            window.StillMoving();
        return shown;
    }

    private double Shown()
    {
        double along = Motion.Along(_since, Motion.GlideTime);
        if (along >= 1)
        {
            _since = -1;
            return _to;
        }
        return _from + (_to - _from) * along;
    }
}
