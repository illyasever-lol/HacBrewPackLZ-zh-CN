using Android.App;
using Android.Content;
using Android.Content.PM;
using Android.Content.Res;
using Android.OS;
using Android.Views;
using Android.Views.InputMethods;
using Android.Widget;

namespace HacBrewPackLz;

/// <summary>
/// The same options as the Windows GUI, made from the same list, and kept the same way. Opened as the welcome, the first time the app
/// opens or from the options, it's what the app is for and the few options most people want a say in, with Start along the bottom.
/// </summary>
[Activity(Label = "Options", Exported = false,
    ConfigurationChanges = ConfigChanges.Orientation | ConfigChanges.ScreenSize | ConfigChanges.ScreenLayout | ConfigChanges.SmallestScreenSize
        | ConfigChanges.Keyboard | ConfigChanges.KeyboardHidden)]
public sealed class OptionsActivity : Activity
{
    private const int FolderCode = 20;

    /// <summary>The keys are chosen from the main screen's menu and kept inside the app, so there's no path to set here.</summary>
    private static readonly HashSet<string> Left = ["--keyset"];

    private const string WelcomeExtra = "welcome";

    private bool _welcome;
    private OwnBack? _ownBack;
    private LinearLayout? _panel;
    /// <summary>On its side, Start is in the bar at the top, where an app's main action goes, as the height is needed for the rest.</summary>
    private Button? _headerStart;

    /// <summary>The welcome, rather than all the options.</summary>
    public static Intent WelcomeIntent(Context context) => new Intent(context, typeof(OptionsActivity)).PutExtra(WelcomeExtra, true);

    private Session _session = null!;
    private LinearLayout _rows = null!;
    private OptionSpec? _choosing;
    private TextView? _updateOutcome;

    protected override void OnCreate(Bundle? savedInstanceState)
    {
        base.OnCreate(savedInstanceState);
        Look.Refresh(this);
        _session = AppState.Session(this);
        _welcome = Intent?.GetBooleanExtra(WelcomeExtra, false) ?? false;
        // Going back from the welcome counts as having seen it.
        if (_welcome)
        {
            _ownBack = OwnBack.For(this, Start);
            _ownBack?.Set(true);
        }
        var p = Look.P;

        var root = Look.Column(this);
        root.SetBackgroundColor(p.Background);
        var header = Look.Row(this);
        // The same height as the main screen's bar. The welcome only goes with Start, so it has no way back.
        if (_welcome)
        {
            header.SetPadding(Look.Dp(16), 0, Look.Dp(16), 0);
            header.AddView(Look.Text(this, OptionCatalog.Welcome.Title, 18, p.Text, bold: true), Look.Stretch(true));
            _headerStart = Look.Button(this, "Start", Style.Primary, Start);
            header.AddView(_headerStart, new LinearLayout.LayoutParams(Look.Dp(160), Look.Dp(40)));
        }
        else
        {
            header.SetPadding(Look.Dp(4), 0, Look.Dp(16), 0);
            header.AddView(Look.IconButton(this, Resource.Drawable.ic_back, "Back", Finish), Look.Square(48));
            header.AddView(Look.Text(this, "Options", 18, p.Text, bold: true), Look.Wrap(Look.Dp(8)));
        }
        root.AddView(header, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MatchParent, Look.Dp(56)));

