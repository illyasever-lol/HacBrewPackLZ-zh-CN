namespace HacBrewPackLz;

/// <summary>A drawn line of text that opens a web address when clicked or chosen with the keyboard.</summary>
internal sealed class LinkLabel(string text, Action<string> open) : Widget
{
    public override bool CanFocus => true;

    public override void Paint(Gfx g)
    {
        var p = Theme.P;
        g.Text(text, Bounds, Fonts.Body, IsHovered ? p.AccentHover : p.Accent, TextFlags.Line);
        int width = Math.Min(Window.TextWidth(text, Fonts.Body), Bounds.W);
        g.Fill(new Rect(Bounds.X, Bounds.Bottom - S(3), width, 1), p.Accent);
    }

    public override void MouseUp(int x, int y)
    {
        if (Bounds.Contains(x, y))
            open(text);
    }

    public override bool KeyDown(int key)
    {
        if (key is not (Win32.VK_RETURN or Win32.VK_SPACE))
            return false;
        open(text);
        return true;
    }
}

/// <summary>The about box, with a clickable link to the translator's page.</summary>
internal sealed class AboutWindow : AppWindow
{
    private const string AuthorUrl = "https://space.bilibili.com/36010";
    private readonly Button _ok;
    private readonly LinkLabel _link;

    private static readonly string[] Paragraphs =
    [
        "构建 NSP 文件，也可把 NSP、NSZ、XCI 和 XCZ 文件转换为 NSP；通过 NCA 压缩层以 LZ4 压缩游戏数据。原始文件不会被修改。",
        "基于 GNU 通用公共许可证第 2 版发布的自由软件。",
        "汉化作者:illyasever",
    ];

    public AboutWindow(AppWindow owner)
    {
        CreateTopLevel($"关于 {AppInfo.Name}", 560, 380, owner);
        _ok = Add(new Button("确定", ButtonStyle.Primary, Close));
        _link = Add(new LinkLabel(AuthorUrl, Explorer.OpenUrl));
        Relayout();
    }

    protected override (int width, int height) MinimumSize => (480, 320);

    protected override bool OnKey(int key)
    {
        if (key == Win32.VK_ESCAPE)
        {
            Close();
            return true;
        }
        return false;
    }

    private int TextTop(int width)
    {
        int room = width - S(48);
        int y = S(24) + S(44);
        foreach (string paragraph in Paragraphs)
            y += Math.Max(TextHeight(paragraph, Fonts.Body, room), S(22)) + S(10);
        return y;
    }

    protected override void Layout(int width, int height)
    {
        // CreateTopLevel 会在构造期间同步派发 WM_SIZE，此时控件字段尚未赋值。
        if (_ok is null || _link is null || width <= 0 || height <= 0)
            return;
        _ok.Bounds = new Rect(width - S(110), height - S(52), S(90), S(34));
        _link.Bounds = new Rect(S(24), Math.Min(TextTop(width), height - S(90)), width - S(48), S(24));
    }

    protected override void PaintWindow(Gfx g)
    {
        if (ClientWidth <= 0)
            return;
        var p = Theme.P;
        int left = S(24);
        int room = ClientWidth - S(48);
        int y = S(24);
        g.Text($"{AppInfo.Name} {AppInfo.Version}", new Rect(left, y, room, S(32)), Fonts.Title, p.Text);
        y += S(44);
        foreach (string paragraph in Paragraphs)
        {
            int lineHeight = Math.Max(TextHeight(paragraph, Fonts.Body, room), S(22));
            g.Text(paragraph, new Rect(left, y, room, lineHeight + S(8)), Fonts.Body, p.SubText, TextFlags.Wrapped);
            y += lineHeight + S(10);
        }
    }
}
