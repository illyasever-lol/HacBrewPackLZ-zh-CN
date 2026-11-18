using Android.Content.Res;
using Android.Graphics;
using Android.Text;
using Android.Views;
using Android.Widget;

namespace HacBrewPackLz;

/// <summary>The rows of the list: titles, then what couldn't be added, then notes, as in the Windows GUI.</summary>
internal sealed class TitleAdapter(MainActivity screen) : BaseAdapter
{
    public abstract record Item
    {
        public sealed record Title(TitleRow Row) : Item;
        public sealed record Issue(InputIssue Value) : Item;
        public sealed record Note(string Text) : Item;
    }

    private readonly Dictionary<string, (byte[] data, Bitmap? picture)> _pictures = [];

    /// <summary>Each game is a card, outlined while it's ticked, and lit up while it's pressed. Each row has its own.</summary>
    private static Android.Graphics.Drawables.Drawable Card(bool ticked)
    {
        var p = Look.P;
        var outline = new Color(p.Accent.R, p.Accent.G, p.Accent.B, (byte)0x80);
        return new Android.Graphics.Drawables.RippleDrawable(ColorStateList.ValueOf(p.Hover),
            Look.Rounded(p.Surface, 16, ticked ? outline : p.Surface), Look.Rounded(Color.White, 16));
    }
    private SessionSnapshot? _state;

    public List<Item> Items { get; private set; } = [];

    public void SetData(SessionSnapshot state)
    {
        _state = state;
        var items = new List<Item>();
        items.AddRange(state.Rows.Select(r => new Item.Title(r)));
        items.AddRange(state.Issues.Select(i => new Item.Issue(i)));
        // Below a job's games, once it's over, what to know about what it made.
        items.AddRange((state.Job is { } job ? job.Said : state.Notes).Select(n => new Item.Note(n)));
        Items = items;

        foreach (string key in _pictures.Keys.Where(k => !state.Rows.Any(r => r.Key == k)).ToList())
        {
            _pictures[key].picture?.Recycle();
            _pictures.Remove(key);
        }
        NotifyDataSetChanged();
    }

    public override int Count => Items.Count;
    public override Java.Lang.Object? GetItem(int position) => null;
    public override long GetItemId(int position) => position;
    public override int ViewTypeCount => 3;
    public override int GetItemViewType(int position) => Items[position] switch { Item.Title => 0, Item.Issue => 1, _ => 2 };
    public override bool IsEnabled(int position) => Items[position] is not Item.Note;

    public override View GetView(int position, View? convertView, ViewGroup? parent) => Items[position] switch
    {
        Item.Title(var row) => TitleView(row, convertView as TitleHolder ?? new TitleHolder(screen)),
        Item.Issue(var issue) => IssueView(issue, convertView as TextView ?? NoteText(true)),
        Item.Note note => NoteView(note, convertView as TextView ?? NoteText(false)),
        _ => throw new InvalidOperationException(),
    };

