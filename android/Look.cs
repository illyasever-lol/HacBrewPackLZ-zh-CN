using Android.Animation;
using Android.Content;
using Android.Content.Res;
using Android.Graphics;
using Android.Graphics.Drawables;
using Android.Util;
using Android.App;
using Android.Views;
using Android.Widget;
using Android.Window;

namespace HacBrewPackLz;

internal enum Style
{
    Primary,
    Secondary,
    Ghost,
}

/// <summary>The same colours as the Windows GUI, light or dark as the phone is set.</summary>
internal sealed record Palette(
    Color Background, Color Surface, Color Hover, Color Border, Color StrongBorder,
    Color Text, Color SubText, Color Faint, Color Accent, Color AccentPressed, Color OnAccent,
    Color Good, Color Warn, Color Bad, Color WarnSurface);

/// <summary>
/// Back that a screen has for itself while it's needed: up a folder in Look inside, from what a job came to back to the list, or the welcome
/// counting as seen. Android 13 and newer ask to be told beforehand whether Back is the screen's own, so that otherwise it can show where Back
/// goes as it's swiped, and from Android 16 that's the only way Back is heard at all. Before 13, onBackPressed hears it, and there's no such
/// thing to make, so <see cref="For"/> gives none: what's made of this class is never loaded there.
/// </summary>
internal sealed class OwnBack : Java.Lang.Object, IOnBackInvokedCallback
{
    private readonly Activity _activity;
    private readonly Action _back;
    private bool _on;

    private OwnBack(Activity activity, Action back)
    {
        _activity = activity;
        _back = back;
    }

    public static OwnBack? For(Activity activity, Action back) => OperatingSystem.IsAndroidVersionAtLeast(33) ? new OwnBack(activity, back) : null;

    /// <summary>Whether Back is the screen's own right now, rather than leaving it.</summary>
    public void Set(bool own)
    {
        if (own == _on || !OperatingSystem.IsAndroidVersionAtLeast(33))
            return;
        _on = own;
        if (own)
            _activity.OnBackInvokedDispatcher.RegisterOnBackInvokedCallback(IOnBackInvokedDispatcher.PriorityDefault, this);
        else
            _activity.OnBackInvokedDispatcher.UnregisterOnBackInvokedCallback(this);
    }

    public void OnBackInvoked() => _back();
}

/// <summary>Sizes, colours and the few kinds of control the screens are made of.</summary>
internal static class Look
{
    // No pure white: cards are a soft off-white on a deeper grey, so the screen isn't glaring. Text stays at 4.5:1 or more.
    private static readonly Palette Light = new(
        Hex(0xD8DBDF), Hex(0xECEEF1), Hex(0xDEE1E5), Hex(0xC3C7CD), Hex(0x7A818A),
        Hex(0x15171A), Hex(0x41464D), Hex(0x5A6068), Hex(0x0A6878), Hex(0x07505C), Hex(0xFFFFFF),
        Hex(0x1B6E34), Hex(0x7C5400), Hex(0xB42222), Hex(0xF0DFB8));

    private static readonly Palette Dark = new(
        Hex(0x1C1D20), Hex(0x26282C), Hex(0x2E3035), Hex(0x35383D), Hex(0x50545B),
        Hex(0xECEDEF), Hex(0xA4A8AF), Hex(0x70747B), Hex(0x3FB8C6), Hex(0x31A3B1), Hex(0x0A2226),
        Hex(0x6DCB8C), Hex(0xE3B566), Hex(0xEF7F79), Hex(0x36301F));

    public static Palette P { get; private set; } = Light;

    private static float _density = 1;

    private static Color Hex(int rgb) => new((byte)(rgb >> 16), (byte)(rgb >> 8), (byte)rgb);

    /// <summary>Called as each screen is made, so it follows the phone's light or dark setting.</summary>
    public static void Refresh(Context context)
    {
        var configuration = context.Resources!.Configuration!;
        P = (configuration.UiMode & UiMode.NightMask) == UiMode.NightYes ? Dark : Light;
        _density = context.Resources.DisplayMetrics!.Density;
    }

    public static int Dp(float value) => (int)Math.Round(value * _density);

