using Android.App;
using Android.Content;
using Android.Content.PM;
using Android.Content.Res;
using Android.Graphics;
using Android.Graphics.Drawables;
using Android.OS;
using Android.Text;
using Android.Views;
using Android.Views.InputMethods;
using Android.Widget;

namespace HacBrewPackLz;

/// <summary>
/// Look inside, as on the Switch: the game's parts as folders, each opened with a tap and left with Back, and what any of it is a tap away, the
/// way its kind is shown, with Save to… for it, or a whole folder with a long press or ⋮, through the phone's own picker. Find goes through every
/// name in it. The game's Details opened it, with the reader that goes on here, and stay open under it.
/// </summary>
[Activity(Label = "Look inside", Exported = false,
    ConfigurationChanges = ConfigChanges.Orientation | ConfigChanges.ScreenSize | ConfigChanges.ScreenLayout | ConfigChanges.SmallestScreenSize
        | ConfigChanges.Keyboard | ConfigChanges.KeyboardHidden)]
public sealed class InsideActivity : Activity
{
    private const string KeyExtra = "key";
    private const int SaveCode = 30;
    /// <summary>The most lines a preview lays out on a phone, where a long text, or many bytes, would take a while.</summary>
    private const int MostLines = 2000;

    public static Intent For(Context context, string key) => new Intent(context, typeof(InsideActivity)).PutExtra(KeyExtra, key);

    private InsideReader _reader = null!;
    private int _pending;
    private int _folder;
    private List<int>? _found;
    private string _finding = "";
    /// <summary>Where it's been, to go back to with Back: a folder, or what Find found (-1), and how far down the list was.</summary>
    private readonly Stack<(int folder, int first)> _above = new();
    private TextView _title = null!, _where = null!, _status = null!;
    private LinearLayout _findRow = null!, _statusRow = null!;
    private EditText _find = null!;
    private ImageButton _more = null!;
    private ListView _list = null!;
    private EntryAdapter _adapter = null!;
    private Button _stop = null!;
    /// <summary>What a tap shows, in a sheet from the bottom, while it's open, and whether it's what's shown in it yet.</summary>
    private Dialog? _sheet;
    private int _previewing = -1;
    private bool _asBytes;
    private (int index, bool bytes) _filled = (-1, false);
    private LinearLayout? _previewBody;
    private TextView? _previewChips;
    private Button? _bytesButton;
    /// <summary>What Save to… is for, while the picker is open.</summary>
    private int _toSave = -1;
    /// <summary>Back goes up a folder, out of what Find found, or stops a save, before it leaves.</summary>
    private OwnBack? _ownBack;

    private GameInside Inside => _reader.Inside!;

    protected override void OnCreate(Bundle? savedInstanceState)
    {
        base.OnCreate(savedInstanceState);
        Look.Refresh(this);
        string key = Intent?.GetStringExtra(KeyExtra) ?? "";
        if (GameActivity.Reading is not { } reading || reading.key != key || reading.reader.Inside is null)
        {
            Finish();
            return;
        }
        _reader = reading.reader;
        _ownBack = OwnBack.For(this, GoBack);
        SetContentView(Build());
        _reader.Changed += OnChanged;
        ShowFolder(0);
    }

    protected override void OnDestroy()
    {
        if (_reader is not null)
            _reader.Changed -= OnChanged;
        _sheet?.Dismiss();
        base.OnDestroy();
    }

    private View Build()
    {
        var p = Look.P;
        var root = Look.Column(this);
        root.SetBackgroundColor(p.Background);

        // The bar: the way back, where this is, Find, and ⋮ for saving all of the folder.
        var bar = Look.Row(this);
        bar.SetPadding(Look.Dp(4), 0, Look.Dp(4), 0);
        bar.AddView(Look.IconButton(this, Resource.Drawable.ic_back, "Back", GoBack), Look.Square(48));
        _title = Look.Text(this, "", 18, p.Text, bold: true);
        _title.SetSingleLine(true);
        _title.Ellipsize = TextUtils.TruncateAt.Middle;
        var titleLayout = Look.Stretch(true);
        titleLayout.LeftMargin = Look.Dp(8);
        bar.AddView(_title, titleLayout);
        bar.AddView(Look.IconButton(this, Resource.Drawable.ic_search, "Find", OpenFind), Look.Square(48));
        _more = Look.IconButton(this, Resource.Drawable.ic_more, "More", ShowMore);
        bar.AddView(_more, Look.Square(48));
        root.AddView(bar, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MatchParent, Look.Dp(56)));