        var scroll = new ScrollView(this);
        _rows = Look.Column(this);
        scroll.AddView(_rows);
        root.AddView(scroll, Look.Stretch(false));
        if (_welcome)
        {
            // Start along the bottom, where the main screen has its next step, with the words that go with it.
            _panel = Look.Column(this);
            root.AddView(_panel, Look.Fill());
            FillPanel();
        }
        Look.KeepClearOfBars(root, _panel);
        FitWidth();
        SetContentView(root);
        Build();
        _session.Changed += OnChanged;
    }

    /// <summary>On a wide screen, as with the phone on its side, the settings stay a width that reads well, in the middle.</summary>
    private void FitWidth()
    {
        int width = Resources!.Configuration!.ScreenWidthDp;
        int widest = _welcome && Look.Wide(this) ? 1000 : 680;
        int side = Math.Max(16, (width - widest) / 2);
        // On its side the welcome has to fit the height as it is, with no room to spare under the cards.
        _rows.SetPadding(Look.Dp(side), 0, Look.Dp(side), Look.Dp(_welcome && Look.Wide(this) ? 8 : 24));
        _panel?.SetPadding(Look.Dp(side), Look.Dp(12), Look.Dp(side), Look.Dp(16));
    }

    /// <summary>Start along the bottom with the words that go with it upright, and in the bar at the top on its side.</summary>
    private void FillPanel()
    {
        if (_panel is null)
            return;
        var p = Look.P;
        bool wide = Look.Wide(this);
        _panel.Visibility = wide ? ViewStates.Gone : ViewStates.Visible;
        if (_headerStart is not null)
            _headerStart.Visibility = wide ? ViewStates.Visible : ViewStates.Gone;
        if (_panel.ChildCount == 0)
        {
            _panel.SetBackgroundColor(p.Surface);
            _panel.AddView(Look.Text(this, OptionCatalog.WelcomeLater, 13, p.SubText), Look.Wrap(0, 0, 0, Look.Dp(10)));
            _panel.AddView(Look.Button(this, "Start", Style.Primary, Start), new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MatchParent, Look.Dp(48)));
        }
        FitWidth();
    }

    /// <summary>The welcome has been seen, and the list is next. Going back from it counts too, as that's moving on from it.</summary>
    private void Start()
    {
        _session.Settings.Welcomed = true;
        _session.Settings.Save();
        Finish();
    }

    public override void OnBackPressed()
    {
        if (_welcome)
            Start();
        else
            base.OnBackPressed();
    }

    public override void OnConfigurationChanged(Android.Content.Res.Configuration newConfig)
    {
        base.OnConfigurationChanged(newConfig);
        FillPanel();
        FitWidth();
        // The welcome is laid out differently on its side.
        if (_welcome)
            Build();
        Window?.DecorView?.RequestApplyInsets();
    }

    protected override void OnDestroy()
    {
        _session.Changed -= OnChanged;
        base.OnDestroy();
    }

    /// <summary>Checking for updates says how it went.</summary>
    private void OnChanged() => RunOnUiThread(() =>
    {
        if (_updateOutcome is not null)
        {
            _updateOutcome.Text = _session.Snapshot().UpdateNote ?? "";
            _updateOutcome.Visibility = _updateOutcome.Text.Length > 0 ? ViewStates.Visible : ViewStates.Gone;
        }
    });

    private void Build()
    {
        var p = Look.P;
        _rows.RemoveAllViews();
        _updateOutcome = null;
        bool Shown(OptionSpec o) => !Left.Contains(o.Flag) && OptionCatalog.Shown(o, _session.Settings.Get);

        if (_welcome)
        {
            // What it's for, and what to know before installing what it makes, then the options themselves.
            var about = Look.Column(this);
            about.Background = Look.Rounded(p.Surface, 10);
            about.SetPadding(Look.Dp(16), Look.Dp(14), Look.Dp(16), Look.Dp(14));
            // What it is, quietly; the one thing to take away, standing out with a bar beside it; and what to watch out for.
            about.AddView(Look.Text(this, OptionCatalog.WelcomeIntro, 14, p.SubText), Look.Fill());
            var tip = Look.Row(this);
            var bar = new View(this) { Background = Look.Rounded(p.Accent, 2) };
            tip.AddView(bar, new LinearLayout.LayoutParams(Look.Dp(3), ViewGroup.LayoutParams.MatchParent) { RightMargin = Look.Dp(10) });
            tip.AddView(Look.Text(this, OptionCatalog.WelcomeTip, 15, p.Text, bold: true), Look.Stretch(true));
            about.AddView(tip, Look.Fill(0, Look.Dp(12)));
            about.AddView(Look.Text(this, OptionCatalog.WelcomeWarning, 13, p.Warn), Look.Fill(0, Look.Dp(12)));
            var options = OptionCatalog.Welcome.Options.Where(Shown).ToList();
            if (Look.Wide(this))
            {
                // Side by side, so what it's for and what can be chosen both show, rather than one under the other off the bottom.
                var columns = Look.Row(this);
                columns.SetGravity(GravityFlags.Top);
                columns.AddView(about, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WrapContent, 1) { RightMargin = Look.Dp(16), TopMargin = Look.Dp(8) });
                var right = Look.Column(this);
                // Start is in the bar at the top, and what it says upright beside it is left out: the height is all needed for this.
                AddGroup(right, "", options, welcome: true);
                columns.AddView(right, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WrapContent, 1.2f) { TopMargin = Look.Dp(8) });
                _rows.AddView(columns, Look.Fill());
            }
            else
            {
                _rows.AddView(about, Look.Fill(0, Look.Dp(8)));
                AddGroup(_rows, "To begin with", options, welcome: true);
            }
            return;
        }

        foreach (var group in OptionCatalog.GeneralGroups)
            AddGroup(_rows, group.Title, group.Options.Where(Shown).ToList());
    }

    /// <summary>A heading, if it has one, and a card of the options under it, made as the welcome's rows or as the options' own.</summary>
    private void AddGroup(LinearLayout into, string title, List<OptionSpec> options, bool welcome = false)
    {
        var p = Look.P;
        if (options.Count == 0)
            return;
        if (title.Length > 0)
            into.AddView(Look.Text(this, title, 14, p.Accent, bold: true), Look.Fill(Look.Dp(4), Look.Dp(welcome ? 16 : 20), 0, Look.Dp(8)));
        var card = Look.Column(this);
        card.Background = Look.Rounded(p.Surface, 10);
        for (int i = 0; i < options.Count; i++)
        {
            if (i > 0)
            {
                var line = new View(this);
                line.SetBackgroundColor(p.Border);
                card.AddView(line, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MatchParent, Math.Max(Look.Dp(1), 1)) { LeftMargin = Look.Dp(16), RightMargin = Look.Dp(16) });
            }
            card.AddView(welcome ? WelcomeRow(options[i]) : OptionRow(options[i]), Look.Fill());
        }
        into.AddView(card, Look.Fill());
    }

    /// <summary>
    /// One of the welcome's options, all alike: its name over a line of what it is or where it's set to, and what changes it in a column of
    /// its own at the end, the same width on every row. What each does in full is under Options.
    /// </summary>
    private View WelcomeRow(OptionSpec option)
    {
        var p = Look.P;
        string value = _session.Settings.Get(option.Flag);
        bool folder = option.Kind is OptionKind.Folder or OptionKind.File;
        var row = Look.Row(this);
        row.SetGravity(GravityFlags.CenterVertical);
        bool wide = Look.Wide(this);
        row.SetPadding(Look.Dp(16), Look.Dp(wide ? 7 : 10), Look.Dp(12), Look.Dp(wide ? 7 : 10));
        row.SetMinimumHeight(Look.Dp(wide ? 58 : 64));
        var words = Look.Column(this);
        words.AddView(Look.Text(this, option.Label, 15, p.Text, bold: true), Look.Wrap());
        var second = Look.Text(this, folder ? (value.Length > 0 ? Storage.Friendly(value) : option.Placeholder) : option.Brief ?? option.Help, 13,
            folder && value.Length > 0 ? p.Accent : p.SubText);
        second.SetMaxLines(1);
        second.Ellipsize = Android.Text.TextUtils.TruncateAt.End;
        words.AddView(second, Look.Wrap(0, Look.Dp(2)));
        row.AddView(words, Look.Stretch(true));
        var end = Look.Row(this);
        end.SetGravity(GravityFlags.Right | GravityFlags.CenterVertical);
        row.AddView(end, new LinearLayout.LayoutParams(Look.Dp(88), ViewGroup.LayoutParams.WrapContent) { LeftMargin = Look.Dp(8) });

        void Commit(string chosen)
        {
            if (Set(option, chosen) is { } problem)
                Toast.MakeText(this, problem, ToastLength.Long)!.Show();
        }

        switch (option.Kind)
        {
            case OptionKind.Toggle:
            {
                var toggle = new Switch(this) { Checked = option.ToggleOn(value) != option.Inverted };
                toggle.ThumbTintList = new ColorStateList([[Android.Resource.Attribute.StateChecked], []], [p.Accent.ToArgb(), p.SubText.ToArgb()]);
                toggle.TrackTintList = new ColorStateList([[Android.Resource.Attribute.StateChecked], []], [p.Accent.ToArgb(), p.StrongBorder.ToArgb()]);
                toggle.CheckedChange += (_, e) => Commit(option.ToggleValue(e.IsChecked != option.Inverted));
                end.AddView(toggle, Look.Wrap());
                row.Click += (_, _) => toggle.Toggle();
                break;
            }
            case OptionKind.Choice:
            {
                var spinner = new Spinner(this, SpinnerMode.Dropdown);
                var adapter = new ArrayAdapter<string>(this, Android.Resource.Layout.SimpleSpinnerItem, option.Choices);
                adapter.SetDropDownViewResource(Android.Resource.Layout.SimpleSpinnerDropDownItem);
                spinner.Adapter = adapter;
                string chosenNow = value.Length > 0 ? value : option.Default;
                int at = Array.FindIndex(option.Choices, c => c.Equals(chosenNow, StringComparison.OrdinalIgnoreCase));
                spinner.SetSelection(Math.Max(at, 0), false);
                spinner.ItemSelected += (_, e) =>
                {
                    string chosen = option.Choices[e.Position];
                    Commit(chosen == option.Default ? "" : chosen);
                };
                end.AddView(spinner, Look.Wrap());
                row.Click += (_, _) => spinner.PerformClick();
                break;
            }
            default:
            {
                var chevron = new ImageView(this) { ImageTintList = ColorStateList.ValueOf(p.SubText) };
                chevron.SetImageResource(Resource.Drawable.ic_chevron_right);
                end.AddView(chevron, Look.Square(24));
                row.Click += (_, _) =>
                {
                    _choosing = option;
                    try
                    {
                        StartActivityForResult(Storage.PickFolder(), FolderCode);
                    }
                    catch (ActivityNotFoundException)
                    {
                    }
                };
                break;
            }
        }
        row.Clickable = true;
        row.Background = new Android.Graphics.Drawables.RippleDrawable(ColorStateList.ValueOf(p.Hover), null, Look.Rounded(Android.Graphics.Color.White, 10));
        return row;
    }

    private View OptionRow(OptionSpec option)
    {
        var p = Look.P;
        string value = _session.Settings.Get(option.Flag);
        var row = Look.Column(this);
        row.SetPadding(Look.Dp(16), Look.Dp(12), Look.Dp(16), Look.Dp(12));
        var top = Look.Row(this);
        var words = Look.Column(this);
        words.AddView(Look.Text(this, option.Label, 15, p.Text, bold: true), Look.Wrap());
        words.AddView(Look.Text(this, option.Help, 13, p.SubText), Look.Wrap(0, Look.Dp(2)));
        top.AddView(words, Look.Stretch(true));
        row.AddView(top, Look.Fill());
        var problem = Look.Text(this, "", 13, p.Bad);
        problem.Visibility = ViewStates.Gone;

        void Commit(string chosen)
        {
            string? error = Set(option, chosen);
            problem.Text = error ?? "";
            problem.Visibility = error is null ? ViewStates.Gone : ViewStates.Visible;
        }

        switch (option.Kind)
        {
            case OptionKind.Toggle:
            {
                var toggle = new Switch(this) { Checked = option.ToggleOn(value) != option.Inverted };
                // Off, as in the Windows GUI: a grey knob on an outlined track, which has to stand out from the card.
                toggle.ThumbTintList = new ColorStateList([[Android.Resource.Attribute.StateChecked], []], [p.Accent.ToArgb(), p.SubText.ToArgb()]);
                toggle.TrackTintList = new ColorStateList([[Android.Resource.Attribute.StateChecked], []], [p.Accent.ToArgb(), p.StrongBorder.ToArgb()]);
                toggle.CheckedChange += (_, e) => Commit(option.ToggleValue(e.IsChecked != option.Inverted));
                top.AddView(toggle, Look.Wrap(Look.Dp(12)));
                row.Click += (_, _) => toggle.Toggle();
                break;
            }
            case OptionKind.Slider:
            {
                int current = int.TryParse(value.Length > 0 ? value : option.Default, out int number) ? number : option.Min;
                var shown = Look.Text(this, current.ToString(), 15, p.Text, bold: true);
                top.AddView(shown, Look.Wrap(Look.Dp(12)));
                var slider = new SeekBar(this) { Max = option.Max - option.Min, Progress = current - option.Min };
                slider.ProgressTintList = slider.ThumbTintList = ColorStateList.ValueOf(p.Accent);
                slider.ProgressChanged += (_, e) => shown.Text = (e.Progress + option.Min).ToString();
                slider.StopTrackingTouch += (_, _) =>
                {
                    string chosen = (slider.Progress + option.Min).ToString();
                    Commit(chosen == option.Default ? "" : chosen);
                };
                row.AddView(slider, Look.Fill(0, Look.Dp(8)));
                break;
            }
            case OptionKind.Choice:
            {
                var spinner = new Spinner(this, SpinnerMode.Dropdown);
                var adapter = new ArrayAdapter<string>(this, Android.Resource.Layout.SimpleSpinnerItem, option.Choices);
                adapter.SetDropDownViewResource(Android.Resource.Layout.SimpleSpinnerDropDownItem);
                spinner.Adapter = adapter;
                string chosenNow = value.Length > 0 ? value : option.Default;
                int at = Array.FindIndex(option.Choices, c => c.Equals(chosenNow, StringComparison.OrdinalIgnoreCase));
                spinner.SetSelection(Math.Max(at, 0), false);
                spinner.ItemSelected += (_, e) =>
                {
                    string chosen = option.Choices[e.Position];
                    Commit(chosen == option.Default ? "" : chosen);
                };
                row.AddView(spinner, Look.Wrap(0, Look.Dp(6)));
                break;
            }
            case OptionKind.Action:
            {
                top.AddView(Look.Button(this, option.Verb, Style.Secondary, () =>
                {
                    if (option == OptionCatalog.CheckForUpdates)
                    {
                        _session.CheckForUpdates();
                    }
                    else if (option == OptionCatalog.ShowWelcome)
                    {
                        // Start goes on to the list, as it does the first time.
                        StartActivity(WelcomeIntent(this));
                        Finish();
                    }
                }), Look.Wrap(Look.Dp(12)));
                if (option == OptionCatalog.CheckForUpdates)
                {
                    _updateOutcome = Look.Text(this, _session.Snapshot().UpdateNote ?? "", 13, p.Text);
                    _updateOutcome.Visibility = _updateOutcome.Text.Length > 0 ? ViewStates.Visible : ViewStates.Gone;
                    row.AddView(_updateOutcome, Look.Fill(0, Look.Dp(6)));
                }
                break;
            }
            case OptionKind.Folder or OptionKind.File:
            {
                // The whole row opens the folder picker, with where it is now below, as in Android's own settings.
                var path = Look.Text(this, value.Length > 0 ? Storage.Friendly(value) : option.Placeholder, 14, value.Length > 0 ? p.Accent : p.Faint);
                ((LinearLayout)top.GetChildAt(0)!).AddView(path, Look.Wrap(0, Look.Dp(6)));
                var chevron = new ImageView(this) { ImageTintList = ColorStateList.ValueOf(p.SubText) };
                chevron.SetImageResource(Resource.Drawable.ic_chevron_right);
                top.AddView(chevron, Look.Square(24));
                row.Clickable = true;
                row.Background = new Android.Graphics.Drawables.RippleDrawable(ColorStateList.ValueOf(p.Hover), null, Look.Rounded(Android.Graphics.Color.White, 10));
                row.Click += (_, _) =>
                {
                    _choosing = option;
                    try
                    {
                        StartActivityForResult(Storage.PickFolder(), FolderCode);
                    }
                    catch (ActivityNotFoundException)
                    {
                    }
                };
                if (value.Length > 0)
                    row.AddView(Look.Button(this, "Use the default", Style.Ghost, () => { Commit(""); Build(); }), Look.Wrap(-Look.Dp(18), Look.Dp(2)));
                break;
            }
            default:
            {
                var field = new EditText(this) { Text = value, Hint = option.Placeholder };
                field.SetSingleLine(true);
                field.SetTextColor(p.Text);
                field.SetHintTextColor(p.Faint);
                field.BackgroundTintList = ColorStateList.ValueOf(p.Accent);
                field.InputType = option.Kind == OptionKind.Number ? Android.Text.InputTypes.ClassNumber : Android.Text.InputTypes.ClassText;
                field.ImeOptions = ImeAction.Done;
                field.EditorAction += (_, e) =>
                {
                    e.Handled = false;
                    if (e.ActionId == ImeAction.Done)
                        Commit(field.Text?.Trim() ?? "");
                };
                field.FocusChange += (_, e) =>
                {
                    if (!e.HasFocus)
                        Commit(field.Text?.Trim() ?? "");
                };
                row.AddView(field, Look.Fill(0, Look.Dp(4)));
                break;
            }
        }
        row.AddView(problem, Look.Fill(0, Look.Dp(4)));
        return row;
    }

    /// <summary>Checks the value the way the command line would, keeps it, and tells the list. Returns what's wrong with it.</summary>
    private string? Set(OptionSpec option, string value)
    {
        var settings = _session.Settings;
        string old = settings.Get(option.Flag);
        if (old == value)
            return null;
        settings.Set(option.Flag, value);
        try
        {
            settings.ToOptions(option.Use);
        }
        catch (ToolException e)
        {
            settings.Set(option.Flag, old);
            return e.Message;
        }
        settings.Save();
        _session.OptionChanged(option.Flag);
        if (OptionCatalog.DecidesShown(option))
            _rows.Post(Build);
        return null;
    }

    protected override void OnActivityResult(int requestCode, Result resultCode, Intent? data)
    {
        base.OnActivityResult(requestCode, resultCode, data);
        if (requestCode != FolderCode || resultCode != Result.Ok || _choosing is not { } option)
            return;
        if (Storage.FolderFrom(data) is { } folder)
        {
            if (Set(option, folder) is { } problem)
                Toast.MakeText(this, problem, ToastLength.Long)!.Show();
            Build();
        }
        else
        {
            Toast.MakeText(this, "Couldn't use that folder. Choose one on the phone's storage or memory card.", ToastLength.Long)!.Show();
        }
    }
}
