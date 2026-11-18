using Android;
using Android.App;
using Android.Content;
using Android.Content.PM;
using Android.Content.Res;
using Android.Graphics;
using Android.Graphics.Drawables;
using Android.OS;
using Android.Text;
using Android.Text.Style;
using Android.Util;
using Android.Views;
using Android.Widget;

namespace HacBrewPackLz;

/// <summary>
/// The screen people see: the games, and a panel with how it's going and what to do next. Upright, the panel is along
/// the bottom; on its side, the phone has the room to put it beside the list. Everything else is in the top bar.
/// </summary>
[Activity(Label = "hacBrewPackLZ", MainLauncher = true, Exported = true, LaunchMode = LaunchMode.SingleTop,
    ConfigurationChanges = ConfigChanges.Orientation | ConfigChanges.ScreenSize | ConfigChanges.ScreenLayout | ConfigChanges.SmallestScreenSize
        | ConfigChanges.Keyboard | ConfigChanges.KeyboardHidden | ConfigChanges.Navigation)]
public sealed class MainActivity : Activity
{
    private const int AddFilesCode = 10;
    private const int AddFolderCode = 11;
    private const int KeysCode = 12;

    private Session _session = null!;
    private SessionSnapshot _state = null!;

    /// <summary>Compressing rather than repacking, as Settings has it.</summary>
    internal bool Compressing => _session.Settings.Get("--nocompress") != "1";
    private int _pending;
    private bool _wasBusy;
    private bool _sendNext;
    private bool _wide;
    private string _banners = "?";

    private LinearLayout _bannerHost = null!;
    private View _listPart = null!;
    /// <summary>To do, Done and Everything, above the list, as on the Switch and in the Windows GUI.</summary>
    private readonly TextView[] _viewChips = new TextView[3];
    private static readonly GamesShown[] Views = [GamesShown.ToDo, GamesShown.Done, GamesShown.Everything];
    private View _chipsRow = null!;
    private readonly ViewPill _viewPill = new();
    /// <summary>On a job's own view, what it's doing to how many games, in place of the views.</summary>
    private TextView _jobHeadline = null!;
    /// <summary>What an empty view says, in place of the list.</summary>
    private TextView _viewEmpty = null!;
    private View _emptyPart = null!;
    private View _panel = null!;
    private TextView _emptyTitle = null!;
    private TextView _emptyText = null!;
    private ProgressBar _emptySpinner = null!;
    private ImageView _emptyIcon = null!;
    private LinearLayout _emptyButtons = null!;
    private TextView _subtitle = null!;
    private ImageButton _tickAll = null!;
    private ListView _list = null!;
    private TitleAdapter _adapter = null!;
    private ImageButton _add = null!, _options = null!, _more = null!, _details = null!;
    private Button _analyze = null!, _compress = null!, _compressMenu = null!, _stop = null!;
    private bool _compressAlone;
    /// <summary>From what a job came to, back to the list: beside Send to Switch, or the main button when there's nothing to send.</summary>
    private Button _back = null!, _backAlone = null!;
    private ProgressBar _progress = null!;
    private TextView _statusFirst = null!, _statusSecond = null!;
    private TextView? _logText;
    private ScrollView? _logScroll;
    /// <summary>The status sheet's own words, kept up to date while it's open.</summary>
    private TextView? _sheetFirst, _sheetSecond;
    private Dialog? _statusSheet;
    private int _logShown = -1;
    /// <summary>From what a job came to, Back goes back to the list.</summary>
    private OwnBack? _ownBack;
    /// <summary>A newer version was asked for from the menu, so how that went is said once it's known.</summary>
    private bool _sayVersion;

    private const string EmptyText = "Choose NSP, NSZ, XCI or XCZ files, or a folder to look through for them. Your files are never changed.";

    protected override void OnCreate(Bundle? savedInstanceState)
    {
        base.OnCreate(savedInstanceState);
        Look.Refresh(this);
        _session = AppState.Session(this);
        _state = _session.Snapshot();
        _wide = Look.Wide(this);
        _ownBack = OwnBack.For(this, () => _session.BackToList());
        SetContentView(BuildScreen());

        // The first time, the welcome comes first, and asking about notifications waits for it.
        if (!_session.Settings.Welcomed && savedInstanceState is null)
            StartActivity(OptionsActivity.WelcomeIntent(this));
        // Whoever had the welcome before sending problems was on it is asked the once.
        else if (_session.Settings.AskAboutProblems && savedInstanceState is null)
            new AlertDialog.Builder(this)
                .SetTitle(OptionCatalog.ProblemsQuestion)!
                .SetMessage(OptionCatalog.ProblemsExplained)!
                .SetCancelable(false)!
                .SetPositiveButton("Send them", (_, _) => _session.Settings.AnswerAboutProblems(true))!
                .SetNegativeButton("No thanks", (_, _) => _session.Settings.AnswerAboutProblems(false))!
                .Show();

        _session.Changed += OnChanged;
        AppState.PackageReady += OnChanged;
        Refresh();
    }

    /// <summary>Turning the phone lays the screen out again, beside or below the list, keeping the list where it was.</summary>
    public override void OnConfigurationChanged(Configuration newConfig)
    {
        base.OnConfigurationChanged(newConfig);
        bool wide = Look.Wide(this);
        if (wide == _wide)
            return;
        _wide = wide;
        int first = _list.FirstVisiblePosition;
        SetContentView(BuildScreen());
        _banners = "?";
        Refresh();
        _list.SetSelection(first);
    }

    /// <summary>From what a job came to, Back goes back to the list, as its button does, rather than out of the app.</summary>
    public override void OnBackPressed()
    {
        if (_state is { Job.Running: false })
            _session.BackToList();
        else
            base.OnBackPressed();
    }

    protected override void OnDestroy()
    {
        _session.Changed -= OnChanged;
        AppState.PackageReady -= OnChanged;
        base.OnDestroy();
    }

    protected override void OnResume()
    {
        base.OnResume();
        AskForNotificationsOnce();
        // Access to files may have just been given in the system settings.
        _banners = "?";
        Refresh();
    }

