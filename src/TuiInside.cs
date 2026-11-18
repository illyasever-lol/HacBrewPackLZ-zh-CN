namespace HacBrewPackLz;

/// <summary>
/// Details and Look inside on the home screen, as on the Switch: Enter on a game opens its Details, everything there is to know about it with
/// what can be done to it, and L there looks inside it, a folder at a time beside what the file under the pointer is. A terminal can't show
/// pictures, so a picture says how big it is, and S saves it to be seen with anything else.
/// </summary>
internal sealed partial class Tui
{
    /// <summary>The game Details and Look inside are on, by its key, and its reader, opened when Details opens.</summary>
    private string? _insideKey;
    private InsideReader? _reader;
    private int _detailsScroll;
    /// <summary>Where Look inside is: the folder shown, or what Find found, and the pointer in it.</summary>
    private int _insideFolder;
    private List<int>? _insideFound;
    private string _insideFinding = "";
    private int _insideSelected;
    private int _insideFirst;
    private readonly Stack<(int folder, int selected)> _insideAbove = new();
    private bool _insideBytes;
    /// <summary>Look inside was opened from Details, which Esc goes back to, rather than from the list.</summary>
    private bool _insideFromDetails;
    private int _previewScroll;

    /// <summary>Opens the Details of a game on the list, or what's wrong with a file that can't be used.</summary>
    private void OpenDetails(string key)
    {
        CloseInside();
        _insideKey = key;
        _detailsScroll = 0;
        _view = View.Details;
        var row = _session.Snapshot().Rows.FirstOrDefault(r => r.Key == key);
        // A folder is a game's files already, and one that can't be used is a message: neither has anything to open.
        if (row is not null && !row.IsFolder)
        {
            _reader = new InsideReader(row.Name, row.Files, _session.Keys);
            _reader.Changed += () => _dirty = true;
        }
    }

    private void CloseInside()
    {
        _reader?.CloseInBackground();
        _reader = null;
        _insideKey = null;
        _insideFolder = 0;
        _insideFound = null;
        _insideAbove.Clear();
        _insideSelected = _insideFirst = 0;
        _insideBytes = false;
        _previewScroll = 0;
    }

    private void LeaveDetails()
    {
        CloseInside();
        _view = View.Titles;
    }

    // ---------------------------------------------------------------------------------------------------------------
    // Details