    private View TitleView(TitleRow row, TitleHolder holder)
    {
        var p = Look.P;
        bool busy = _state?.IsBusy == true;
        var job = _state?.Job;
        holder.Row = row;
        // On a job's own view, the card being worked on is the one outlined, and how each is going takes the place of its tick.
        bool outlined = job is null ? row.Selected : row.State == RowState.Working;
        if (holder.Ticked != outlined || holder.Background is null)
        {
            holder.Background = Card(outlined);
            holder.Ticked = outlined;
        }
        holder.Tick.Checked = row.Selected;
        holder.Tick.Enabled = !busy;
        holder.Tick.Visibility = job is null ? ViewStates.Visible : ViewStates.Gone;
        holder.Mark.Visibility = job is null ? ViewStates.Gone : ViewStates.Visible;
        if (job is not null)
        {
            var (mark, markColor) = row.State switch
            {
                RowState.Done => ("\u2713", p.Good),
                RowState.Working => ("\u25CF", p.Accent),
                RowState.Failed => ("\u2715", p.Bad),
                _ when !job.Running && (row.BelowMinimum is not null || row.IsCompressed) => ("\u2013", p.Warn),
                _ => ("\u00B7", p.Faint),
            };
            holder.Mark.Text = mark;
            holder.Mark.SetTextColor(markColor);
        }
        if (Picture(row) is { } picture)
        {
            holder.Icon.SetScaleType(ImageView.ScaleType.CenterCrop);
            holder.Icon.ImageTintList = null;
            holder.Icon.SetImageBitmap(picture);
        }
        else
        {
            holder.Icon.SetScaleType(ImageView.ScaleType.Center);
            holder.Icon.ImageTintList = ColorStateList.ValueOf(p.Faint);
            holder.Icon.SetImageResource(row.IsFolder ? Resource.Drawable.ic_folder : Resource.Drawable.ic_game);
        }
        holder.Name.Text = row.Name;
        bool shown = row.Selected || job is not null;
        holder.Name.SetTextColor(shown ? p.Text : p.Faint);

        // The line below: its size now and after, or how it's getting on, or else what it's made of. The game's sheet
        // has all of it.
        var (after, afterColor, saved, savedColor, isSize) = Result(row);
        // On Done, a copy that knows what it took before reads from that to what it takes now, as one compressed here does.
        bool knownBefore = OnDone && row.State != RowState.Done && row.Freed > 0;
        var line = new SpannableStringBuilder(ProgressReporter.Format(knownBefore ? row.InputBytes + row.Freed : row.InputBytes));
        bool failed = row.State == RowState.Failed && row.Failure is not null;
        string? leftOut = job is { Running: false } && row.State == RowState.Ready ? row.BelowMinimum ?? (row.IsCompressed ? "already compressed" : null) : null;
        // On Done, each game's own line, as that it saves nothing worth it goes without saying there, unless its developer compressed it.
        bool developer = row.IsCompressed && row.Estimate?.ByMakers == true;
        string rest = failed ? Help.PlainReason(row.Failure!, "Tap the game to see what.")
            : knownBefore ? ProgressReporter.Format(row.InputBytes)
            // With no headings to say what a figure is, one that isn't known is said on the line.
            : OnDone && row.State != RowState.Done && row.IsCompressed ? developer ? "Compressed by its developer" : "Freed: not known"
            : after.Length > 0 ? after
            : job is { Running: true } && row.State == RowState.Ready ? "Waiting"
            : leftOut is not null ? $"Left out: {leftOut}"
            : job is not null && row.State == RowState.Ready ? "Not done"
            : row.BelowMinimum is { } why ? why.StartsWith("not worth it") ? "Not worth it, saves too little"
                : why.StartsWith("compressed by its developer") ? "Compressed by its developer"
                : why.StartsWith("already compressed") ? "Already compressed" : "Nothing to gain"
            : row.NotSure ? "Not sure yet, may save more"
            // The phone has room for a few words: which version it is goes on the game's page.
            : Capitalized(row.ShortDetail.Length > 0 ? row.ShortDetail : row.Detail);
        int start = line.Length() + 4;
        line.Append((isSize || knownBefore ? "  \u2192  " : "  \u00B7  ") + rest);
        Color restColor = failed ? p.Bad : after.Length > 0 ? afterColor : OnDone ? developer ? p.Good : p.SubText
            : leftOut is not null || row.BelowMinimum is not null ? p.Warn : job is not null && row.State == RowState.Ready ? p.Faint : p.SubText;
        line.SetSpan(new Android.Text.Style.ForegroundColorSpan(shown ? restColor : p.Faint), start, line.Length(), SpanTypes.ExclusiveExclusive);
        holder.Detail.SetText(line, TextView.BufferType.Spannable);
        holder.Detail.SetTextColor(shown ? p.SubText : p.Faint);
        holder.Saved.Text = saved;
        holder.Saved.SetTextColor(shown ? savedColor : p.Faint);
        holder.Saved.Visibility = saved.Length > 0 ? ViewStates.Visible : ViewStates.Gone;
        // How much smaller as a bar too, as the Switch and the Windows GUI show it, once it's known, and for the game being done, how far
        // it's got.
        long? afterBytes = row.State == RowState.Done ? row.OutputBytes : row.State == RowState.Ready ? row.SureAfterBytes : null;
        double share = afterBytes is { } a && row.InputBytes > 0 ? 1 - (double)a / row.InputBytes : 0;
        bool working = job is not null && row.State == RowState.Working;
        if (working)
            share = _state?.Progress is { Total: > 0 } step ? (double)step.Done / step.Total : 0;
        // On Done, what each game freed says it all, with no bar.
        holder.Bar.Set(share, !shown ? p.Faint : working ? p.Accent : Look.SavingColor(share), row.Key);
        holder.Bar.Visibility = (isSize || working) && share > 0 && !OnDone ? ViewStates.Visible : ViewStates.Gone;
        return holder;
    }