    private bool _askedForNotifications;

    /// <summary>What a long job is doing shows as a notification, which Android 13 and newer ask about, once the welcome is out of the way.</summary>
    private void AskForNotificationsOnce()
    {
        if (_askedForNotifications || !_session.Settings.Welcomed)
            return;
        _askedForNotifications = true;
        if (OperatingSystem.IsAndroidVersionAtLeast(33) && CheckSelfPermission(Manifest.Permission.PostNotifications) != Permission.Granted)
            RequestPermissions([Manifest.Permission.PostNotifications], 2);
    }

    private void OnChanged()
    {
        if (Interlocked.Exchange(ref _pending, 1) == 0)
            RunOnUiThread(Refresh);
    }

    private View BuildScreen()
    {
        var p = Look.P;
        var root = Look.Column(this);
        root.SetBackgroundColor(p.Background);

        // The top bar, no taller than Android's own: the name with how many games there are under it, and what's always
        // there. The version is in About.
        var bar = Look.Row(this);
        bar.SetPadding(Look.Dp(20), 0, Look.Dp(4), 0);
        var names = Look.Column(this);
        names.AddView(Look.Text(this, AppInfo.Name, 18, p.Text, bold: true), Look.Wrap());
        _subtitle = Look.Text(this, "", 12, p.SubText);
        _subtitle.SetSingleLine(true);
        names.AddView(_subtitle, Look.Wrap());
        bar.AddView(names, Look.Stretch(true));
        _tickAll = Look.IconButton(this, Resource.Drawable.ic_tick_some, "Tick all", () =>
        {
            if (!_state.IsBusy)
                _session.SelectAll(!_state.Rows.All(r => r.Selected));
        });
        bar.AddView(_tickAll, Look.Square(48));
        _add = Look.IconButton(this, Resource.Drawable.ic_add, "Add games", OpenAdd);
        _options = Look.IconButton(this, Resource.Drawable.ic_options, "Options", () => StartActivity(new Intent(this, typeof(OptionsActivity))));
        _more = Look.IconButton(this, Resource.Drawable.ic_more, "More", OpenMore);
        bar.AddView(_add, Look.Square(48));
        bar.AddView(_options, Look.Square(48));
        bar.AddView(_more, Look.Square(48));
        root.AddView(bar, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MatchParent, Look.Dp(56)));

        _bannerHost = Look.Column(this);
        _bannerHost.SetPadding(Look.Dp(16), 0, Look.Dp(16), 0);
        root.AddView(_bannerHost, Look.Fill());

