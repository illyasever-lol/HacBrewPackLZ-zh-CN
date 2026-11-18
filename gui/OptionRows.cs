namespace HacBrewPackLz;

/// <summary>
/// Options as rows, each with its name and what it does on the left and its control on the right, in whatever window has
/// them: the options window has pages of them, and the main window the welcome's few.
/// </summary>
/// <param name="brief">Each option's few words rather than all it says, so every row takes the same room, as on the welcome.</param>
/// <param name="changed">Told of each value taken, as some change which options are shown.</param>
internal sealed class OptionRows(AppWindow window, IFormStore store, Action<OptionSpec>? run = null, Func<OptionSpec, string?>? outcome = null, bool brief = false,
    Action<OptionSpec>? changed = null)
{
    public const int MinimumRowHeight = 58;

    private sealed record Row(OptionSpec Option, Widget[] Controls);

    private readonly List<Row> _rows = [];
    private int[] _tops = [0];
    private int _left;
    private int _right;
    private int _controlX;
    private int _y;
    private string? _errorFlag;

    /// <summary>What was wrong with the last value given, which stays as it was.</summary>
    public string? Error { get; private set; }

    public int Height => _tops[^1];

    private string HelpOf(OptionSpec option) => brief && option.Brief is { } few ? few : option.Help;

    /// <summary>Each option's control, by its flag without the dashes, for the tests.</summary>
    public IEnumerable<(string name, Widget widget)> Named => _rows.Select(r => (r.Option.Flag.TrimStart('-'), r.Controls[0]));

    /// <summary>These options from now on, in place of any before.</summary>
    public void Show(IEnumerable<OptionSpec> options)
    {
        Clear();
        foreach (var option in options)
            _rows.Add(new Row(option, CreateControls(option)));
    }

    public void Clear()
    {
        foreach (var row in _rows)
        {
            foreach (var control in row.Controls)
                window.Remove(control);
        }
        _rows.Clear();
        _tops = [0];
        Error = null;
        _errorFlag = null;
    }

    /// <summary>Works out how tall each row is between these two edges, each as tall as its help text needs.</summary>
    public int Measure(int left, int right)
    {
        _left = left;
        _right = right;
        _controlX = left + (right - left) * 46 / 100;
        _tops = new int[_rows.Count + 1];
        for (int i = 0; i < _rows.Count; i++)
        {
            int help = window.TextHeight(HelpOf(_rows[i].Option), Fonts.Small, _controlX - _left - window.S(16));
            _tops[i + 1] = _tops[i] + Math.Max(window.S(MinimumRowHeight), window.S(10) + window.S(20) + help + window.S(10));
        }
        return Height;
    }

    /// <summary>Puts the controls in place for rows starting at <paramref name="y"/>, cut off outside <paramref name="clip"/>.</summary>
    public void Place(int y, Rect clip)
    {
        _y = y;
        for (int i = 0; i < _rows.Count; i++)
        {
            int rowY = y + _tops[i];
            int controlHeight = window.S(32);
            int controlY = rowY + (_tops[i + 1] - _tops[i] - controlHeight) / 2;
            var controls = _rows[i].Controls;
            foreach (var control in controls)
                control.ClipTo = clip;
            if (_rows[i].Option.Kind == OptionKind.Action && controls[0] is Button action)
            {
                action.Bounds = new Rect(_controlX, controlY, action.PreferredWidth(), controlHeight);
            }
            else if (controls.Length == 2 && controls[1] is Button browse)
            {
                int browseWidth = browse.PreferredWidth();
                browse.Bounds = new Rect(_right - browseWidth, controlY, browseWidth, controlHeight);
                controls[0].Bounds = new Rect(_controlX, controlY, _right - browseWidth - window.S(8) - _controlX, controlHeight);
            }
            else if (_rows[i].Option.Kind == OptionKind.Choice && controls.Length == 2)
            {
                // v18：可编辑下拉 = 自定义输入框 + 预置下拉，输入框占左侧剩余宽度。
                int presetWidth = Math.Min(_right - _controlX, window.S(200));
                controls[1].Bounds = new Rect(_right - presetWidth, controlY, presetWidth, controlHeight);
                controls[0].Bounds = new Rect(_controlX, controlY, Math.Max(window.S(80), _right - presetWidth - window.S(8) - _controlX), controlHeight);
            }
            else if (_rows[i].Option.Kind == OptionKind.Choice)
            {
                // A short list of choices, as wide as they need, so it weighs the same as a switch beside it rather than the whole row.
                controls[0].Bounds = new Rect(_controlX, controlY, Math.Min(_right - _controlX, window.S(180)), controlHeight);
            }
            else
            {
                controls[0].Bounds = new Rect(_controlX, controlY, _right - _controlX, controlHeight);
            }
        }
    }

    /// <summary>The names and help, where the last <see cref="Place"/> put the rows.</summary>
    public void Paint(Gfx g)
    {
        var p = Theme.P;
        int textWidth = _controlX - _left - window.S(16);
        for (int i = 0; i < _rows.Count && i + 1 < _tops.Length; i++)
        {
            int rowY = _y + _tops[i];
            var option = _rows[i].Option;
            g.Text(option.Label, new Rect(_left, rowY + window.S(9), textWidth, window.S(20)), Fonts.Body, p.Text);
            g.Text(HelpOf(option), new Rect(_left, rowY + window.S(29), textWidth, _tops[i + 1] - _tops[i] - window.S(29)), Fonts.Small, p.SubText,
                TextFlags.WordBreak | TextFlags.EditControl | TextFlags.NoPrefix);
            if (option.Kind == OptionKind.Action && outcome?.Invoke(option) is { } said)
            {
                var button = _rows[i].Controls[0].Bounds;
                g.Text(said, new Rect(button.Right + window.S(12), button.Y, _right - button.Right - window.S(12), button.H), Fonts.Small, p.SubText, TextFlags.Line);
            }
        }
    }

    /// <summary>What's been typed into the box that has the focus, which is otherwise only taken on leaving it.</summary>
    public void CommitFocused()
    {
        if (window.Focused is TextField field && _rows.FirstOrDefault(r => r.Controls.Contains(field)) is { } row)
            Commit(row.Option, field.Value.Trim(), field);
    }

    private Widget[] CreateControls(OptionSpec option)
    {
        string value = store.Get(option.Flag);
        switch (option.Kind)
        {
            case OptionKind.Toggle:
                var toggle = new ToggleSwitch(on => Commit(option, option.ToggleValue(on != option.Inverted), null)) { On = option.ToggleOn(value) != option.Inverted };
                return [window.Add(toggle)];
            case OptionKind.Slider:
                var slider = new Slider(option.Min, option.Max, chosen => Commit(option, chosen.ToString() == option.Default ? "" : chosen.ToString(), null))
                {
                    Value = int.TryParse(value.Length > 0 ? value : option.Default, out int number) ? number : option.Min,
                };
                return [window.Add(slider)];
            case OptionKind.Choice when option.Editable:
                var custom = window.Add(new TextField(option.Placeholder));
                custom.Value = value;
                custom.Committed = () => Commit(option, custom.Value.Trim(), custom);
                var preset = window.Add(new Dropdown(option.Choices, chosen =>
                {
                    string chosenValue = chosen == option.Default ? "" : chosen;
                    custom.Value = chosenValue;
                    Commit(option, chosenValue, custom);
                })
                {
                    Value = Array.IndexOf(option.Choices, value) >= 0 ? value : option.Default,
                });
                return [custom, preset];
            case OptionKind.Choice:
                var dropdown = new Dropdown(option.Choices, chosen => Commit(option, chosen == option.Default ? "" : chosen, null))
                {
                    Value = value.Length > 0 ? value : option.Default,
                };
                return [window.Add(dropdown)];
            case OptionKind.Action:
                return [window.Add(new Button(option.Verb, ButtonStyle.Secondary, () =>
                {
                    run?.Invoke(option);
                    window.Invalidate();
                }))];
            case OptionKind.File or OptionKind.Folder:
                var path = window.Add(new TextField(option.Placeholder));
                path.Value = value;
                var browse = window.Add(new Button("浏览", ButtonStyle.Secondary, () => Browse(option, path)));
                path.Committed = () => Commit(option, path.Value.Trim(), path);
                return [path, browse];
            default:
                var text = window.Add(new TextField(option.Placeholder));
                text.Value = value;
                text.Committed = () => Commit(option, text.Value.Trim(), text);
                return [text];
        }
    }

    private void Commit(OptionSpec option, string value, TextField? field)
    {
        string? problem = store.Set(option, value);
        if (problem is null)
            changed?.Invoke(option);
        if (field is not null)
            field.HasError = problem is not null;
        if (problem is not null)
        {
            Error = problem;
            _errorFlag = option.Flag;
        }
        else if (_errorFlag == option.Flag)
        {
            Error = null;
            _errorFlag = null;
        }
        window.Invalidate();
    }

    private void Browse(OptionSpec option, TextField field)
    {
        string? chosen;
        if (option.Kind == OptionKind.Folder)
        {
            chosen = FileDialogs.OpenFolders(window.Handle, $"选择：{option.Label}", multiple: false).FirstOrDefault();
        }
        else if (option.Flag == "--output")
        {
            chosen = FileDialogs.SaveFile(window.Handle, "将 NSP 另存为", "NSP 文件", "*.nsp", "nsp", field.Value);
        }
        else
        {
            bool keys = option.Flag == "--keyset";
            chosen = FileDialogs.OpenFiles(window.Handle, $"选择：{option.Label}", keys ? "密钥文件" : "NSP 文件", keys ? "*.keys;*.txt;*.dat;*.ini" : "*.nsp", multiple: false).FirstOrDefault();
        }
        if (chosen is null)
            return;
        field.Value = chosen;
        Commit(option, chosen, field);
    }
}