    /// <summary>The list shows Done, where each game says what's become of it.</summary>
    private bool OnDone => _state is { View: GamesShown.Done, Job: null };

    /// <summary>What comes after its size now: the size after, as an arrow to it, or else how it's getting on.</summary>
    private (string after, Color afterColor, string saved, Color savedColor, bool isSize) Result(TitleRow row)
    {
        var p = Look.P;
        // While sending, what the Switch has read of each one is what matters.
        if (_state?.Send is { } send && _state.Activity == SessionActivity.Sending && send.Of(row.Files) is { length: > 0 } sent)
        {
            return sent.read >= sent.length
                ? ("Sent", p.Good, "", p.Text, false)
                : ($"Sending, {sent.read * 100 / sent.length}%", p.Accent, "", p.Text, false);
        }
        switch (row.State)
        {
            case RowState.Working when _state?.Job is not null:
            {
                // The step it's on in plain words, how fast, and how far it's got.
                var step = _state.Progress;
                string doing = step is null ? "Starting" : ProgressReporter.Plain(step.Value.Label)
                    + (step.Value.ShowSizes && step.Value.BytesPerSecond > 0 ? $", {step.Value.BytesPerSecond / 1048576:0} MB/s" : "");
                string percent = step is { Total: > 0 } s ? $"{s.Done * 100 / s.Total}%" : "";
                return (doing, p.Accent, percent, p.Accent, false);
            }
            case RowState.Working:
                return ("Working…", p.Accent, "", p.Text, false);
            case RowState.Failed when _state?.Job?.Kind == SessionActivity.Verifying:
                return ("Damaged", p.Bad, "", p.Text, false);
            case RowState.Failed:
                return ("Failed", p.Bad, "", p.Text, false);
            case RowState.Done when row.DoneAs == BatchMode.Extract:
                return ("Extracted", p.Good, "", p.Text, false);
            case RowState.Done when row.DoneAs == BatchMode.Check:
                return ("Whole", p.Good, "", p.Text, false);
            // On Done, what compressing it freed, as on the Switch.
            case RowState.Done when OnDone:
                return (ProgressReporter.Format(row.OutputBytes), p.Good, row.OutputBytes < row.InputBytes ? $"−{ProgressReporter.Format(row.InputBytes - row.OutputBytes)}" : "", p.Good, true);
            case RowState.Done:
                return (ProgressReporter.Format(row.OutputBytes), p.Good, Smaller(row.InputBytes, row.OutputBytes), p.Good, true);
        }
        // Compressed before it was added: what that freed, when it's known, and nothing about what it would save.
        if (OnDone)
            return ("", p.Text, row.Freed > 0 ? $"−{ProgressReporter.Format(row.Freed)}" : "", p.Good, false);
        if (row.Estimate is not { } estimate)
            return ("", p.Text, "", p.Text, false);
        // A first look can't vouch for it, so there's no figure to give until compressing has measured it. Nor is there for one sure to save
        // nothing, such as what's compressed already: the line says why instead.
        if (row.NotSure || !estimate.HasFigure(screen.Compressing))
            return ("", p.Text, "", p.Text, false);
        // What it's sure to save, and what it comes to with that, so the two add up.
        string approximately = estimate.MarginBytes > 0 ? "~" : "";
        return (approximately + ProgressReporter.Format(estimate.MostBytes), p.Text, Smaller(estimate.InputBytes, estimate.MostBytes),
            row.BelowMinimum is not null ? p.Warn : estimate.SavedBytes >= 0 ? p.Good : p.Warn, true);
    }