    /// <summary>What a game saves, on its bar: one green, stronger the more it saves, as on the Switch and in the Windows app. A saving of 10% or
    /// less is the quietest, one of 60% or more the strongest, brighter on dark and deeper on light.</summary>
    public static Color SavingColor(double saved)
    {
        double strength = Math.Clamp((saved - 0.10) / 0.50, 0, 1);
        var (quiet, strong) = P == Dark ? (Hex(0x277448), Hex(0x34C778)) : (Hex(0x84C296), Hex(0x189652));
        byte Mix(byte a, byte b) => (byte)Math.Round(a + (b - a) * strength);
        return new Color(Mix(quiet.R, strong.R), Mix(quiet.G, strong.G), Mix(quiet.B, strong.B));
    }

    public static TextView Text(Context context, string text, float sp, Color color, bool bold = false)
    {
        var view = new TextView(context) { Text = text };
        view.SetTextSize(ComplexUnitType.Sp, sp);
        view.SetTextColor(color);
        if (bold)
            view.SetTypeface(Typeface.Create("sans-serif-medium", TypefaceStyle.Normal), TypefaceStyle.Normal);
        return view;
    }

    public static Drawable Rounded(Color fill, float radiusDp, Color? stroke = null) => Rounded(fill, radiusDp, radiusDp, stroke);

    private static (Color fill, Color text, Color? stroke) ButtonColors(Style style) => style switch
    {
        Style.Primary => (P.Accent, P.OnAccent, null),
        Style.Secondary => (P.Surface, P.Text, P.StrongBorder),
        _ => (Color.Transparent, P.Text, null),
    };

    /// <summary>A button's face in its style, its corners as given, so a button that sits beside another can be rounded on one side only, and
    /// on both again when it's alone.</summary>
    public static Drawable ButtonBackground(Style style, float leftRadius, float rightRadius)
    {
        var (fill, _, stroke) = ButtonColors(style);
        return new RippleDrawable(ColorStateList.ValueOf(style == Style.Primary ? P.AccentPressed : P.Hover),
            Rounded(fill, leftRadius, rightRadius, stroke), Rounded(Color.White, leftRadius, rightRadius));
    }

    /// <summary>A rounded box whose left and right corners can differ, so two buttons can sit together as one.</summary>
    public static Drawable Rounded(Color fill, float leftDp, float rightDp, Color? stroke = null)
    {
        var shape = new GradientDrawable();
        shape.SetColor(fill);
        float l = Dp(leftDp), r = Dp(rightDp);
        shape.SetCornerRadii([l, l, r, r, r, r, l, l]);
        if (stroke is { } line)
            shape.SetStroke(Math.Max(Dp(1), 1), line);
        return shape;
    }

    /// <summary>A box with each corner rounded as given, such as a panel rounded only along its top.</summary>
    public static Drawable Corners(Color fill, float topLeft, float topRight, float bottomRight, float bottomLeft)
    {
        var shape = new GradientDrawable();
        shape.SetColor(fill);
        float a = Dp(topLeft), b = Dp(topRight), c = Dp(bottomRight), d = Dp(bottomLeft);
        shape.SetCornerRadii([a, a, b, b, c, c, d, d]);
        return shape;
    }

    /// <summary>Whether there's room to put the list and what to do with it side by side, as with the phone on its side.</summary>
    public static bool Wide(Context context)
    {
        var configuration = context.Resources!.Configuration!;
        return configuration.ScreenWidthDp >= 600 || configuration.ScreenWidthDp > configuration.ScreenHeightDp && configuration.ScreenWidthDp >= 480;
    }

    /// <summary>A button with words, and an icon before them when that makes it quicker to find.</summary>
    public static Button Button(Context context, string text, Style style, Action click, int icon = 0, float leftRadius = 8, float rightRadius = 8)
    {
        var p = P;
        var button = new Button(context) { Text = text, Gravity = GravityFlags.Center };
        button.SetAllCaps(false);
        button.SetTextSize(ComplexUnitType.Sp, 15);
        button.SetTypeface(Typeface.Create("sans-serif-medium", TypefaceStyle.Normal), TypefaceStyle.Normal);
        button.SetMinWidth(0);
        button.SetMinimumWidth(0);
        button.SetMinHeight(Dp(48));
        button.SetMinimumHeight(Dp(48));
        button.SetPadding(Dp(14), 0, Dp(14), 0);
        // A word broken over two lines reads as two words.
        button.SetSingleLine(true);
        button.Ellipsize = Android.Text.TextUtils.TruncateAt.End;
        button.StateListAnimator = null;
        button.Background = ButtonBackground(style, leftRadius, rightRadius);
        var colors = new ColorStateList([[-Android.Resource.Attribute.StateEnabled], []], [p.Faint.ToArgb(), ButtonColors(style).text.ToArgb()]);
        button.SetTextColor(colors);
        Label(button, text, icon);
        button.Click += (_, _) => click();
        return button;
    }