    private List<string> DrawDetails(SessionSnapshot s, int width, int height)
    {
        var row = s.Rows.FirstOrDefault(r => r.Key == _insideKey);
        if (row is null)
        {
            // A file that can't be used, or one that's gone from the list: what About it said.
            _aboutKey = _insideKey;
            return DrawAbout(s, width, height);
        }
        var rows = new List<string>();
        Header(rows, width, "详细信息", Sgr.Dim);
        int inner = width - Margin * 2;
        // The game, and the one figure that decides, with a bar for what it saves.
        var inside = _reader?.Inside;
        string publisher = inside?.Publisher ?? "";
        var top = new Line(width).To(Margin).Add(Terminal.Fit(row.Name, Math.Max(inner - Terminal.DisplayWidth(publisher) - 3, 10)), Sgr.Strong);
        if (publisher.Length > 0)
            top.Right(publisher, Sgr.Dim);
        rows.Add(top.ToString());
        var figure = GameDetails.Figure(row, Compressing);
        var said = new Line(width).To(Margin).Add(figure.Label + " ", Sgr.Dim).Add(figure.Big, Sgr.AccentStrong);
        if (figure.Sub.Length > 0)
            said.Add(", " + figure.Sub, Sgr.Dim);
        if (figure.Share is { } comesTo && comesTo < 1 && inner >= 70)
        {
            double saved = 1 - comesTo;
            string percent = $"{saved * 100:0}%";
            int bar = Math.Min(16, inner / 6);
            int at = width - Margin - bar;
            if (said.Used + percent.Length + 4 < at)
                said.To(at - percent.Length - 1).Add(percent, Sgr.GoodStrong).To(at).AddBar(bar, saved, Sgr.Saving(saved));
        }
        rows.Add(said.ToString());
        rows.Add("");

        // The facts, in the same four places as everywhere, each label in a column as wide as the longest. A wide window has them in two
        // columns, as the Switch's Details has them beside the game, so all of them show at once; what's good to know goes across below.
        string? problem = _reader?.Problem;
        var sections = GameDetails.Sections(row, inside, problem, Compressing);
        int labels = sections.SelectMany(x => x.Lines).Max(l => Terminal.DisplayWidth(l.Label)) + 3;
        const int Gap = 4;
        bool two = inner >= 96 && sections.Count > 2;
        int column = two ? (inner - Gap) / 2 : inner;
        List<List<(string text, string style)>> Facts(IEnumerable<DetailsSection> these)
        {
            var facts = new List<List<(string text, string style)>>();
            foreach (var section in these)
            {
                if (facts.Count > 0)
                    facts.Add([]);
                facts.Add([(section.Title, Sgr.Strong)]);
                foreach (var fact in section.Lines)
                {
                    string style = fact.Bad ? Sgr.Bad : fact.Good ? Sgr.Good : "";
                    int room = Math.Max(column - labels - 2, 10);
                    bool together = fact.Note.Length > 0 && Terminal.DisplayWidth(fact.Value + "   " + fact.Note) <= room;
                    var values = Terminal.Wrap(together ? $"{fact.Value}   {fact.Note}" : fact.Value, room);
                    for (int i = 0; i < values.Count; i++)
                        facts.Add(i == 0 ? [("  " + fact.Label.PadRight(labels), Sgr.Dim), (values[i], style)] : [(new string(' ', labels + 2) + values[i], style)]);
                    if (!together && fact.Note.Length > 0)
                    {
                        foreach (string note in Terminal.Wrap(fact.Note, room))
                            facts.Add([(new string(' ', labels + 2) + note, Sgr.Faint)]);
                    }
                }
            }
            return facts;
        }
        var left = Facts(two ? sections.Take(2) : sections);
        var right = two ? Facts(sections.Skip(2)) : [];
        var body = new List<(List<(string text, string style)> left, List<(string text, string style)> right)>();
        for (int i = 0; i < Math.Max(left.Count, right.Count); i++)
            body.Add((i < left.Count ? left[i] : [], i < right.Count ? right[i] : []));
        var notes = GameDetails.Notes(row);
        if (notes.Count > 0)
        {
            body.Add(([], []));
            body.Add(([("Good to know", Sgr.Strong)], []));
            foreach (string note in notes)
            {
                foreach (string piece in Terminal.Wrap(note, inner - 2))
                    body.Add(([("  " + piece, Sgr.Dim)], []));
            }
        }

        var hints = HintRows(width, 1, DetailsHints(row, scrolls: false));
        int footer = height - 3 - hints.Count;
        int visible = Math.Max(footer - rows.Count, 1);
        // When there's more than fits, it says how to see the rest.
        if (body.Count > visible)
            hints = HintRows(width, 1, DetailsHints(row, scrolls: true));
        _detailsScroll = Math.Clamp(_detailsScroll, 0, Math.Max(body.Count - visible, 0));
        foreach (var (l, r) in body.Skip(_detailsScroll).Take(visible))
        {
            var line = new Line(width).To(Margin);
            foreach (var (text, style) in l)
                line.Add(text, style);
            if (r.Count > 0)
            {
                line.To(Margin + column + Gap);
                foreach (var (text, style) in r)
                    line.Add(text, style);
            }
            rows.Add(line.ToString());
        }
        Pad(rows, footer);
        rows.Add(RuleRow(width));
        _promptRow = rows.Count;
        rows.Add(_message is { } message ? new Line(width).To(Margin).Add(message, Sgr.Warn).ToString() : "");
        rows.AddRange(hints);
        rows.Add("");
        return rows;
    }

