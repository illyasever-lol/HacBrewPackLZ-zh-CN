using Android.App;
using Android.Content;
using Android.Content.PM;
using Android.Content.Res;
using Android.Graphics;
using Android.OS;
using Android.Views;
using Android.Widget;

namespace HacBrewPackLz;

/// <summary>
/// A game's Details, as on the Switch and in every version: the game with the one figure that decides, its facts in the same four places and
/// what's good to know, with what can be done to it. Look inside and compressing it are along the bottom, where the list has its next step,
/// and the rest is in ⋮. The game is opened as the page opens, by a reader that Look inside goes on with.
/// </summary>
[Activity(Label = "Details", Exported = false,
    ConfigurationChanges = ConfigChanges.Orientation | ConfigChanges.ScreenSize | ConfigChanges.ScreenLayout | ConfigChanges.SmallestScreenSize
        | ConfigChanges.Keyboard | ConfigChanges.KeyboardHidden)]
public sealed class GameActivity : Activity
{
    private const string KeyExtra = "key";
    private const string InsideExtra = "inside";

    /// <param name="lookInside">Straight on into Look inside once the game is open, as its menu on the list asks.</param>
    public static Intent For(Context context, string key, bool lookInside = false) =>
        new Intent(context, typeof(GameActivity)).PutExtra(KeyExtra, key).PutExtra(InsideExtra, lookInside);

    /// <summary>The game whose Details are open and the reader that opened it, which Look inside goes on with.</summary>
    internal static (string key, InsideReader reader)? Reading { get; private set; }

    private Session _session = null!;
    private string _key = "";
    private InsideReader? _reader;
    private bool _insideWhenOpen;
    private int _pending;
    /// <summary>What the page says, so it's only made again when that changes, rather than with every bit of progress elsewhere.</summary>
    private string _shown = "";
    private LinearLayout _content = null!;
    private ImageButton _more = null!;
    private Button _look = null!, _compress = null!;
    private LinearLayout _panel = null!;
    private Bitmap? _icon;
    private byte[]? _iconFrom;

    private bool Compressing => _session.Settings.Get("--nocompress") != "1";

    protected override void OnCreate(Bundle? savedInstanceState)
    {
        base.OnCreate(savedInstanceState);
        Look.Refresh(this);
        _session = AppState.Session(this);
        _key = Intent?.GetStringExtra(KeyExtra) ?? "";
        _insideWhenOpen = Intent?.GetBooleanExtra(InsideExtra, false) ?? false;
        if (Row(_session.Snapshot()) is not { } row)
        {
            Finish();
            return;
        }
        // A game's folders are its files already, with nothing packed to open.
        if (!row.IsFolder)
        {
            _reader = new InsideReader(row.Name, row.Files, _session.Keys);
            _reader.Changed += OnChanged;
            Reading = (_key, _reader);
        }
        SetContentView(Build());
        _session.Changed += OnChanged;
        Refresh();
    }

    protected override void OnDestroy()
    {
        _session.Changed -= OnChanged;
        if (_reader is { } reader)
        {
            reader.Changed -= OnChanged;
            if (Reading?.reader == reader)
                Reading = null;
            reader.CloseInBackground();
        }
        base.OnDestroy();
    }

    public override void OnConfigurationChanged(Configuration newConfig)
    {
        base.OnConfigurationChanged(newConfig);
        FitWidth();
    }

    private TitleRow? Row(SessionSnapshot s) => s.Rows.FirstOrDefault(r => r.Key == _key);

    private View Build()
    {
        var p = Look.P;
        var root = Look.Column(this);
        root.SetBackgroundColor(p.Background);

        // The same bar as Options: the way back and where this is, and ⋮ for the rest of what can be done to the game.
        var bar = Look.Row(this);
        bar.SetPadding(Look.Dp(4), 0, Look.Dp(4), 0);
        bar.AddView(Look.IconButton(this, Resource.Drawable.ic_back, "Back", Finish), Look.Square(48));
        var title = Look.Text(this, "Details", 18, p.Text, bold: true);
        var titleLayout = Look.Stretch(true);
        titleLayout.LeftMargin = Look.Dp(8);
        bar.AddView(title, titleLayout);
        _more = Look.IconButton(this, Resource.Drawable.ic_more, "More", ShowMore);
        bar.AddView(_more, Look.Square(48));
        root.AddView(bar, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MatchParent, Look.Dp(56)));

        var scroll = new ScrollView(this);
        _content = Look.Column(this);
        scroll.AddView(_content);
        root.AddView(scroll, Look.Stretch(false));