    /// <summary>A game's picture at the size the list shows it, made the first time it's needed. The game's sheet uses it too.</summary>
    internal Bitmap? Picture(TitleRow row)
    {
        if (row.Icon is not { } data)
            return null;
        if (_pictures.TryGetValue(row.Key, out var known) && known.data == data)
            return known.picture;
        known.picture?.Recycle();
        int size = Look.Dp(48);
        var decoded = BitmapFactory.DecodeByteArray(data, 0, data.Length);
        Bitmap? picture = decoded is null ? null : Bitmap.CreateScaledBitmap(decoded, size, size, true);
        if (decoded is not null && picture != decoded)
            decoded.Recycle();
        _pictures[row.Key] = (data, picture);
        return picture;
    }

    /// <summary>"−71%" for smaller and "+4%" for larger, which reads without a column heading.</summary>
    private static string Smaller(long before, long after)
    {
        if (before <= 0)
            return "";
        double saved = (before - after) * 100.0 / before;
        return saved >= 0 ? $"−{saved:0}%" : $"+{-saved:0}%";
    }

    private static string Capitalized(string text) => text.Length > 0 ? char.ToUpperInvariant(text[0]) + text[1..] : text;

    private TextView NoteText(bool issue)
    {
        var p = Look.P;
        var view = Look.Text(screen, "", 14, issue ? p.Text : p.SubText);
        if (issue)
        {
            // What couldn't be added stands out as a card of its own.
            view.Background = Look.Rounded(p.WarnSurface, 16);
            view.SetPadding(Look.Dp(16), Look.Dp(14), Look.Dp(16), Look.Dp(14));
            // The whole of it is a tap away.
            view.SetMaxLines(3);
            view.Ellipsize = TextUtils.TruncateAt.End;
        }
        else
        {
            view.SetPadding(Look.Dp(8), Look.Dp(6), Look.Dp(8), Look.Dp(6));
        }
        return view;
    }

    private static View IssueView(InputIssue issue, TextView view)
    {
        view.Text = "⚠  " + issue.Message;
        return view;
    }

    private static View NoteView(Item.Note note, TextView view)
    {
        view.Text = note.Text;
        return view;
    }

    /// <summary>One title's row, made once and filled in again for whichever title scrolls into it.</summary>
    private sealed class TitleHolder : LinearLayout
    {
        public readonly CheckBox Tick;
        /// <summary>How it's going on a job's own view, in the tick's place.</summary>
        public readonly TextView Mark;
        public readonly ImageView Icon;
        public readonly TextView Name, Detail, Saved;
        public readonly ShareBar Bar;
        public TitleRow? Row;
        public bool Ticked;