    /// <summary>What can be done to this game from its Details: what L, E, V and C do on the list to the game the pointer's on.</summary>
    private (string key, string label)[] DetailsHints(TitleRow row, bool scrolls)
    {
        var hints = new List<(string key, string label)>();
        if (_reader is { Inside: not null })
            hints.Add(("L", "查看内部"));
        if (!row.IsFolder)
            hints.AddRange([("E", "Extract"), ("V", "Check for damage")]);
        if (!row.IsDone(BatchMode.Convert) && !(Compressing && row.IsCompressed))
            hints.Add(("C", $"{CompressLabel()} this game"));
        if (scrolls)
            hints.Add(("Up/Down", "更多"));
        hints.Add(("Esc", "返回"));
        return hints.ToArray();
    }

    private void HandleDetailsKey(ConsoleKeyInfo key)
    {
        var snapshot = _session.Snapshot();
        var row = snapshot.Rows.FirstOrDefault(r => r.Key == _insideKey);
        _message = null;
        switch (key.Key)
        {
            case ConsoleKey.UpArrow:
                _detailsScroll--;
                break;
            case ConsoleKey.DownArrow:
                _detailsScroll++;
                break;
            case ConsoleKey.PageUp:
                _detailsScroll -= 10;
                break;
            case ConsoleKey.PageDown or ConsoleKey.Spacebar:
                _detailsScroll += 10;
                break;
            case ConsoleKey.L when _reader is { Inside: not null }:
                OpenInside(fromDetails: true);
                break;
            case ConsoleKey.E or ConsoleKey.V or ConsoleKey.C when row is not null && !Blocked(snapshot):
                // As on the list, to this game alone, whose job then shows on the list.
                string only = row.Key;
                LeaveDetails();
                DoToOne(key.Key, only);
                break;
            case ConsoleKey.Escape or ConsoleKey.Backspace or ConsoleKey.Enter or ConsoleKey.I:
                LeaveDetails();
                break;
            case ConsoleKey.Q:
                _quit = true;
                break;
        }
    }

    /// <summary>Extract, check or compress the one game, as E, V and C do, from the list or its Details.</summary>
    private void DoToOne(ConsoleKey key, string only)
    {
        bool started = key switch
        {
            ConsoleKey.E => _session.Extract(only),
            ConsoleKey.V => _session.Check(only),
            _ => _session.Compress(only),
        };
        if (started)
            _jobSelected = _jobFirst = 0;
        else
            _message = key switch
            {
                ConsoleKey.E => "It's extracted already, or it's a game's folders, which are extracted as they are.",
                ConsoleKey.V => "A game's folders have nothing packed in them to check.",
                _ => "It's compressed already.",
            };
    }

    // ---------------------------------------------------------------------------------------------------------------
    // Look inside

    private void OpenInside(bool fromDetails)
    {
        _insideFromDetails = fromDetails;
        _insideFolder = 0;
        _insideFound = null;
        _insideAbove.Clear();
        _insideSelected = _insideFirst = 0;
        _previewScroll = 0;
        _view = View.Inside;
    }

    /// <summary>What the list shows: what's in the folder, or what Find found.</summary>
    private List<int> InsideShown(GameInside inside) => _insideFound ?? inside.Nodes[_insideFolder].Children;