        _panel = Look.Row(this);
        _panel.Background = Look.Corners(p.Surface, 24, 24, 0, 0);
        _look = Look.Button(this, "Look inside", Style.Secondary, OpenInside);
        _compress = Look.Button(this, "Compress this game", Style.Primary, () => Do(BatchMode.Convert));
        _panel.AddView(_look, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WrapContent, 1) { RightMargin = Look.Dp(10) });
        _panel.AddView(_compress, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WrapContent, 1.4f));
        root.AddView(_panel, Look.Fill());
        // Its padding first, which the navigation bar's is added to.
        FitWidth();
        Look.KeepClearOfBars(root, _panel);
        return root;
    }

    /// <summary>On a wide screen, as with the phone on its side, it stays a width that reads well, in the middle, as Options does.</summary>
    private void FitWidth()
    {
        int side = Math.Max(20, (Resources!.Configuration!.ScreenWidthDp - 680) / 2);
        _content.SetPadding(Look.Dp(side), Look.Dp(8), Look.Dp(side), Look.Dp(24));
        _panel.SetPadding(Look.Dp(side), Look.Dp(12), Look.Dp(side), _panel.PaddingBottom > 0 ? _panel.PaddingBottom : Look.Dp(16));
    }

    private void OnChanged()
    {
        if (Interlocked.Exchange(ref _pending, 1) == 0)
            RunOnUiThread(Refresh);
    }

    private void Refresh()
    {
        Interlocked.Exchange(ref _pending, 0);
        if (IsFinishing || IsDestroyed)
            return;
        var s = _session.Snapshot();
        if (Row(s) is not { } row)
        {
            // Taken off the list, from here or from anywhere else.
            Finish();
            return;
        }
        var inside = _reader?.Inside;
        if (_insideWhenOpen && _reader is { Opening: false })
        {
            _insideWhenOpen = false;
            if (inside is not null)
                OpenInside();
        }

        // Measuring goes on behind the list, and gives way to anything asked for.
        bool free = !s.IsBusy || s.Activity == SessionActivity.Measuring;
        bool canCompress = !row.IsDone(BatchMode.Convert) && !(Compressing && row.IsCompressed);
        Show(_look, !row.IsFolder);
        Look.Enable(_look, inside is not null);
        Look.Label(_compress, Compressing ? "Compress this game" : "Repack this game", 0);
        Show(_compress, canCompress);
        Look.Enable(_compress, free);
        Show(_panel, !row.IsFolder || canCompress);
        Show(_more, free);

        var figure = GameDetails.Figure(row, Compressing);
        var sections = GameDetails.Sections(row, inside, _reader?.Problem, Compressing);
        var notes = GameDetails.Notes(row);
        byte[]? iconData = inside?.Icon ?? row.Icon;
        var said = new List<string> { row.Name, inside?.Publisher ?? "", figure.ToString(), iconData?.Length.ToString() ?? "" };
        said.AddRange(sections.Select(x => x.Title + ": " + string.Join("; ", x.Lines)));
        said.AddRange(notes);
        string shown = string.Join("\n", said);
        if (shown == _shown)
            return;
        _shown = shown;
        Fill(row, inside?.Publisher, iconData, figure, sections, notes);
    }

    private static void Show(View view, bool shown) => view.Visibility = shown ? ViewStates.Visible : ViewStates.Gone;

    private void Fill(TitleRow row, string? publisher, byte[]? iconData, DetailsFigure figure, List<DetailsSection> sections, List<string> notes)
    {
        var p = Look.P;
        _content.RemoveAllViews();

        // The game: its picture, its name and who made it.
        var head = Look.Row(this);
        var icon = new ImageView(this) { Background = Look.Rounded(p.Hover, 14), ClipToOutline = true };
        if (Picture(iconData) is { } picture)
        {
            icon.SetScaleType(ImageView.ScaleType.CenterCrop);
            icon.SetImageBitmap(picture);
        }
        else
        {
            icon.SetScaleType(ImageView.ScaleType.Center);
            icon.ImageTintList = ColorStateList.ValueOf(p.Faint);
            icon.SetImageResource(row.IsFolder ? Resource.Drawable.ic_folder : Resource.Drawable.ic_game);
        }
        head.AddView(icon, new LinearLayout.LayoutParams(Look.Dp(72), Look.Dp(72)) { RightMargin = Look.Dp(16) });
        var names = Look.Column(this);
        var name = Look.Text(this, row.Name, 20, p.Text, bold: true);
        name.SetTextIsSelectable(true);
        names.AddView(name, Look.Fill());
        if (publisher is { Length: > 0 })
            names.AddView(Look.Text(this, publisher, 14, p.SubText), Look.Fill(0, Look.Dp(2)));
        head.AddView(names, Look.Stretch(true));
        _content.AddView(head, Look.Fill(0, Look.Dp(8)));

        // The one figure that decides, with a bar for what it saves.
        var card = Look.Column(this);
        card.Background = Look.Rounded(p.Surface, 16);
        card.SetPadding(Look.Dp(16), Look.Dp(12), Look.Dp(16), Look.Dp(14));
        card.AddView(Look.Text(this, figure.Label, 13, p.SubText), Look.Fill());
        card.AddView(Look.Text(this, figure.Big, 26, p.Accent, bold: true), Look.Fill(0, Look.Dp(2)));
        if (figure.Sub.Length > 0)
            card.AddView(Look.Text(this, figure.Sub, 14, p.SubText), Look.Fill(0, Look.Dp(2)));
        if (figure.Share is { } comesTo && comesTo < 1)
        {
            double saved = 1 - comesTo;
            var bar = new ShareBar(this);
            bar.Set(saved, Look.SavingColor(saved));
            card.AddView(bar, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MatchParent, Look.Dp(6)) { TopMargin = Look.Dp(10) });
        }
        _content.AddView(card, Look.Fill(0, Look.Dp(18)));

        // Its facts, in the same four places as on the Switch: each with its label beside it, and a word more under it.
        foreach (var section in sections)
        {
            _content.AddView(Sheet.Heading(this, section.Title), Look.Fill(0, Look.Dp(22), 0, Look.Dp(8)));
            foreach (var line in section.Lines)
            {
                var item = Look.Row(this, GravityFlags.Top);
                item.AddView(Look.Text(this, line.Label, 14, p.SubText), new LinearLayout.LayoutParams(Look.Dp(116), ViewGroup.LayoutParams.WrapContent) { RightMargin = Look.Dp(12) });
                var value = Look.Column(this);
                string shown = line.Value.StartsWith('/') ? Storage.Friendly(line.Value) : line.Value;
                var text = Look.Text(this, shown, 15, line.Bad ? p.Bad : line.Good ? p.Good : p.Text);
                text.SetTextIsSelectable(true);
                value.AddView(text, Look.Fill());
                if (line.Note.Length > 0)
                    value.AddView(Look.Text(this, line.Note, 13, p.Faint), Look.Fill(0, Look.Dp(2)));
                item.AddView(value, Look.Stretch(true));
                _content.AddView(item, Look.Fill(0, 0, 0, Look.Dp(10)));
            }
        }
        if (notes.Count > 0)
        {
            _content.AddView(Sheet.Heading(this, "Good to know"), Look.Fill(0, Look.Dp(22), 0, Look.Dp(8)));
            foreach (string note in notes)
                _content.AddView(Look.Text(this, note, 14, p.SubText), Look.Fill(0, 0, 0, Look.Dp(8)));
        }
    }

    /// <summary>The game's picture, made once for as long as it's the same one.</summary>
    private Bitmap? Picture(byte[]? data)
    {
        if (data is null)
            return null;
        if (!ReferenceEquals(data, _iconFrom))
        {
            _iconFrom = data;
            _icon = BitmapFactory.DecodeByteArray(data, 0, data.Length);
        }
        return _icon;
    }

    /// <summary>The rest of what can be done to it, as its menu on the list has it.</summary>
    private void ShowMore()
    {
        if (Row(_session.Snapshot()) is not { } row)
            return;
        var menu = new PopupMenu(this, _more, GravityFlags.End);
        var items = menu.Menu!;
        if (!row.IsFolder)
        {
            items.Add(0, 1, 0, "Extract this game");
            items.Add(0, 2, 0, "Check this game for damage");
        }
        items.Add(0, 3, 0, "Remove it from the list");
        menu.MenuItemClick += (_, e) =>
        {
            switch (e.Item!.ItemId)
            {
                case 1:
                    Do(BatchMode.Extract);
                    break;
                case 2:
                    Do(BatchMode.Check);
                    break;
                case 3:
                    _session.Remove(row);
                    Finish();
                    break;
            }
        };
        menu.Show();
    }

    private void Do(BatchMode mode)
    {
        bool started = mode switch
        {
            BatchMode.Extract => _session.Extract(_key),
            BatchMode.Check => _session.Check(_key),
            _ => _session.Compress(_key),
        };
        // The job shows on the list, as it does from there.
        if (started)
            Finish();
    }

    private void OpenInside()
    {
        if (_reader?.Inside is not null)
            StartActivity(InsideActivity.For(this, _key));
    }
}