    /// <summary>
    /// A button's words, with an icon drawn in line before them, so the two stay together in the middle however wide the
    /// button is. 0 is no icon.
    /// </summary>
    public static void Label(Button button, string text, int icon)
    {
        if (icon == 0)
        {
            button.Text = text;
            return;
        }
        var drawable = button.Context!.GetDrawable(icon)!.Mutate()!;
        int size = Dp(20);
        drawable.SetBounds(0, 0, size, size);
        // The words' own colour, as greying out the whole button covers being turned off.
        drawable.SetTint(button.TextColors!.DefaultColor);
        var label = new Android.Text.SpannableString(text.Length > 0 ? "  " + text : " ");
        var align = OperatingSystem.IsAndroidVersionAtLeast(29) ? Android.Text.Style.SpanAlign.Center : Android.Text.Style.SpanAlign.Baseline;
        label.SetSpan(new Android.Text.Style.ImageSpan(drawable, align), 0, 1, Android.Text.SpanTypes.ExclusiveExclusive);
        button.TextFormatted = label;
    }

    /// <summary>A round button with only an icon, for what's always in the same place, like Options at the top.</summary>
    public static ImageButton IconButton(Context context, int icon, string description, Action click)
    {
        var p = P;
        var button = new ImageButton(context) { ContentDescription = description };
        button.SetImageResource(icon);
        button.ImageTintList = ColorStateList.ValueOf(p.Text);
        button.SetScaleType(ImageView.ScaleType.Center);
        var round = new GradientDrawable();
        round.SetShape(ShapeType.Oval);
        round.SetColor(Color.White);
        button.Background = new RippleDrawable(ColorStateList.ValueOf(p.Hover), null, round);
        button.TooltipText = description;
        button.Click += (_, _) => click();
        return button;
    }

    public static LinearLayout.LayoutParams Square(int dp) => new(Dp(dp), Dp(dp));

    /// <summary>Greys a button out as a whole, as a filled one would otherwise look as ready as ever.</summary>
    public static void Enable(View view, bool enabled)
    {
        view.Enabled = enabled;
        view.Alpha = enabled ? 1f : 0.45f;
    }

    public static LinearLayout Row(Context context, GravityFlags gravity = GravityFlags.CenterVertical)
    {
        var row = new LinearLayout(context) { Orientation = Android.Widget.Orientation.Horizontal };
        row.SetGravity(gravity);
        return row;
    }

    public static LinearLayout Column(Context context) => new(context) { Orientation = Android.Widget.Orientation.Vertical };