    private List<string> DrawInside(int width, int height)
    {
        var rows = new List<string>();
        var inside = _reader?.Inside;
        if (inside is null)
        {
            Header(rows, width, "查看内部", Sgr.Dim);
            rows.Add(new Line(width).To(Margin).Add(_reader?.Problem ?? "Opening it" + Terminal.Ellipsis, _reader?.Problem is null ? Sgr.Dim : Sgr.Bad).ToString());
            Pad(rows, height - 2);
            rows.Add(Hints(width, ("Esc", "返回")));
            return rows;
        }
        string where = _insideFound is not null ? $"Found {GameInside.WithCommas(_insideFound.Count)} named \"{_insideFinding}\""
            : _insideFolder == 0 ? $"{GameInside.WithCommas(inside.Files)} files, {ProgressReporter.Format(inside.Bytes)}" : inside.PathOf(_insideFolder).Replace("/", " › ");
        Header(rows, width, Terminal.Fit(where, Math.Max(width / 2, 10)), Sgr.Dim);
        rows[1] = new Line(width).To(Margin).Add(Terminal.Fit(inside.Name, Math.Max(width / 2 - Margin * 2, 10)), Sgr.Strong).Right(Terminal.Fit(where, Math.Max(width / 2 - 2, 10)), Sgr.Dim).ToString();

        var hints = HintRows(width, 1, InsideHints());
        int bottom = height - 4 - hints.Count;
        int left = Math.Clamp(width * 9 / 20, 30, 60);
        int previewWidth = width - left - Margin - 3;
        rows.Add(new Line(width).To(Margin).Add(new string(Terminal.Rule, left - Margin) + "┬" + new string(Terminal.Rule, Math.Max(width - left - Margin - 1, 0)), Sgr.Faint).ToString());

        var shown = InsideShown(inside);
        _insideSelected = Math.Clamp(_insideSelected, 0, Math.Max(shown.Count - 1, 0));
        int visible = Math.Max(bottom - rows.Count, 1);
        _insideFirst = KeepInView(_insideFirst, _insideSelected, visible, shown.Count);
        int selected = shown.Count > 0 ? shown[_insideSelected] : -1;
        if (selected >= 0)
            _reader!.Want(selected, _insideBytes);
        var preview = PreviewLines(_reader!, selected, previewWidth, visible);
        for (int i = 0; i < visible; i++)
        {
            var line = new Line(width);
            int at = _insideFirst + i;
            if (at < shown.Count)
            {
                var node = inside.Nodes[shown[at]];
                bool chosen = at == _insideSelected;
                string size = node.Folder ? GameInside.ItemsWord(node.Items) : ProgressReporter.Format(node.Size);
                string kind = node.Folder ? "" : node.Kind.ToLowerInvariant();
                int nameRoom = left - Margin - 2 - 11 - 9;
                // What Find found says where it is, as it can be anywhere.
                string name = (node.Folder ? node.Name + "/" : node.Name) + (_insideFound is not null ? "  " + inside.PathOf(inside.Nodes[shown[at]].Parent) : "");
                line.To(Margin).Add(chosen ? Terminal.Pointer.ToString() : " ", Sgr.Accent).Add(" ")
                    .Add(Terminal.Fit(name, nameRoom), chosen ? Sgr.Strong : node.Folder ? "" : "")
                    .To(left - 9 - 11).Add(size.PadLeft(10), Sgr.Dim).Add(" ").Add(kind, Sgr.Faint);
            }
            line.To(left).Add("│", Sgr.Faint);
            if (i < preview.Count)
                line.Add("  ").Add(preview[i].text, preview[i].style);
            rows.Add(line.ToString());
        }
        rows.Add(new Line(width).To(Margin).Add(new string(Terminal.Rule, left - Margin) + "┴" + new string(Terminal.Rule, Math.Max(width - left - Margin - 1, 0)), Sgr.Faint).ToString());
        _promptRow = rows.Count;
        rows.Add(InsideStatus(width));
        rows.Add("");
        rows.AddRange(hints);
        rows.Add("");
        return rows;
    }

    /// <summary>The line above the keys: what's being typed to find, how saving is going, or where it went.</summary>
    private string InsideStatus(int width)
    {
        var line = new Line(width).To(Margin);
        if (_prompt == Prompt.Find)
            return line.Add(PromptQuestion(), Sgr.Strong).Add("> ", Sgr.Accent).Add(_input).ToString();
        var reader = _reader!;
        if (reader.Saving)
        {
            _spinning = true;
            string what = reader.SavingFiles <= 1 ? "Saving it" : $"Saving {GameInside.WithCommas(Math.Min(reader.SavedFiles + 1, reader.SavingFiles))} of {GameInside.WithCommas(reader.SavingFiles)} files";
            line.Add("|/-\\"[(int)(_spinnerFrame % 4)] + " " + what, Sgr.Accent);
            if (reader.SavingBytes > 0)
                line.Add($"   {ProgressReporter.Format(reader.SavedBytes)} of {ProgressReporter.Format(reader.SavingBytes)}", Sgr.Dim);
            return line.ToString();
        }
        if (_message is { } message)
            return line.Add(message, Sgr.Warn).ToString();
        if (reader.SaveProblem is { } problem)
            return line.Add(problem, Sgr.Warn).ToString();
        if (reader.SavedTo is { } saved)
            return line.Add("Saved to ", Sgr.Good).Add(Terminal.Fit(saved, width - Margin * 2 - 9), Sgr.Dim).ToString();
        return "";
    }