        // Find, under the bar once it's asked for.
        _findRow = Look.Row(this);
        _findRow.SetPadding(Look.Dp(16), 0, Look.Dp(16), Look.Dp(4));
        _find = new EditText(this) { Hint = "Find a file or folder by name" };
        _find.SetSingleLine(true);
        _find.SetTextColor(p.Text);
        _find.SetHintTextColor(p.Faint);
        _find.BackgroundTintList = ColorStateList.ValueOf(p.Accent);
        _find.ImeOptions = ImeAction.Search;
        _find.EditorAction += (_, e) =>
        {
            e.Handled = e.ActionId is ImeAction.Search or ImeAction.Done || e.Event?.KeyCode == Keycode.Enter;
            if (e.Handled)
                Find(_find.Text ?? "");
        };
        _findRow.AddView(_find, Look.Stretch(true));
        _findRow.Visibility = ViewStates.Gone;
        root.AddView(_findRow, Look.Fill());

        _where = Look.Text(this, "", 13, p.SubText);
        _where.SetPadding(Look.Dp(20), 0, Look.Dp(20), Look.Dp(8));
        _where.SetMaxLines(2);
        _where.Ellipsize = TextUtils.TruncateAt.Start;
        root.AddView(_where, Look.Fill());

        _adapter = new EntryAdapter(this);
        _list = new ListView(this) { Adapter = _adapter, Divider = new ColorDrawable(Color.Transparent), DividerHeight = 0 };
        _list.SetSelector(Android.Resource.Color.Transparent);
        _list.SetPadding(Look.Dp(8), 0, Look.Dp(8), Look.Dp(12));
        _list.SetClipToPadding(false);
        _list.ItemClick += (_, e) => Tapped(_adapter.Shown[e.Position]);
        _list.ItemLongClick += (_, e) =>
        {
            Preview(_adapter.Shown[e.Position]);
            e.Handled = true;
        };
        root.AddView(_list, Look.Stretch(false));