        public TitleHolder(MainActivity screen) : base(screen)
        {
            var p = Look.P;
            Orientation = Android.Widget.Orientation.Horizontal;
            SetGravity(GravityFlags.CenterVertical);
            SetPadding(Look.Dp(6), Look.Dp(14), Look.Dp(16), Look.Dp(14));
            DescendantFocusability = DescendantFocusability.BlockDescendants;

            // The tick box ticks it; tapping anywhere else on the card opens its Details, and holding it down has what can be done to it.
            Tick = new CheckBox(screen) { ButtonTintList = ColorStateList.ValueOf(p.Accent), Focusable = false };
            Tick.Click += (_, _) =>
            {
                if (Row is not null)
                    screen.Toggle(Row);
            };
            AddView(Tick, new LayoutParams(Look.Dp(48), Look.Dp(48)));
            Mark = Look.Text(screen, "", 20, p.Faint, bold: true);
            Mark.Gravity = GravityFlags.Center;
            Mark.Visibility = ViewStates.Gone;
            AddView(Mark, new LayoutParams(Look.Dp(48), Look.Dp(48)));
            // Rounded like the Switch's own icons, with a plain tile until there's a picture.
            Icon = new ImageView(screen) { Background = Look.Rounded(p.Hover, 8), ClipToOutline = true };
            Icon.SetScaleType(ImageView.ScaleType.CenterCrop);
            AddView(Icon, new LayoutParams(Look.Dp(48), Look.Dp(48)) { RightMargin = Look.Dp(12) });

            var words = Look.Column(screen);
            Name = Look.Text(screen, "", 15, p.Text, bold: true);
            Name.SetSingleLine(true);
            Name.Ellipsize = TextUtils.TruncateAt.End;
            words.AddView(Name, Look.Fill());
            // Below the name, across the card: sizes, or how it's getting on, with how much it saves at the end.
            var below = Look.Row(screen);
            Detail = Look.Text(screen, "", 14, p.SubText);
            Detail.SetSingleLine(true);
            Detail.Ellipsize = TextUtils.TruncateAt.End;
            below.AddView(Detail, new LayoutParams(0, ViewGroup.LayoutParams.WrapContent, 1));
            Saved = Look.Text(screen, "", 15, p.Good, bold: true);
            below.AddView(Saved, Look.Wrap(Look.Dp(8)));
            words.AddView(below, Look.Fill(0, Look.Dp(3)));
            Bar = new ShareBar(screen);
            words.AddView(Bar, new LayoutParams(Look.Dp(60), Look.Dp(4)) { Gravity = GravityFlags.End, TopMargin = Look.Dp(6) });
            AddView(words, new LayoutParams(0, ViewGroup.LayoutParams.WrapContent, 1));
        }
    }
}

/// <summary>What a game saves, or how far the game being done has got, coloured from the left on a quiet track, as on the Switch.</summary>
internal sealed class ShareBar(Android.Content.Context context) : View(context)
{
    private readonly Paint _paint = new(PaintFlags.AntiAlias);
    private double _share, _shown;
    private Color _color;
    private string? _key;
    private Android.Animation.ValueAnimator? _glide;

    /// <summary>The bar of a game it showed already glides to its new figure, as on Windows and the Switch. Given another game, as the list
    /// reuses it, it's there at once.</summary>
    public void Set(double share, Color color, string? key = null)
    {
        bool same = key is not null && key == _key;
        if (same && share == _share && color == _color)
            return;
        _key = key;
        _color = color;
        _glide?.Cancel();
        if (same && share != _share && Android.Animation.ValueAnimator.AreAnimatorsEnabled())
        {
            double from = _shown;
            _glide = Look.Move(along =>
            {
                _shown = from + (share - from) * along;
                Invalidate();
            });
            _glide.Start();
        }
        else
        {
            _shown = share;
        }
        _share = share;
        Invalidate();
    }

    protected override void OnDraw(Canvas canvas)
    {
        float radius = Height / 2f;
        _paint.Color = Look.P.Border;
        canvas.DrawRoundRect(0, 0, Width, Height, radius, radius, _paint);
        if (_shown <= 0)
            return;
        _paint.Color = _color;
        canvas.DrawRoundRect(0, 0, Math.Max(Height, (float)(Width * Math.Min(_shown, 1))), Height, radius, radius, _paint);
    }
}