    /// <summary>What the file or folder under the pointer is, the way its kind is shown, fitted to the room beside the list.</summary>
    private List<(string text, string style)> PreviewLines(InsideReader reader, int index, int width, int rows)
    {
        var lines = new List<(string text, string style)>();
        if (index < 0 || width < 10)
            return lines;
        if (reader.PreviewOf != index || reader.PreviewAsBytes != _insideBytes || reader.Preview is not { } preview)
        {
            _spinning = true;
            lines.Add(("|/-\\"[(int)(_spinnerFrame % 4)] + " Reading it" + Terminal.Ellipsis, Sgr.Dim));
            return lines;
        }
        foreach (string chips in Terminal.Wrap(string.Join("  ·  ", preview.Chips), width))
            lines.Add((chips, Sgr.Dim));
        lines.Add(("", ""));
        var body = new List<(string text, string style)>();
        if (preview.Kind == "Folder")
        {
            foreach (var (kind, files, bytes) in preview.Breakdown)
                body.Add(($"{kind,-10} {(files == 1 ? "1 file" : $"{GameInside.WithCommas(files)} files"),12}  {ProgressReporter.Format(bytes),10}", ""));
        }
        else if (preview.Picture is not null)
        {
            body.Add(("A picture, which a terminal can't show.", ""));
            body.Add(("S saves it, to see it with anything else.", Sgr.Dim));
        }
        else if (preview.Kind == "Text")
        {
            int number = preview.Lines.Count.ToString().Length;
            for (int i = 0; i < preview.Lines.Count; i++)
                body.Add(((i + 1).ToString().PadLeft(number) + "  " + Terminal.Fit(preview.Lines[i], width - number - 2), ""));
        }
        else if (preview.Raw is { } raw)
        {
            // As many bytes a line as fit beside the list: 16, or 8 in a narrower window.
            int perLine = width >= 8 + 2 + 16 * 3 + 1 + 16 ? 16 : 8;
            foreach (string line in GameInside.ByteLines(raw, perLine, raw.Length))
                body.Add((line, ""));
        }
        _previewScroll = Math.Clamp(_previewScroll, 0, Math.Max(body.Count - (rows - lines.Count), 0));
        lines.AddRange(body.Skip(_previewScroll));
        return lines;
    }

    private (string key, string label)[] InsideHints()
    {
        if (_prompt == Prompt.Find)
            return [("Enter", "Find"), ("Esc", "取消")];
        if (_reader is { Saving: true })
            return [("Esc", "Stop saving")];
        if (_reader?.Inside is null)
            return [("Esc", "返回")];
        return [("Enter", "打开"), ("S", "保存"), ("/", "Find"), ("B", _insideBytes ? "As it is" : "As bytes"), ("Esc", "返回")];
    }