        // How saving's going, where it went, or why it couldn't be, along the bottom while there's something to say.
        _statusRow = Look.Row(this);
        _statusRow.Background = Look.Corners(p.Surface, 24, 24, 0, 0);
        _statusRow.SetPadding(Look.Dp(20), Look.Dp(12), Look.Dp(16), Look.Dp(12));
        _status = Look.Text(this, "", 14, p.Text);
        _status.SetMaxLines(3);
        _status.Ellipsize = TextUtils.TruncateAt.Middle;
        _statusRow.AddView(_status, Look.Stretch(true));
        _stop = Look.Button(this, "Stop", Style.Secondary, () => _reader.StopSaving());
        _statusRow.AddView(_stop, Look.Wrap(Look.Dp(8)));
        _statusRow.Visibility = ViewStates.Gone;
        root.AddView(_statusRow, Look.Fill());
        Look.KeepClearOfBars(root, _statusRow);
        return root;
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
        RefreshStatus();
        FillPreview();
    }

    private static void Show(View view, bool shown) => view.Visibility = shown ? ViewStates.Visible : ViewStates.Gone;

    /// <summary>A folder, or what Find found, from this far down.</summary>
    private void ShowFolder(int first)
    {
        var inside = Inside;
        _adapter.Set(inside, _found ?? inside.Nodes[_folder].Children, _found is not null);
        _title.Text = _found is not null ? _finding : _folder == 0 ? inside.Name : inside.Nodes[_folder].Name;
        _where.Text = _found is not null
            ? _found.Count == 0 ? $"Nothing in it is called \"{_finding}\"" : $"Found {GameInside.WithCommas(_found.Count)} named \"{_finding}\""
            : _folder == 0 ? $"{GameInside.WithCommas(inside.Files)} files, {ProgressReporter.Format(inside.Bytes)}"
            : $"{inside.Name}  ›  {inside.PathOf(_folder).Replace("/", "  ›  ")}";
        _list.SetSelection(first);
        Show(_more, _found is null);
        RefreshStatus();
    }

    /// <summary>Whether Back has anything to do here before it leaves, as GoBack goes.</summary>
    private void UpdateBack() =>
        _ownBack?.Set(_reader.Saving || _findRow.Visibility == ViewStates.Visible || _above.Count > 0 || _found is not null || _folder != 0);

    private void Tapped(int index)
    {
        if (Inside.Nodes[index].Folder)
            Open(index);
        else
            Preview(index);
    }

    private void Open(int index)
    {
        _above.Push((_found is null ? _folder : -1, _list.FirstVisiblePosition));
        _folder = index;
        _found = null;
        CloseFind();
        ShowFolder(0);
    }

    /// <summary>Back stops a save that's going on, then goes back the way it came, up through the folders, and out to the game's Details.</summary>
    private void GoBack()
    {
        if (_reader.Saving)
        {
            _reader.StopSaving();
            return;
        }
        if (_findRow.Visibility == ViewStates.Visible && _found is null)
        {
            CloseFind();
            return;
        }
        if (_above.Count > 0)
        {
            var (folder, first) = _above.Pop();
            if (folder < 0)
            {
                _found = Inside.Search(_finding);
            }
            else
            {
                _found = null;
                _folder = folder;
                CloseFind();
            }
            ShowFolder(first);
            return;
        }
        if (_found is not null || _folder != 0)
        {
            _found = null;
            _folder = 0;
            CloseFind();
            ShowFolder(0);
            return;
        }
        Finish();
    }

    public override void OnBackPressed() => GoBack();

    private void OpenFind()
    {
        if (_findRow.Visibility == ViewStates.Visible)
        {
            Find(_find.Text ?? "");
            return;
        }
        _findRow.Visibility = ViewStates.Visible;
        UpdateBack();
        _find.RequestFocus();
        (GetSystemService(InputMethodService) as InputMethodManager)?.ShowSoftInput(_find, ShowFlags.Implicit);
    }

    private void CloseFind()
    {
        if (_findRow.Visibility != ViewStates.Visible)
            return;
        HideKeyboard();
        _find.Text = "";
        _findRow.Visibility = ViewStates.Gone;
        UpdateBack();
    }

    private void HideKeyboard() => (GetSystemService(InputMethodService) as InputMethodManager)?.HideSoftInputFromWindow(_find.WindowToken, HideSoftInputFlags.None);

    private void Find(string text)
    {
        text = text.Trim();
        if (text.Length == 0)
            return;
        if (_found is null)
            _above.Push((_folder, _list.FirstVisiblePosition));
        _found = Inside.Search(text);
        _finding = text;
        HideKeyboard();
        ShowFolder(0);
    }

    /// <summary>Saving all of the folder that's shown, or of the game at its top.</summary>
    private void ShowMore()
    {
        var menu = new PopupMenu(this, _more, GravityFlags.End);
        var items = menu.Menu!;
        items.Add(0, 1, 0, _folder == 0 ? "Save all of it to…" : "Save this folder to…");
        menu.MenuItemClick += (_, _) => SaveTo(_folder);
        menu.Show();
    }

    /// <summary>What it is, the way its kind is shown: a picture, text with its lines numbered, bytes, or what a folder's made of, with Save to….</summary>
    private void Preview(int index)
    {
        var p = Look.P;
        var node = Inside.Nodes[index];
        _previewing = index;
        _filled = (-1, false);
        var body = Sheet.Body(this);
        var top = Look.Row(this);
        var name = Look.Text(this, node.Name, 19, p.Text, bold: true);
        name.SetTextIsSelectable(true);
        top.AddView(name, Look.Stretch(true));
        _bytesButton = null;
        if (!node.Folder)
        {
            _bytesButton = Look.Button(this, _asBytes ? "As it is" : "As bytes", Style.Ghost, ToggleBytes);
            Show(_bytesButton, ShownOtherwise(node));
            top.AddView(_bytesButton, Look.Wrap(Look.Dp(8)));
        }
        body.AddView(top, Look.Fill());
        _previewChips = Look.Text(this, "Reading it…", 14, p.SubText);
        body.AddView(_previewChips, Look.Fill(0, Look.Dp(2)));
        _previewBody = Look.Column(this);
        body.AddView(_previewBody, Look.Fill(0, Look.Dp(12)));
        var sheet = Sheet.Show(this, body, ("Close", Style.Secondary, null), ("Save to…", Style.Primary, () => SaveTo(index)));
        _sheet = sheet;
        sheet.SetOnDismissListener(new Dismissed(() =>
        {
            // Only if it's still the one open: another may have opened since.
            if (_sheet != sheet)
                return;
            _sheet = null;
            _previewBody = null;
            _previewChips = null;
            _bytesButton = null;
            _previewing = -1;
        }));
        _reader.Want(index, _asBytes && !node.Folder);
        FillPreview();
    }

    private void ToggleBytes()
    {
        if (_previewing < 0)
            return;
        _asBytes = !_asBytes;
        if (_bytesButton is not null)
            Look.Label(_bytesButton, _asBytes ? "As it is" : "As bytes", 0);
        _filled = (-1, false);
        if (_previewChips is not null)
            _previewChips.Text = "Reading it…";
        _previewBody?.RemoveAllViews();
        _reader.Want(_previewing, _asBytes);
        FillPreview();
    }

    private void FillPreview()
    {
        if (_previewBody is null || _previewChips is null || _previewing < 0)
            return;
        bool bytes = _asBytes && !Inside.Nodes[_previewing].Folder;
        if (_reader.PreviewOf != _previewing || _reader.PreviewAsBytes != bytes || _reader.Preview is not { } preview || _filled == (_previewing, bytes))
            return;
        _filled = (_previewing, bytes);
        var p = Look.P;
        // What it is may only be known now it's been read.
        if (_bytesButton is not null)
            Show(_bytesButton, ShownOtherwise(Inside.Nodes[_previewing]));
        _previewChips.Text = string.Join("  ·  ", preview.Chips);
        _previewBody.RemoveAllViews();

        if (preview.Picture is { } data && Decode(data) is { } picture)
        {
            // As big as it is, or fitted to the sheet.
            var image = new ImageView(this);
            image.SetImageBitmap(picture);
            image.SetAdjustViewBounds(true);
            image.SetScaleType(ImageView.ScaleType.FitCenter);
            image.SetMaxHeight(Look.Dp(preview.Kind == "Folder" ? 200 : 360));
            _previewBody.AddView(image, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WrapContent, ViewGroup.LayoutParams.WrapContent)
            {
                Gravity = GravityFlags.CenterHorizontal, BottomMargin = Look.Dp(12),
            });
        }
        if (preview.Kind == "Folder")
        {
            foreach (var (kind, files, total) in preview.Breakdown)
            {
                var line = Look.Row(this);
                line.AddView(Look.Text(this, kind, 14, p.Text), Look.Stretch(true));
                line.AddView(Look.Text(this, files == 1 ? "1 file" : $"{GameInside.WithCommas(files)} files", 14, p.SubText), Look.Wrap(Look.Dp(12)));
                var size = Look.Text(this, ProgressReporter.Format(total), 14, p.SubText);
                size.Gravity = GravityFlags.End;
                line.AddView(size, new LinearLayout.LayoutParams(Look.Dp(84), ViewGroup.LayoutParams.WrapContent));
                _previewBody.AddView(line, Look.Fill(0, 0, 0, Look.Dp(6)));
            }
            return;
        }

        // Text with its lines numbered, or bytes, as many a line as fit: a line at a time, never wrapped.
        List<string> lines;
        string? more = null;
        if (preview.Kind == "Text")
        {
            int digits = preview.Lines.Count.ToString().Length;
            lines = preview.Lines.Take(MostLines).Select((line, i) => (i + 1).ToString().PadLeft(digits) + "  " + line).ToList();
            int all = Math.Max(preview.TotalLines, preview.Lines.Count);
            if (all > lines.Count)
                more = $"The first {GameInside.WithCommas(lines.Count)} lines of {GameInside.WithCommas(all)}";
        }
        else if (preview.Raw is { } raw)
        {
            int perLine = Resources!.Configuration!.ScreenWidthDp >= 600 ? 16 : 8;
            lines = GameInside.ByteLines(raw, perLine, raw.Length).Take(MostLines).ToList();
            if (lines.Count * perLine < raw.Length)
                more = $"The first {ProgressReporter.Format(lines.Count * perLine)}";
        }
        else
        {
            return;
        }
        var text = Look.Text(this, string.Join("\n", lines), 12, p.Text);
        text.Typeface = Typeface.Monospace;
        text.SetTextIsSelectable(true);
        var across = new HorizontalScrollView(this);
        across.AddView(text);
        _previewBody.AddView(across, Look.Fill());
        if (more is not null)
            _previewBody.AddView(Look.Text(this, more, 13, p.Faint), Look.Fill(0, Look.Dp(8)));
    }

    /// <summary>Text and pictures can be shown as bytes too; anything else is shown as bytes already.</summary>
    private static bool ShownOtherwise(GameInside.Node node) => node.Kind is "Text" or "Picture";

    /// <summary>A picture, made smaller as it's read when it's far bigger than any screen.</summary>
    private static Bitmap? Decode(byte[] data)
    {
        var bounds = new BitmapFactory.Options { InJustDecodeBounds = true };
        BitmapFactory.DecodeByteArray(data, 0, data.Length, bounds);
        int sample = 1;
        while (bounds.OutWidth / sample > 2048 || bounds.OutHeight / sample > 2048)
            sample *= 2;
        return BitmapFactory.DecodeByteArray(data, 0, data.Length, new BitmapFactory.Options { InSampleSize = sample });
    }

    /// <summary>Saving goes where the phone's own picker says, straight into the folder chosen.</summary>
    private void SaveTo(int index)
    {
        if (_reader.Saving)
            return;
        _toSave = index;
        try
        {
            StartActivityForResult(Storage.PickFolder(), SaveCode);
        }
        catch (ActivityNotFoundException)
        {
            Say("This phone has no file picker to choose a folder with.");
        }
    }

    protected override void OnActivityResult(int requestCode, Result resultCode, Intent? data)
    {
        base.OnActivityResult(requestCode, resultCode, data);
        if (requestCode != SaveCode || resultCode != Result.Ok || _toSave < 0)
            return;
        if (Storage.FolderFrom(data) is { } folder)
            _reader.Save(_toSave, folder, chosenFolder: true);
        else
            Say("Couldn't use that folder. Choose one on the phone's storage or memory card.");
        _toSave = -1;
    }

    private void RefreshStatus()
    {
        var p = Look.P;
        string text = "";
        Color color = p.Text;
        if (_reader.Saving)
        {
            text = _reader.SavingFiles <= 1 ? $"Saving it  ·  {ProgressReporter.Format(_reader.SavedBytes)} of {ProgressReporter.Format(_reader.SavingBytes)}"
                : $"Saving {GameInside.WithCommas(Math.Min(_reader.SavedFiles + 1, _reader.SavingFiles))} of {GameInside.WithCommas(_reader.SavingFiles)} files";
        }
        else if (_reader.SaveProblem is { } problem)
        {
            (text, color) = (problem, p.Warn);
        }
        else if (_reader.SavedTo is { } saved)
        {
            (text, color) = ($"Saved to {Storage.Friendly(saved)}", p.Good);
        }
        _status.Text = text;
        _status.SetTextColor(color);
        Show(_stop, _reader.Saving);
        UpdateBack();
        bool shown = text.Length > 0;
        if (shown != (_statusRow.Visibility == ViewStates.Visible))
        {
            Show(_statusRow, shown);
            _statusRow.RequestApplyInsets();
        }
    }

    private void Say(string message) =>
        new AlertDialog.Builder(this).SetMessage(message)!.SetPositiveButton("OK", (_, _) => { })!.Show();

    private sealed class Dismissed(Action dismissed) : Java.Lang.Object, IDialogInterfaceOnDismissListener
    {
        public void OnDismiss(IDialogInterface? dialog) => dismissed();
    }

    /// <summary>What's in the folder shown: folders first, as the game has them.</summary>
    private sealed class EntryAdapter(InsideActivity screen) : BaseAdapter
    {
        private GameInside? _inside;
        private bool _found;

        public List<int> Shown { get; private set; } = [];

        public void Set(GameInside inside, List<int> shown, bool found)
        {
            _inside = inside;
            Shown = shown;
            _found = found;
            NotifyDataSetChanged();
        }

        public override int Count => Shown.Count;
        public override Java.Lang.Object? GetItem(int position) => null;
        public override long GetItemId(int position) => position;

        public override View GetView(int position, View? convertView, ViewGroup? parent)
        {
            var holder = convertView as EntryHolder ?? new EntryHolder(screen);
            var node = _inside!.Nodes[Shown[position]];
            // What Find found says where it is, as it can be anywhere.
            holder.Fill(node, _found && node.Parent > 0 ? _inside.PathOf(node.Parent) : "");
            return holder;
        }
    }

    private sealed class EntryHolder : LinearLayout
    {
        private readonly ImageView _icon;
        private readonly TextView _name, _detail;

        public EntryHolder(Context context) : base(context)
        {
            var p = Look.P;
            Orientation = Android.Widget.Orientation.Horizontal;
            SetGravity(GravityFlags.CenterVertical);
            SetPadding(Look.Dp(12), Look.Dp(10), Look.Dp(12), Look.Dp(10));
            Background = new RippleDrawable(ColorStateList.ValueOf(p.Hover), null, Look.Rounded(Color.White, 12));
            _icon = new ImageView(context);
            _icon.SetScaleType(ImageView.ScaleType.Center);
            AddView(_icon, new LayoutParams(Look.Dp(24), Look.Dp(24)) { RightMargin = Look.Dp(16) });
            var words = Look.Column(context);
            _name = Look.Text(context, "", 15, p.Text);
            _name.SetSingleLine(true);
            _name.Ellipsize = TextUtils.TruncateAt.Middle;
            words.AddView(_name, Look.Fill());
            _detail = Look.Text(context, "", 13, p.SubText);
            _detail.SetSingleLine(true);
            _detail.Ellipsize = TextUtils.TruncateAt.Middle;
            words.AddView(_detail, Look.Fill(0, Look.Dp(2)));
            AddView(words, new LayoutParams(0, ViewGroup.LayoutParams.WrapContent, 1));
        }

        public void Fill(GameInside.Node node, string where)
        {
            var p = Look.P;
            _icon.SetImageResource(node.Folder ? Resource.Drawable.ic_folder : Resource.Drawable.ic_file);
            _icon.ImageTintList = ColorStateList.ValueOf(node.Folder ? p.Accent : p.SubText);
            _name.Text = node.Name;
            string about = node.Folder ? GameInside.ItemsWord(node.Items) : $"{ProgressReporter.Format(node.Size)}  ·  {node.Kind.ToLowerInvariant()}";
            _detail.Text = where.Length > 0 ? $"{about}  ·  {where}" : about;
        }
    }
}
