using Android.App;
using Android.Content;
using Android.Graphics;
using Android.Graphics.Drawables;
using Android.Views;
using Android.Widget;

namespace HacBrewPackLz;

/// <summary>
/// Everything there is to know about something, rising from the bottom of the screen, where the list and the panel only
/// have room for the short of it. It scrolls when there's more than fits, and goes with a tap outside it or Back.
/// </summary>
internal static class Sheet
{
    public static Dialog Show(Activity activity, View content, params (string label, Style style, Action? action)[] buttons)
    {
        var p = Look.P;
        var dialog = new Dialog(activity);
        dialog.RequestWindowFeature((int)WindowFeatures.NoTitle);

        var root = Look.Column(activity);
        root.Background = Look.Corners(p.Surface, 24, 24, 0, 0);
        var handle = new View(activity) { Background = Look.Rounded(p.StrongBorder, 2) };
        root.AddView(handle, new LinearLayout.LayoutParams(Look.Dp(36), Look.Dp(4)) { Gravity = GravityFlags.CenterHorizontal, TopMargin = Look.Dp(10), BottomMargin = Look.Dp(8) });

        // What scrolls gets what the screen has left after the handle, the buttons and the status bar, so the buttons are
        // always there, upright or on its side.
        int reserved = Look.Dp(22) + (buttons.Length > 0 ? Look.Dp(84) : Look.Dp(16)) + Look.Dp(48);
        var scroll = new CappedScroll(activity, reserved);
        scroll.AddView(content);
        root.AddView(scroll, Look.Fill());

        if (buttons.Length > 0)
        {
            var row = Look.Row(activity);
            row.SetPadding(Look.Dp(20), Look.Dp(12), Look.Dp(20), Look.Dp(16));
            for (int i = 0; i < buttons.Length; i++)
            {
                var (label, style, action) = buttons[i];
                var button = Look.Button(activity, label, style, () =>
                {
                    dialog.Dismiss();
                    action?.Invoke();
                });
                row.AddView(button, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WrapContent, 1) { LeftMargin = i > 0 ? Look.Dp(10) : 0 });
            }
            root.AddView(row, Look.Fill());
        }

        // Clear of the navigation bar, where the window reaches under it.
        int bottom = root.PaddingBottom;
        if (OperatingSystem.IsAndroidVersionAtLeast(30))
        {
            root.SetOnApplyWindowInsetsListener(new Insets((view, insets) =>
            {
                var bars = insets.GetInsets(WindowInsets.Type.NavigationBars());
                view.SetPadding(view.PaddingLeft, view.PaddingTop, view.PaddingRight, bottom + bars.Bottom);
                return insets;
            }));
        }

        dialog.SetContentView(root);
        var window = dialog.Window!;
        window.SetBackgroundDrawable(new ColorDrawable(Color.Transparent));
        // It reaches the bottom of the screen, under the navigation bar, with what's on it kept clear of the bar.
        if (OperatingSystem.IsAndroidVersionAtLeast(30))
        {
            window.SetDecorFitsSystemWindows(false);
            var attributes = window.Attributes!;
            attributes.FitInsetsTypes = 0;
            window.Attributes = attributes;
        }
        window.SetGravity(GravityFlags.Bottom | GravityFlags.CenterHorizontal);
        // As wide as the screen on a phone held upright, and a readable width otherwise.
        var metrics = activity.Resources!.DisplayMetrics!;
        int width = Math.Min(metrics.WidthPixels, Look.Dp(600));
        window.SetLayout(width, ViewGroup.LayoutParams.WrapContent);
        window.Attributes!.WindowAnimations = Android.Resource.Style.AnimationInputMethod;
        dialog.SetCanceledOnTouchOutside(true);
        dialog.Show();
        return dialog;
    }

    /// <summary>A heading inside a sheet.</summary>
    public static TextView Heading(Context context, string text) =>
        Look.Text(context, text, 13, Look.P.Accent, bold: true);

    /// <summary>A column with the sheet's own margins, to put its content in.</summary>
    public static LinearLayout Body(Context context)
    {
        var body = Look.Column(context);
        body.SetPadding(Look.Dp(24), Look.Dp(8), Look.Dp(24), Look.Dp(8));
        return body;
    }

    /// <summary>Figures side by side, each with a small label above it, sharing the width.</summary>
    public static View Figures(Context context, params (string label, string value, Color color)[] figures)
    {
        var p = Look.P;
        var row = Look.Row(context, GravityFlags.Top);
        row.Background = Look.Rounded(p.Background, 14);
        row.SetPadding(Look.Dp(16), Look.Dp(12), Look.Dp(16), Look.Dp(12));
        foreach (var (label, value, color) in figures)
        {
            var cell = Look.Column(context);
            cell.AddView(Look.Text(context, label, 12, p.Faint), Look.Wrap());
            cell.AddView(Look.Text(context, value, 17, color, bold: true), Look.Wrap(0, Look.Dp(2)));
            row.AddView(cell, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WrapContent, 1));
        }
        return row;
    }

    /// <summary>A scroller no taller than what's left of the screen, so a long sheet scrolls rather than pushing its buttons off.</summary>
    private sealed class CappedScroll(Context context, int reserved) : ScrollView(context)
    {
        protected override void OnMeasure(int widthMeasureSpec, int heightMeasureSpec)
        {
            int height = Context!.Resources!.DisplayMetrics!.HeightPixels;
            int cap = Math.Min((int)(height * 0.85), height - reserved);
            base.OnMeasure(widthMeasureSpec, MeasureSpec.MakeMeasureSpec(cap, MeasureSpecMode.AtMost));
        }
    }

    private sealed class Insets(Func<View, WindowInsets, WindowInsets> apply) : Java.Lang.Object, View.IOnApplyWindowInsetsListener
    {
        public WindowInsets OnApplyWindowInsets(View view, WindowInsets insets) => apply(view, insets);
    }
}