    public static LinearLayout.LayoutParams Wrap(int left = 0, int top = 0, int right = 0, int bottom = 0)
    {
        var layout = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WrapContent, ViewGroup.LayoutParams.WrapContent);
        layout.SetMargins(left, top, right, bottom);
        return layout;
    }

    public static LinearLayout.LayoutParams Fill(int left = 0, int top = 0, int right = 0, int bottom = 0)
    {
        var layout = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MatchParent, ViewGroup.LayoutParams.WrapContent);
        layout.SetMargins(left, top, right, bottom);
        return layout;
    }

    /// <summary>Blank room along a row, pushing what follows to the end. It has no height of its own, as a plain view would take all there is.</summary>
    public static LinearLayout.LayoutParams Gap() => new(0, 0, 1);

    /// <summary>Takes whatever room is left along a row or column.</summary>
    public static LinearLayout.LayoutParams Stretch(bool across) =>
        across ? new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WrapContent, 1) : new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MatchParent, 0, 1);

    /// <summary>
    /// Since Android 15 apps draw under the status and navigation bars, so what's on the edges is moved in from them.
    /// A panel along the bottom goes on under the navigation bar instead, with only what's on it moved up.
    /// </summary>
    public static void KeepClearOfBars(View root, View? bottom = null)
    {
        // Before Android 11 the window already stops short of the bars.
        if (!OperatingSystem.IsAndroidVersionAtLeast(30))
            return;
        int left = root.PaddingLeft, top = root.PaddingTop, right = root.PaddingRight, under = root.PaddingBottom;
        int panelBottom = bottom?.PaddingBottom ?? 0;
        root.SetOnApplyWindowInsetsListener(new InsetListener((view, insets) =>
        {
            var bars = insets.GetInsets(WindowInsets.Type.SystemBars() | WindowInsets.Type.DisplayCutout());
            var keyboard = insets.GetInsets(WindowInsets.Type.Ime());
            int floor = Math.Max(bars.Bottom, keyboard.Bottom);
            bool panelShown = bottom is { Visibility: ViewStates.Visible };
            view.SetPadding(left + bars.Left, top + bars.Top, right + bars.Right, under + (panelShown ? 0 : floor));
            bottom?.SetPadding(bottom.PaddingLeft, bottom.PaddingTop, bottom.PaddingRight, panelBottom + floor);
            return WindowInsets.Consumed!;
        }));
    }

    private sealed class InsetListener(Func<View, WindowInsets, WindowInsets> apply) : Java.Lang.Object, View.IOnApplyWindowInsetsListener
    {
        public WindowInsets OnApplyWindowInsets(View view, WindowInsets insets) => apply(view, insets);
    }

    /// <summary>A movement from 0 to 1 over as long as the view shown and a game's bar take on Windows and the Switch, slowing towards its end.
    /// Android's own animation setting scales it, and with animations removed there's none: it's at its end at once.</summary>
    public static ValueAnimator Move(Action<float> along)
    {
        var move = ValueAnimator.OfFloat(0f, 1f)!;
        move.SetDuration(160);
        move.SetInterpolator(new Android.Views.Animations.DecelerateInterpolator(1.5f));
        move.Update += (_, e) => along(e.Animation.AnimatedFraction);
        return move;
    }
}

/// <summary>Behind the views, the filled box of the one shown, which slides to the next one shown, as on Windows and the Switch.</summary>
internal sealed class ViewPill : Drawable
{
    private readonly Paint _fill = new(PaintFlags.AntiAlias), _edge = new(PaintFlags.AntiAlias);
    private View? _to;
    private RectF _from = new();
    private float _along = 1;
    private ValueAnimator? _slide;

    public ViewPill()
    {
        _edge.SetStyle(Paint.Style.Stroke);
        _edge.StrokeWidth = Math.Max(Look.Dp(1), 1);
    }

    /// <summary>To the view now shown: from where it was, if it was on the screen, and otherwise there at once.</summary>
    public void MoveTo(View chip)
    {
        if (chip == _to)
            return;
        _slide?.Cancel();
        bool from = _to is { Visibility: ViewStates.Visible, Width: > 0 } && ValueAnimator.AreAnimatorsEnabled();
        if (from)
            _from = Where();
        _to = chip;
        _along = from ? 0 : 1;
        if (from)
        {
            _slide = Look.Move(along =>
            {
                _along = along;
                InvalidateSelf();
            });
            _slide.Start();
        }
        InvalidateSelf();
    }

    /// <summary>Where it's drawn now: on its way, or on the view shown wherever that is, as its count changes its width.</summary>
    private RectF Where()
    {
        var to = new RectF(_to!.Left, _to.Top, _to.Right, _to.Bottom);
        if (_along >= 1)
            return to;
        float Mix(float a, float b) => a + (b - a) * _along;
        return new RectF(Mix(_from.Left, to.Left), Mix(_from.Top, to.Top), Mix(_from.Right, to.Right), Mix(_from.Bottom, to.Bottom));
    }

    public override void Draw(Canvas canvas)
    {
        if (_to is not { Visibility: ViewStates.Visible, Width: > 0 })
            return;
        var box = Where();
        float radius = box.Height() / 2;
        // Greyed out with the views while they can't be used.
        int alpha = (int)(255 * _to.Alpha);
        _fill.Color = Look.P.Hover;
        _fill.Alpha = _fill.Alpha * alpha / 255;
        canvas.DrawRoundRect(box, radius, radius, _fill);
        float half = _edge.StrokeWidth / 2;
        box.Inset(half, half);
        _edge.Color = Look.P.Accent;
        _edge.Alpha = _edge.Alpha * alpha / 255;
        canvas.DrawRoundRect(box, radius - half, radius - half, _edge);
    }

    public override int Opacity => (int)Format.Translucent;
    public override void SetAlpha(int alpha) { }
    public override void SetColorFilter(ColorFilter? colorFilter) { }
}