        // The games, or what to do first while there are none.
        var content = new FrameLayout(this);
        content.AddView(BuildList(), new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MatchParent, ViewGroup.LayoutParams.MatchParent));
        content.AddView(BuildEmpty(), new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MatchParent, ViewGroup.LayoutParams.MatchParent));

        _panel = BuildPanel();
        if (_wide)
        {
            var side = Look.Row(this, GravityFlags.Top);
            side.AddView(content, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.MatchParent, 1));
            _panel.Background = Look.Rounded(p.Surface, 20);
            side.AddView(_panel, new LinearLayout.LayoutParams(Look.Dp(380), ViewGroup.LayoutParams.MatchParent)
            {
                LeftMargin = Look.Dp(4), RightMargin = Look.Dp(16), BottomMargin = Look.Dp(16),
            });
            root.AddView(side, Look.Stretch(false));
        }
        else
        {
            root.AddView(content, Look.Stretch(false));
            _panel.Background = Look.Corners(p.Surface, 24, 24, 0, 0);
            root.AddView(_panel, Look.Fill());
        }
        // Upright, the panel runs on under the navigation bar.
        Look.KeepClearOfBars(root, _wide ? null : _panel);
        return root;
    }

    private View BuildList()
    {
        var p = Look.P;
        var part = Look.Column(this);

        _adapter = new TitleAdapter(this);
        _list = new ListView(this) { Adapter = _adapter, Divider = new ColorDrawable(Color.Transparent), DividerHeight = Look.Dp(8) };
        _list.SetSelector(Android.Resource.Color.Transparent);
        _list.SetPadding(Look.Dp(12), Look.Dp(4), Look.Dp(12), Look.Dp(12));
        _list.SetClipToPadding(false);
        _list.ScrollBarStyle = ScrollbarStyles.OutsideOverlay;
        _list.ItemClick += (_, e) => ItemTapped(e.Position);
        _list.ItemLongClick += (_, e) => e.Handled = LongPressed(e.Position, e.View);

        var chips = Look.Row(this);
        chips.SetPadding(Look.Dp(16), Look.Dp(4), Look.Dp(16), Look.Dp(8));
        for (int i = 0; i < Views.Length; i++)
        {
            var view = Views[i];
            var chip = Look.Text(this, "", 14, p.SubText);
            chip.SetPadding(Look.Dp(16), Look.Dp(8), Look.Dp(16), Look.Dp(8));
            chip.Clickable = true;
            chip.Click += (_, _) => _session.SetView(view);
            chips.AddView(chip, Look.Wrap(0, 0, Look.Dp(8)));
            _viewChips[i] = chip;
        }
        // The one shown is filled in behind it, and slides to the next one shown.
        chips.Background = _viewPill;
        chips.LayoutChange += (_, _) => _viewPill.InvalidateSelf();
        part.AddView(chips, Look.Fill());
        _chipsRow = chips;
        _jobHeadline = Look.Text(this, "", 15, p.Text);
        _jobHeadline.SetPadding(Look.Dp(20), Look.Dp(8), Look.Dp(16), Look.Dp(10));
        _jobHeadline.Visibility = ViewStates.Gone;
        part.AddView(_jobHeadline, Look.Fill());
        _viewEmpty = Look.Text(this, "", 15, p.SubText);
        _viewEmpty.Gravity = GravityFlags.Center;
        _viewEmpty.SetPadding(Look.Dp(32), Look.Dp(48), Look.Dp(32), Look.Dp(48));
        part.AddView(_viewEmpty, Look.Fill());
        part.AddView(_list, Look.Stretch(false));
        _listPart = part;
        return part;
    }

    private View BuildEmpty()
    {
        var p = Look.P;
        // In a scroller, so a phone on its side still gets to the buttons.
        var scroll = new ScrollView(this) { FillViewport = true };
        var empty = Look.Column(this);
        empty.SetGravity(GravityFlags.Center);
        empty.SetPadding(Look.Dp(32), Look.Dp(_wide ? 0 : 16), Look.Dp(32), Look.Dp(_wide ? 16 : 32));
        _emptyIcon = new ImageView(this);
        _emptyIcon.SetImageResource(Resource.Mipmap.icon);
        int iconSize = Look.Dp(_wide ? 56 : 96);
        empty.AddView(_emptyIcon, new LinearLayout.LayoutParams(iconSize, iconSize) { BottomMargin = Look.Dp(_wide ? 10 : 16) });
        _emptySpinner = new ProgressBar(this) { Indeterminate = true, IndeterminateTintList = ColorStateList.ValueOf(p.Accent) };
        empty.AddView(_emptySpinner, Look.Wrap(0, 0, 0, Look.Dp(16)));
        _emptyTitle = Look.Text(this, "Add your games", 22, p.Text, bold: true);
        _emptyTitle.Gravity = GravityFlags.Center;
        empty.AddView(_emptyTitle, Look.Wrap());
        _emptyText = Look.Text(this, EmptyText, 15, p.SubText);
        _emptyText.Gravity = GravityFlags.Center;
        _emptyText.SetMaxWidth(Look.Dp(420));
        empty.AddView(_emptyText, Look.Wrap(0, Look.Dp(8), 0, Look.Dp(_wide ? 16 : 24)));
        // Stacked upright; side by side on its side, where there's width to spare and little height.
        _emptyButtons = _wide ? Look.Row(this, GravityFlags.Center) : Look.Column(this);
        _emptyButtons.AddView(Look.Button(this, "Add files", Style.Primary, AddFiles, Resource.Drawable.ic_add),
            new LinearLayout.LayoutParams(Look.Dp(_wide ? 200 : 240), ViewGroup.LayoutParams.WrapContent));
        var folder = new LinearLayout.LayoutParams(Look.Dp(_wide ? 200 : 240), ViewGroup.LayoutParams.WrapContent);
        if (_wide)
            folder.LeftMargin = Look.Dp(12);
        else
            folder.TopMargin = Look.Dp(10);
        _emptyButtons.AddView(Look.Button(this, "Add folder", Style.Secondary, AddFolder, Resource.Drawable.ic_folder), folder);
        empty.AddView(_emptyButtons, Look.Wrap());
        scroll.AddView(empty);
        _emptyPart = scroll;
        return scroll;
    }

    /// <summary>How it's going and what to do next: below the list upright, beside it on its side.</summary>
    private View BuildPanel()
    {
        var p = Look.P;
        var panel = Look.Column(this);
        panel.SetPadding(Look.Dp(20), Look.Dp(12), Look.Dp(20), Look.Dp(16));

        // The short of what's going on, with all of it a tap away: the arrow says there's more.
        var top = Look.Row(this, GravityFlags.Top);
        top.Clickable = true;
        top.Background = new RippleDrawable(ColorStateList.ValueOf(p.Hover), null, Look.Rounded(Color.White, 12));
        top.SetPadding(Look.Dp(8), Look.Dp(6), 0, Look.Dp(6));
        top.Click += (_, _) => ShowStatus();
        var words = Look.Column(this);
        _statusFirst = Look.Text(this, "", 16, p.Text, bold: true);
        _statusFirst.SetMaxLines(_wide ? 3 : 1);
        _statusFirst.Ellipsize = TextUtils.TruncateAt.End;
        words.AddView(_statusFirst, Look.Fill());
        _statusSecond = Look.Text(this, "", 14, p.SubText);
        _statusSecond.SetMaxLines(_wide ? 8 : 2);
        _statusSecond.Ellipsize = TextUtils.TruncateAt.End;
        words.AddView(_statusSecond, Look.Fill(0, Look.Dp(2)));
        top.AddView(words, Look.Stretch(true));
        _details = Look.IconButton(this, Resource.Drawable.ic_expand, "Show all of it", ShowStatus);
        _details.ImageTintList = ColorStateList.ValueOf(p.SubText);
        top.AddView(_details, new LinearLayout.LayoutParams(Look.Dp(40), Look.Dp(40)) { LeftMargin = Look.Dp(4), TopMargin = -Look.Dp(6) });
        panel.AddView(top, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MatchParent, ViewGroup.LayoutParams.WrapContent) { LeftMargin = -Look.Dp(8), RightMargin = -Look.Dp(8) });

        if (_wide)
            panel.AddView(new View(this), Look.Stretch(false));

        _progress = new ProgressBar(this, null, Android.Resource.Attribute.ProgressBarStyleHorizontal)
        {
            Max = 1000,
            ProgressTintList = ColorStateList.ValueOf(p.Accent),
            IndeterminateTintList = ColorStateList.ValueOf(p.Accent),
            ProgressBackgroundTintList = ColorStateList.ValueOf(p.Border),
        };
        panel.AddView(_progress, Look.Fill(0, Look.Dp(8)));

        // The buttons share the width, so the panel reads as one piece whatever's on it.
        var actions = Look.Row(this);
        _analyze = Look.Button(this, "Analyze", Style.Secondary, () => _session.Analyze());
        _compress = Look.Button(this, "Compress", Style.Primary, PressCompress, 0, 10, 0);
        _compressMenu = Look.Button(this, "", Style.Primary, OpenCompressMenu, Resource.Drawable.ic_chevron_down, 0, 10);
        _compressMenu.ContentDescription = "More things to do with the list";
        _compressMenu.SetPadding(0, 0, 0, 0);
        _stop = Look.Button(this, "Stop", Style.Secondary, () => _session.Stop());
        _back = Look.Button(this, "Back to the list", Style.Secondary, () => _session.BackToList());
        _backAlone = Look.Button(this, "Back to the list", Style.Primary, () => _session.BackToList());
        actions.AddView(_back, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WrapContent, 1) { RightMargin = Look.Dp(10) });
        actions.AddView(_backAlone, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WrapContent, 1));
        actions.AddView(_analyze, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WrapContent, 1) { RightMargin = Look.Dp(10) });
        actions.AddView(_compress, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WrapContent, 1.4f) { RightMargin = Math.Max(Look.Dp(1), 1) });
        actions.AddView(_compressMenu, new LinearLayout.LayoutParams(Look.Dp(48), ViewGroup.LayoutParams.WrapContent));
        actions.AddView(_stop, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WrapContent, 1));
        panel.AddView(actions, Look.Fill(0, Look.Dp(10)));
        return panel;
    }

    private void Refresh()
    {
        Interlocked.Exchange(ref _pending, 0);
        var s = _session.Snapshot();

        // Another view, or a job's own view starting or ending, is another list, which starts at its top: where the last one was scrolled to
        // says nothing about where anything in this one is.
        bool otherList = _state is { } before && (before.View != s.View || (before.Job is null) != (s.Job is null));
        _state = s;
        bool busy = s.IsBusy;
        // Measuring goes on behind the list, which can be compressed or sent meanwhile, and its views and menus still open.
        bool blocked = busy && s.Activity != SessionActivity.Measuring;
        bool repack = _session.Settings.Get("--nocompress") == "1";
        int ticked = s.Rows.Count(r => r.Selected);

        _adapter.SetData(s);
        if (otherList)
            _list.SetSelection(0);
        // Nothing at all asks for games. A view with nothing in it while another has some says where they are instead.
        bool empty = _adapter.Count == 0 && s.EverythingCount == 0;
        var p = Look.P;
        // With compression off nothing is done, so there's no Done, as on the Switch.
        for (int i = 0; i < Views.Length; i++)
        {
            bool shown = Views[i] == s.View;
            _viewChips[i].Text = $"{(Views[i] switch { GamesShown.Done => "Done", GamesShown.Everything => "Everything", _ => "To do" })} {s.CountOf(Views[i])}";
            _viewChips[i].SetTextColor(shown ? p.Text : p.SubText);
            _viewChips[i].SetTypeface(null, shown ? TypefaceStyle.Bold : TypefaceStyle.Normal);
            _viewChips[i].Background = shown ? Look.Rounded(Color.Transparent, 18) : Look.Rounded(Color.Transparent, 18, p.StrongBorder);
            if (shown)
                _viewPill.MoveTo(_viewChips[i]);
            Look.Enable(_viewChips[i], !blocked);
            Show(_viewChips[i], s.ViewsOffered.Contains(Views[i]));
        }
        // A job's own view says what it's doing, or did, where the views are.
        Show(_chipsRow, s.Job is null);
        Show(_jobHeadline, s.Job is not null);
        if (s.Job is { } job)
        {
            var (what, more) = job.Headline("\u2192");
            var headline = new SpannableStringBuilder(what);
            var color = job.Running ? p.Text : job.Outcome switch { UiLevel.Success => p.Good, UiLevel.Error => p.Bad, _ => p.Warn };
            headline.SetSpan(new Android.Text.Style.ForegroundColorSpan(color), 0, what.Length, SpanTypes.ExclusiveExclusive);
            headline.SetSpan(new Android.Text.Style.StyleSpan(TypefaceStyle.Bold), 0, what.Length, SpanTypes.ExclusiveExclusive);
            if (more.Length > 0)
                headline.Append("\n" + more);
            _jobHeadline.SetText(headline, TextView.BufferType.Spannable);
            _jobHeadline.SetTextColor(p.SubText);
        }
        bool viewEmpty = _adapter.Count == 0 && s.EverythingCount > 0;
        bool repacking = _session.Settings.Get("--nocompress") == "1";
        _viewEmpty.Text = s.View == GamesShown.Done
            ? repacking ? "Nothing here is done yet.\nGames show up here once they're repacked, and so do games with no update to make part of them."
                : "Nothing here is done yet.\nGames show up here once they're compressed, and so do games that already were when they're added."
            : repacking ? $"Everything here is done.\n{s.DoneCount} {(s.DoneCount == 1 ? "game has" : "games have")} nothing for a repack to gain, under Done."
                : $"Everything here is done.\n{s.DoneCount} {(s.DoneCount == 1 ? "game is" : "games are")} compressed, under Done.";
        Show(_viewEmpty, viewEmpty);
        _listPart.Visibility = empty ? ViewStates.Gone : ViewStates.Visible;
        _emptyPart.Visibility = empty ? ViewStates.Visible : ViewStates.Gone;
        bool reading = s.Activity == SessionActivity.Reading;
        _emptySpinner.Visibility = reading ? ViewStates.Visible : ViewStates.Gone;
        _emptyIcon.Visibility = reading ? ViewStates.Gone : ViewStates.Visible;
        _emptyTitle.Text = reading ? "Reading your games" : "Add your games";
        _emptyText.Text = reading ? s.ReadingText : EmptyText;
        _emptyButtons.Visibility = reading ? ViewStates.Gone : ViewStates.Visible;

        int games = s.Rows.Length;
        string counted = $"{games} {(games == 1 ? "game" : "games")}";
        _subtitle.Text = games == 0 ? "" : ticked == games ? $"{counted}, all ticked" : $"{counted}, {ticked} ticked";
        // Ticks are for the list: a job's own view has its games whatever's ticked.
        Show(_subtitle, games > 0 && s.Job is null);
        bool all = games > 0 && ticked == games;
        _tickAll.SetImageResource(all ? Resource.Drawable.ic_tick_all : Resource.Drawable.ic_tick_some);
        _tickAll.ContentDescription = _tickAll.TooltipText = all ? "Untick all" : "Tick all";
        Show(_tickAll, games > 1 && s.Job is null);
        // Ticking waits for measuring to give way, as on every version.
        Look.Enable(_tickAll, !busy);
        foreach (var button in new View[] { _add, _options, _more })
            Look.Enable(button, !blocked);

        // What comes next goes on the big button: compressing while anything ticked is left to do, and then sending it to the Switch.
        bool canCompress = s.Rows.Any(r => r.LeftToCompress(!repack));
        // Whatever's ticked is compressed already, so it's ready for the Switch. On Done with nothing ticked, sending the list is all that's
        // left too, which the big button then offers, rather than a greyed-out Compress beside an arrow with only that in it.
        _sendNext = !canCompress && (s.Rows.Any(r => r.Selected && (r.IsDone(BatchMode.Convert) || r.IsCompressed)) || ticked == 0 && s.View == GamesShown.Done);
        Look.Label(_compress, _sendNext ? "Send to Switch" : (repack ? "Repack" : "Compress") + (ticked < games && ticked > 0 ? $" {ticked}" : ""),
            _sendNext ? Resource.Drawable.ic_send : 0);
        bool listed = games > 0;
        // A job's own view has only what goes with it: stopping it, and once it's over, sending what it made and going back to the list.
        bool jobShown = s.Job is not null;
        bool over = s.Job is { Running: false };
        Show(_stop, busy);
        Show(_compress, !blocked && listed && (!jobShown || _sendNext));
        // Its menu has sending, while the big button isn't doing that already, and what else can be done to the ticked games.
        Show(_compressMenu, !blocked && listed && !jobShown && (!_sendNext || ticked > 0));
        // Without its arrow beside it, it's rounded on both sides.
        bool alone = _compressMenu.Visibility != ViewStates.Visible;
        if (alone != _compressAlone)
        {
            _compressAlone = alone;
            _compress.Background = Look.ButtonBackground(Style.Primary, 10, alone ? 10 : 0);
        }
        // Only while a ticked game can use it, or there's more to read in full, rather than greyed out.
        Show(_analyze, !busy && !jobShown && (s.Rows.Any(r => r.Selected && r.NeedsEstimate && !r.IsCompressed) || (listed && _session.CanMeasure)));
        Show(_back, over && !busy && _sendNext);
        Show(_backAlone, over && !busy && !_sendNext);
        Look.Enable(_compress, canCompress || _sendNext);
        Show(_details, true);

        var fraction = Status.Fraction(s);
        Show(_progress, busy);
        _progress.Indeterminate = busy && fraction is null;
        if (fraction is { } done)
            _progress.Progress = (int)(done * 1000);

        var (first, tone, second) = Status.Describe(s, _session.Settings);
        _statusFirst.Text = ProgressReporter.KeepFiguresWhole(first);
        // While a job runs, its line names the game, which may be shortened. What a job came to is said in full.
        _statusFirst.SetMaxLines(_wide || !s.IsBusy ? 3 : 1);
        _statusFirst.SetTextColor(tone switch { Tone.Good => p.Good, Tone.Warn => p.Warn, Tone.Bad => p.Bad, Tone.Quiet => p.SubText, _ => p.Text });
        Show(_statusFirst, first.Length > 0);
        _statusSecond.Text = ProgressReporter.KeepFiguresWhole(second);
        Show(_statusSecond, second.Length > 0);
        // With nothing listed and nothing going on, the empty list says it all.
        // Only with something on the list to act on: a list with every game hidden just says so, with a way to show them.
        bool panel = s.Rows.Length + s.Issues.Length > 0 || busy || s.Summary is not null;
        if (panel != (_panel.Visibility == ViewStates.Visible))
        {
            Show(_panel, panel);
            _panel.RequestApplyInsets();
        }

        if (_logText is not null && s.Log.Length != _logShown)
            ShowLog(s.Log);
        if (_sayVersion && s.UpdateNote is { Length: > 0 } note && !note.StartsWith("Looking", StringComparison.Ordinal))
        {
            _sayVersion = false;
            Toast.MakeText(this, note, ToastLength.Long)!.Show();
        }
        if (_sheetFirst is not null && _sheetSecond is not null)
        {
            _sheetFirst.Text = first.Length > 0 ? first : AppInfo.Name;
            _sheetFirst.SetTextColor(_statusFirst.TextColors);
            _sheetSecond.Text = second;
        }
        RefreshBanners(s);
        _ownBack?.Set(s.Job is { Running: false });

        // A job keeps going with the screen off, which takes the service.
        if (busy && !_wasBusy)
            WorkService.Start(this);
        _wasBusy = busy;
    }

    private static void Show(View view, bool shown) => view.Visibility = shown ? ViewStates.Visible : ViewStates.Gone;

    /// <summary>All of what's going on, and below it everything that's been said, kept up to date while it's open.</summary>
    private void ShowStatus()
    {
        var p = Look.P;
        var body = Sheet.Body(this);
        _sheetFirst = Look.Text(this, "", 19, p.Text, bold: true);
        _sheetFirst.SetTextIsSelectable(true);
        body.AddView(_sheetFirst, Look.Fill());
        _sheetSecond = Look.Text(this, "", 15, p.SubText);
        _sheetSecond.SetTextIsSelectable(true);
        body.AddView(_sheetSecond, Look.Fill(0, Look.Dp(6)));

        body.AddView(Sheet.Heading(this, "Everything that's been said"), Look.Fill(0, Look.Dp(20), 0, Look.Dp(6)));
        _logText = Look.Text(this, "", 12, p.SubText);
        _logText.Typeface = Typeface.Monospace;
        _logText.SetTextIsSelectable(true);
        body.AddView(_logText, Look.Fill());
        _logShown = -1;
        ShowLog(_state.Log);
        if (_state.Log.Length == 0)
        {
            _logText.Typeface = Typeface.Default;
            _logText.SetTextSize(Android.Util.ComplexUnitType.Sp, 14);
            _logText.Text = "Nothing yet. What each job does is said here as it goes.";
        }
        Refresh();

        var buttons = _state.IsBusy
            ? new (string, Style, Action?)[] { ("Stop", Style.Secondary, () => _session.Stop()), ("Close", Style.Primary, null) }
            : [("Close", Style.Primary, null)];
        _statusSheet = Sheet.Show(this, body, buttons);
        _logScroll = body.Parent as ScrollView;
        _statusSheet.SetOnDismissListener(new Dismissed(() =>
        {
            _logText = null;
            _logScroll = null;
            _sheetFirst = _sheetSecond = null;
            _statusSheet = null;
        }));
    }

    private sealed class Dismissed(Action dismissed) : Java.Lang.Object, IDialogInterfaceOnDismissListener
    {
        public void OnDismiss(IDialogInterface? dialog) => dismissed();
    }

    private void ShowLog(LogLine[] lines)
    {
        if (_logText is null)
            return;
        var p = Look.P;
        var text = new SpannableStringBuilder();
        foreach (var line in lines.TakeLast(400))
        {
            int start = text.Length();
            text.Append(line.Text + "\n");
            Color? color = line.Level switch { UiLevel.Error => p.Bad, UiLevel.Warning => p.Warn, UiLevel.Success => p.Good, UiLevel.Heading => p.Text, _ => null };
            if (color is { } c)
                text.SetSpan(new ForegroundColorSpan(c), start, text.Length(), SpanTypes.ExclusiveExclusive);
        }
        bool opening = _logShown < 0;
        _logText.SetText(text, TextView.BufferType.Spannable);
        _logShown = lines.Length;
        // Only as it opens: after that, whoever's reading keeps their place.
        if (opening)
            _logScroll?.Post(() => _logScroll?.FullScroll(FocusSearchDirection.Down));
    }

    /// <summary>Adding is one button at the top: files, or a folder to look through.</summary>
    private void OpenAdd()
    {
        var menu = new PopupMenu(this, _add, GravityFlags.End);
        var items = menu.Menu!;
        items.Add(0, 1, 0, "Add files");
        items.Add(0, 2, 0, "Add folder");
        menu.MenuItemClick += (_, e) =>
        {
            if (e.Item!.ItemId == 1)
                AddFiles();
            else
                AddFolder();
        };
        menu.Show();
    }

    /// <summary>What needs doing before anything else, at the top: access to files, the keys, and a newer version.</summary>
    private void RefreshBanners(SessionSnapshot s)
    {
        bool access = Storage.HasAccess(this);
        string key = $"{access}|{s.KeyProblem is null}|{AppState.PendingVersion}";
        if (key == _banners)
            return;
        _banners = key;
        _bannerHost.RemoveAllViews();
        if (!access)
            AddBanner("Allow access to your files", "It reads your games where they are and saves the compressed copies beside them.", "Allow",
                () => Storage.AskForAccess(this));
        if (s.KeyProblem is not null)
            AddBanner("Your keys are needed first", "Choose the prod.keys file from your Switch. It's kept inside this app.", "Choose keys", ChooseKeys);
        if (AppState.PendingVersion is { } version)
            AddBanner($"Version {version} is ready", "It's been downloaded and checked. Android asks before installing it.", "Install",
                () => Try(() => AppState.InstallPending(this)));
    }

    private void AddBanner(string title, string text, string action, Action click)
    {
        var p = Look.P;
        var banner = Look.Row(this);
        banner.Background = Look.Rounded(p.WarnSurface, 16);
        banner.SetPadding(Look.Dp(14), Look.Dp(10), Look.Dp(8), Look.Dp(10));
        var words = Look.Column(this);
        words.AddView(Look.Text(this, title, 15, p.Text, bold: true), Look.Wrap());
        words.AddView(Look.Text(this, text, 13, p.SubText), Look.Wrap(0, Look.Dp(2)));
        banner.AddView(words, Look.Stretch(true));
        banner.AddView(Look.Button(this, action, Style.Secondary, click), Look.Wrap(Look.Dp(8)));
        _bannerHost.AddView(banner, Look.Fill(0, 0, 0, Look.Dp(12)));
    }

    private void ItemTapped(int position)
    {
        if (_state.IsBusy && _adapter.Items[position] is not TitleAdapter.Item.Title)
            return;
        switch (_adapter.Items[position])
        {
            case TitleAdapter.Item.Title(var row):
                StartActivity(GameActivity.For(this, row.Key));
                break;
            case TitleAdapter.Item.Issue(var issue):
                var body = Sheet.Body(this);
                body.AddView(Look.Text(this, "Couldn't be added", 19, Look.P.Text, bold: true), Look.Fill());
                var message = Look.Text(this, issue.Message, 15, Look.P.SubText);
                message.SetTextIsSelectable(true);
                body.AddView(message, Look.Fill(0, Look.Dp(6)));
                Sheet.Show(this, body, ("Remove it from the list", Style.Secondary, () => _session.Remove(issue)), ("Close", Style.Primary, null));
                break;
        }
    }

    /// <summary>What can be done to a game, held down: what its Details have, a step sooner. Anything else held down is as if tapped.</summary>
    private bool LongPressed(int position, View? item)
    {
        if (_adapter.Items[position] is not TitleAdapter.Item.Title(var row))
        {
            ItemTapped(position);
            return true;
        }
        // Measuring goes on behind the list, and gives way to anything asked for.
        bool free = !_state.IsBusy || _state.Activity == SessionActivity.Measuring;
        var menu = new PopupMenu(this, item ?? _list, GravityFlags.End);
        var items = menu.Menu!;
        items.Add(0, 1, 0, "Details");
        if (!row.IsFolder)
            items.Add(0, 2, 0, "Look inside");
        if (free && !row.IsDone(BatchMode.Convert) && !(Compressing && row.IsCompressed))
            items.Add(0, 3, 0, Compressing ? "Compress this game" : "Repack this game");
        if (free && !row.IsFolder)
        {
            items.Add(0, 4, 0, "Extract this game");
            items.Add(0, 5, 0, "Check this game for damage");
        }
        if (free)
            items.Add(0, 6, 0, "Remove it from the list");
        menu.MenuItemClick += (_, e) =>
        {
            switch (e.Item!.ItemId)
            {
                case 1: StartActivity(GameActivity.For(this, row.Key)); break;
                case 2: StartActivity(GameActivity.For(this, row.Key, lookInside: true)); break;
                case 3: _session.Compress(row.Key); break;
                case 4: _session.Extract(row.Key); break;
                case 5: _session.Check(row.Key); break;
                case 6: _session.Remove(row); break;
            }
        };
        menu.Show();
        return true;
    }

    /// <summary>Ticks or unticks a game, from its tick box in the list.</summary>
    internal void Toggle(TitleRow row)
    {
        if (!_state.IsBusy)
            _session.Toggle(row);
    }

    private void PressCompress()
    {
        if (_sendNext)
        {
            _session.SendToSwitch();
            return;
        }
        if (!_state.Rows.Any(r => r.LeftToCompress(_session.Settings.Get("--nocompress") != "1")))
            return;
        var names = _session.TickedWithoutUpdate();
        if (names.Length == 0)
        {
            _session.Compress();
            return;
        }
        const int NamesShown = 6;
        string list = string.Join("\n", names.Take(NamesShown)) + (names.Length > NamesShown ? $"\nand {names.Length - NamesShown} more" : "");
        new AlertDialog.Builder(this)
            .SetMessage($"With compression off, repacking only makes a title smaller by making its update part of it, and these have none:\n\n{list}\n\nUntick them?")!
            .SetPositiveButton("Untick them", (_, _) =>
            {
                _session.UntickWithoutUpdate();
                _session.Compress();
            })!
            .SetNeutralButton("Repack them too", (_, _) => _session.Compress())!
            .SetNegativeButton("Cancel", (_, _) => { })!
            .Show();
    }

    /// <summary>The arrow on Compress: sending, while the big button isn't doing that already, and the other things to do to the ticked games.</summary>
    private void OpenCompressMenu()
    {
        int ticked = _state.Rows.Count(r => r.Selected);
        string games = ticked == 1 ? "the ticked game" : "the ticked games";
        var menu = new PopupMenu(this, _compressMenu, GravityFlags.End);
        var items = menu.Menu!;
        if (!_sendNext)
            items.Add(0, 2, 0, "Send to Switch");
        if (ticked > 0)
        {
            items.Add(0, 3, 0, $"Extract {games}");
            items.Add(0, 4, 0, $"Check {games} for damage");
            items.Add(0, 5, 0, ticked == 1 ? "Analyze it again" : "Analyze them again");
            items.Add(0, 6, 0, ticked == 1 ? "Remove it from the list" : "Remove them from the list");
        }
        menu.MenuItemClick += (_, e) =>
        {
            switch (e.Item!.ItemId)
            {
                case 2: _session.SendToSwitch(); break;
                case 3: _session.Extract(); break;
                case 4: _session.Check(); break;
                case 5: _session.AnalyzeAgain(); break;
                case 6: _session.RemoveTicked(); break;
            }
        };
        menu.Show();
    }

    private void OpenMore()
    {
        // The app's own things, and clearing the list. What's done to games is on them, and on the arrow beside Compress for the ticked ones.
        var menu = new PopupMenu(this, _more, GravityFlags.End);
        var items = menu.Menu!;
        items.Add(0, 1, 0, "Choose keys…");
        if (_state.Rows.Length + _state.Issues.Length > 0)
            items.Add(0, 4, 0, "Clear the list");
        items.Add(0, 5, 0, "Help");
        items.Add(0, 6, 0, "Report a problem");
        items.Add(0, 7, 0, "Check for a new version");
        items.Add(0, 3, 0, "About");
        menu.MenuItemClick += (_, e) =>
        {
            switch (e.Item!.ItemId)
            {
                case 1: ChooseKeys(); break;
                case 3: ShowAbout(); break;
                case 4: _session.RemoveAll(); break;
                case 5: ShowHelp(); break;
                case 6: ReportProblem(); break;
                case 7:
                    _sayVersion = true;
                    _session.CheckForUpdates();
                    break;
            }
        };
        menu.Show();
    }

    /// <summary>The topics, each a tap away, with where to read the rest of it.</summary>
    private void ShowHelp() => new AlertDialog.Builder(this)
        .SetTitle("Help")!
        .SetItems(Help.Topics.Select(t => t.Title).Append("Report a problem").ToArray(), (_, e) =>
        {
            if (e.Which >= Help.Topics.Length)
            {
                ReportProblem();
                return;
            }
            var topic = Help.Topics[e.Which];
            new AlertDialog.Builder(this)
                .SetTitle(topic.Title)!
                .SetMessage(topic.Text)!
                .SetNeutralButton("Read more", (_, _) => OpenWebPage(topic.Url))!
                .SetPositiveButton("Close", (_, _) => { })!
                .Show();
        })!
        .SetPositiveButton("Close", (_, _) => { })!
        .Show();

    /// <summary>
    /// Straight to the developer, with what the person was doing, and the report to read through first. Sharing it through whatever the
    /// phone has, such as mail or a chat, is still there, and is what happens when it can't be sent.
    /// </summary>
    private void ReportProblem()
    {
        var p = Look.P;
        string report = ReportSender.Prepared(_session.ReportText());
        var body = Sheet.Body(this);
        body.AddView(Look.Text(this, "Report a problem", 19, p.Text, bold: true), Look.Fill());
        body.AddView(Look.Text(this, ReportSender.CanSend
            ? $"This goes straight to the developer: {OptionCatalog.ReportHolds}. Saying what you were doing helps most."
            : $"It's shared as text to send to the developer: {OptionCatalog.ReportHolds}. Saying what you were doing helps most.", 14, p.SubText), Look.Fill(0, Look.Dp(6)));
        var said = new EditText(this) { Hint = "What you were doing, such as which game and what you tapped" };
        said.SetMinLines(2);
        body.AddView(said, Look.Fill(0, Look.Dp(10)));
        var shown = Look.Text(this, report.Length > 20_000 ? report[^20_000..] : report, 12, p.SubText);
        shown.SetTextIsSelectable(true);
        var scroll = new ScrollView(this);
        scroll.AddView(shown);
        body.AddView(scroll, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MatchParent, Look.Dp(220)) { TopMargin = Look.Dp(10) });
        if (ReportSender.CanSend)
            Sheet.Show(this, body, ("Share instead", Style.Secondary, () => ShareReport(report, said.Text ?? "")),
                ("Send", Style.Primary, () => SendReport(report, said.Text ?? "")));
        else
            // With nowhere to send it yet, sharing it is all there is.
            Sheet.Show(this, body, ("Share", Style.Primary, () => ShareReport(report, said.Text ?? "")));
    }

    /// <summary>When it can't be sent, sharing it is offered, as a file saved inside the app is somewhere people can't get to.</summary>
    private void SendReport(string report, string said)
    {
        Toast.MakeText(this, "Sending the report\u2026", ToastLength.Short)!.Show();
        string title = said.Trim().Length > 0 ? said.Trim()[..Math.Min(said.Trim().Length, 80)] : "A report from Android";
        new Thread(() =>
        {
            var outcome = ReportSender.Send("manual", "", title, said.Trim(), report);
            RunOnUiThread(() =>
            {
                var box = new AlertDialog.Builder(this);
                if (outcome.Sent)
                    box.SetMessage(outcome.Said)!.SetPositiveButton("OK", (_, _) => { });
                else
                    box.SetMessage($"It couldn't be sent{(outcome.Problem is { } why ? $" ({why})" : "")}. Share it instead, through mail or a chat?")!
                        .SetPositiveButton("Share it", (_, _) => ShareReport(report, said))!
                        .SetNegativeButton("Close", (_, _) => { });
                box.Show();
            });
        }) { IsBackground = true, Name = "Sending a report" }.Start();
    }

    /// <summary>Shared through whatever the phone has. What can be passed along is limited, so a long log keeps its end, where what went wrong is.</summary>
    private void ShareReport(string report, string said)
    {
        const int Most = 200_000;
        string text = said.Trim().Length > 0 ? $"What I was doing: {said.Trim()}\n\n{report}" : report;
        if (text.Length > Most)
            text = text[..20_000] + "\n[...]\n" + text[^(Most - 20_000)..];
        var send = new Intent(Intent.ActionSend);
        send.SetType("text/plain");
        send.PutExtra(Intent.ExtraSubject, $"{AppInfo.Name} {AppInfo.Version} report");
        send.PutExtra(Intent.ExtraText, text);
        StartActivity(Intent.CreateChooser(send, "Share the report"));
    }

    private void OpenWebPage(string url)
    {
        try
        {
            StartActivity(new Intent(Intent.ActionView, Android.Net.Uri.Parse(url)));
        }
        catch (ActivityNotFoundException)
        {
            Toast.MakeText(this, url, ToastLength.Long)!.Show();
        }
    }

    private void ShowAbout() => new AlertDialog.Builder(this)
        .SetTitle($"{AppInfo.Name} {AppInfo.Version}")!
        .SetMessage($"{OptionCatalog.WelcomeIntro}\n\n{OptionCatalog.License}")!
        .SetPositiveButton("Close", (_, _) => { })!
        .Show();

    private void AddFiles()
    {
        if (NeedsAccess())
            return;
        Pick(Storage.PickFiles(true), AddFilesCode);
    }

    private void AddFolder()
    {
        if (NeedsAccess())
            return;
        Pick(Storage.PickFolder(), AddFolderCode);
    }

    private void ChooseKeys() => Pick(Storage.PickFiles(false), KeysCode);

    /// <summary>Games can only be read where they are with access to files, so it's asked for first.</summary>
    private bool NeedsAccess()
    {
        if (Storage.HasAccess(this))
            return false;
        new AlertDialog.Builder(this)
            .SetTitle("Allow access to your files")!
            .SetMessage("hacBrewPackLZ reads your games where they are and saves the compressed copies beside them, as on a PC. Android calls that access to all files.")!
            .SetPositiveButton("Allow", (_, _) => Storage.AskForAccess(this))!
            .SetNegativeButton("Not now", (_, _) => { })!
            .Show();
        return true;
    }

    private void Pick(Intent intent, int code)
    {
        try
        {
            StartActivityForResult(intent, code);
        }
        catch (ActivityNotFoundException)
        {
            Say("This phone has no file picker to choose with.");
        }
    }

    protected override void OnActivityResult(int requestCode, Result resultCode, Intent? data)
    {
        base.OnActivityResult(requestCode, resultCode, data);
        if (resultCode != Result.Ok)
            return;
        switch (requestCode)
        {
            case AddFilesCode:
            {
                var (paths, failed) = Storage.PathsFrom(this, data);
                if (paths.Count > 0)
                    _session.AddInputs(paths);
                if (failed.Count > 0)
                    Say($"Couldn't find where {string.Join(", ", failed)} {(failed.Count == 1 ? "is" : "are")} stored. Choose from the phone's storage or memory card in the picker's menu, rather than from Recent.");
                break;
            }
            case AddFolderCode:
                if (Storage.FolderFrom(data) is { } folder)
                    _session.AddInputs([folder]);
                else
                    Say("Couldn't use that folder. Choose one on the phone's storage or memory card.");
                break;
            case KeysCode:
                if (Storage.KeepKeys(this, data) is { } problem)
                {
                    Say(problem);
                    break;
                }
                // The keys kept inside the app are what's used from now on.
                _session.Settings.Set("--keyset", "");
                _session.Settings.Save();
                _session.ReloadKeys();
                Toast.MakeText(this, "Keys saved", ToastLength.Short)!.Show();
                break;
        }
    }

    private void Say(string message) =>
        new AlertDialog.Builder(this).SetMessage(message)!.SetPositiveButton("OK", (_, _) => { })!.Show();

    private void Try(Action action)
    {
        try
        {
            action();
        }
        catch (Exception e) when (e is IOException or Java.Lang.Exception)
        {
            Say(e.Message);
        }
    }
}