    private void HandleInsideKey(ConsoleKeyInfo key)
    {
        var reader = _reader;
        var inside = reader?.Inside;
        _message = null;
        if (reader is null || inside is null)
        {
            if (key.Key is ConsoleKey.Escape or ConsoleKey.Backspace)
                LeaveInside();
            return;
        }
        if (reader.Saving)
        {
            if (key.Key == ConsoleKey.Escape)
                reader.StopSaving();
            return;
        }
        var shown = InsideShown(inside);
        int Moved(int to)
        {
            _previewScroll = 0;
            return Math.Clamp(to, 0, Math.Max(shown.Count - 1, 0));
        }
        switch (key.Key)
        {
            case ConsoleKey.UpArrow:
                _insideSelected = Moved(_insideSelected - 1);
                break;
            case ConsoleKey.DownArrow:
                _insideSelected = Moved(_insideSelected + 1);
                break;
            case ConsoleKey.PageUp:
                _insideSelected = Moved(_insideSelected - 10);
                break;
            case ConsoleKey.PageDown:
                _insideSelected = Moved(_insideSelected + 10);
                break;
            case ConsoleKey.Home:
                _insideSelected = Moved(0);
                break;
            case ConsoleKey.End:
                _insideSelected = Moved(shown.Count - 1);
                break;
            // The preview goes up and down too, for a long text.
            case ConsoleKey.OemComma or ConsoleKey.U:
                _previewScroll = Math.Max(_previewScroll - 10, 0);
                break;
            case ConsoleKey.OemPeriod or ConsoleKey.J:
                _previewScroll += 10;
                break;
            case ConsoleKey.Enter or ConsoleKey.RightArrow when shown.Count > 0 && inside.Nodes[shown[_insideSelected]].Folder:
                _insideAbove.Push((_insideFound is null ? _insideFolder : -1, _insideSelected));
                _insideFolder = shown[_insideSelected];
                _insideFound = null;
                _insideSelected = _insideFirst = _previewScroll = 0;
                break;
            case ConsoleKey.LeftArrow or ConsoleKey.Backspace when _insideAbove.Count > 0 || _insideFound is not null || _insideFolder != 0:
                GoUp(inside);
                break;
            case ConsoleKey.S when shown.Count > 0:
            {
                var row = _session.Snapshot().Rows.FirstOrDefault(r => r.Key == _insideKey);
                if (row is not null)
                    reader.Save(shown[_insideSelected], GameInside.FilesFromGamesFor(row.Files[0], _session.Settings.Get("--outdir")));
                break;
            }
            case ConsoleKey.Oem2 or ConsoleKey.Divide or ConsoleKey.F:
                _prompt = Prompt.Find;
                _input = "";
                break;
            case ConsoleKey.B:
                _insideBytes = !_insideBytes;
                _previewScroll = 0;
                break;
            case ConsoleKey.Escape:
                LeaveInside();
                break;
            case ConsoleKey.Q:
                _quit = true;
                break;
        }
    }

    /// <summary>Up to the folder this one's in, or from what Find found back to where it was found from.</summary>
    private void GoUp(GameInside inside)
    {
        _previewScroll = 0;
        if (_insideAbove.Count > 0)
        {
            var (folder, selected) = _insideAbove.Pop();
            if (folder < 0)
            {
                // Back to what Find found, which is found again as it was.
                _insideFound = inside.Search(_insideFinding);
            }
            else
            {
                _insideFound = null;
                _insideFolder = folder;
            }
            _insideSelected = selected;
            return;
        }
        if (_insideFound is not null)
        {
            _insideFound = null;
            _insideSelected = 0;
            return;
        }
        int child = _insideFolder;
        _insideFolder = inside.Nodes[_insideFolder].Parent;
        _insideSelected = Math.Max(inside.Nodes[_insideFolder].Children.IndexOf(child), 0);
    }

    private void LeaveInside()
    {
        if (_insideFromDetails)
            _view = View.Details;
        else
            LeaveDetails();
    }

    /// <summary>Files and folders whose names have what was typed in them, from anywhere in the game, as a folder of their own.</summary>
    private void FindInside(string text)
    {
        var inside = _reader?.Inside;
        if (inside is null || text.Length == 0)
            return;
        var found = inside.Search(text);
        if (found.Count == 0)
        {
            _message = "Nothing in it is called that";
            return;
        }
        if (_insideFound is null)
            _insideAbove.Push((_insideFolder, _insideSelected));
        _insideFound = found;
        _insideFinding = text;
        _insideSelected = _insideFirst = _previewScroll = 0;
    }
}
