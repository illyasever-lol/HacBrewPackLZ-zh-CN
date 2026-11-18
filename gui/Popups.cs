namespace HacBrewPackLz;

/// <summary>A small list that opens below a control, for menus and choices. It closes itself when anything else is clicked.</summary>
internal sealed class PopupList : AppWindow
{
    private readonly (string text, char icon, Action action)[] _items;
    private readonly int _chosen;
    private readonly int _width;
    private int _hover = -1;
    private bool _closing;
    /// <summary>Where it goes on the screen, how far from there it starts, and when it came out, while it's on its way.</summary>
    private readonly POINT _place;
    private readonly int _startsOff;
    private double _since = -1;

    public PopupList(AppWindow owner, Rect below, (string text, char icon, Action action)[] items, int chosen = -1)
    {
        _items = items;
        _chosen = chosen;
        bool moves = Motion.On;
        CreatePopup(owner, moves ? Win32.WS_EX_LAYERED : 0);
        int iconRoom = items.Any(i => i.icon != '\0') ? S(26) : 0;
        _width = Math.Max(below.W, items.Max(i => TextWidth(i.text, Fonts.Body)) + iconRoom + S(40));

        // Below what it's for, or above it when there isn't room below in the window, as for a button along the bottom.
        int height = items.Length * S(ItemHeight) + S(8);
        bool above = below.Bottom + S(4) + height > owner.ClientHeight && below.Y - S(4) - height >= 0;
        var corner = new POINT { X = below.X, Y = above ? below.Y - S(4) - height : below.Bottom + S(4) };
        Win32.ClientToScreen(owner.Handle, ref corner);
        _place = corner;
        if (moves)
        {
            // It starts a little way back towards what opened it, unseen, and the first picture of it starts it moving.
            _startsOff = above ? S(10) : -S(10);
            _since = Motion.Now;
            Win32.SetLayeredWindowAttributes(Handle, 0, 0, Win32.LWA_ALPHA);
        }
        Win32.SetWindowPos(Handle, IntPtr.Zero, corner.X, corner.Y + _startsOff, _width, height, Win32.SWP_NOZORDER | Win32.SWP_NOACTIVATE);
        Win32.ShowWindow(Handle, Win32.SW_SHOW);
        Win32.SetForegroundWindow(Handle);
        Win32.SetFocus(Handle);
    }

    private const int ItemHeight = 32;

    protected override void Layout(int width, int height) { }

    /// <summary>On its way out: as far along as it's got, and as clear, until it's in its place, where it's an ordinary window again.</summary>
    protected override void OnFrame()
    {
        if (_since < 0)
            return;
        double along = Motion.Along(_since, Motion.RiseTime);
        Win32.SetWindowPos(Handle, IntPtr.Zero, _place.X, _place.Y + (int)Math.Round(_startsOff * (1 - along)), 0, 0,
            Win32.SWP_NOSIZE | Win32.SWP_NOZORDER | Win32.SWP_NOACTIVATE);
        if (along < 1)
        {
            Win32.SetLayeredWindowAttributes(Handle, 0, (byte)Math.Round(255 * along), Win32.LWA_ALPHA);
            return;
        }
        _since = -1;
        Win32.SetWindowLongPtrW(Handle, Win32.GWL_EXSTYLE, (IntPtr)((long)Win32.GetWindowLongPtrW(Handle, Win32.GWL_EXSTYLE) & ~(long)Win32.WS_EX_LAYERED));
    }

    protected override void PaintWindow(Gfx g)
    {
        var p = Theme.P;
        if (_since >= 0)
            StillMoving();
        g.Fill(new Rect(0, 0, ClientWidth, ClientHeight), p.StrongBorder);
        g.Fill(new Rect(1, 1, ClientWidth - 2, ClientHeight - 2), p.Surface);
        for (int i = 0; i < _items.Length; i++)
        {
            var row = new Rect(S(4), S(4) + i * S(ItemHeight), ClientWidth - S(8), S(ItemHeight));
            if (i == _hover)
                g.FillRound(row, S(4), p.Hover);
            int x = row.X + S(10);
            if (_items[i].icon != '\0')
            {
                g.Icon(_items[i].icon, new Rect(x, row.Y, S(16), row.H), 14, p.SubText);
                x += S(26);
            }
            g.Text(_items[i].text, new Rect(x, row.Y, row.Right - x - S(8), row.H), i == _chosen ? Fonts.Strong : Fonts.Body, i == _chosen ? p.Accent : p.Text);
        }
    }

    protected override IntPtr OnMessage(uint message, IntPtr wParam, IntPtr lParam)
    {
        switch (message)
        {
            case Win32.WM_MOUSEMOVE:
                int hover = ItemAt(Win32.High(lParam));
                if (hover != _hover)
                {
                    _hover = hover;
                    Invalidate();
                }
                base.OnMessage(message, wParam, lParam);
                return 0;
            case Win32.WM_LBUTTONUP:
                int item = ItemAt(Win32.High(lParam));
                if (item >= 0)
                {
                    var action = _items[item].action;
                    Dismiss();
                    action();
                }
                return 0;
            case Win32.WM_KEYDOWN:
                HandlePopupKey((int)wParam);
                return 0;
            case Win32.WM_ACTIVATE when Win32.Low(wParam) == 0:
                Dismiss();
                return 0;
        }
        return base.OnMessage(message, wParam, lParam);
    }

    private void HandlePopupKey(int key)
    {
        switch (key)
        {
            case Win32.VK_ESCAPE:
                Dismiss();
                break;
            case Win32.VK_DOWN:
                _hover = Math.Min(_hover + 1, _items.Length - 1);
                Invalidate();
                break;
            case Win32.VK_UP:
                _hover = Math.Max(_hover - 1, 0);
                Invalidate();
                break;
            case Win32.VK_RETURN or Win32.VK_SPACE when _hover >= 0:
                var action = _items[_hover].action;
                Dismiss();
                action();
                break;
        }
    }

    private int ItemAt(int y)
    {
        int index = (y - S(4)) / S(ItemHeight);
        return y >= S(4) && index < _items.Length ? index : -1;
    }

    private void Dismiss()
    {
        if (_closing)
            return;
        _closing = true;
        Win32.DestroyWindow(Handle);
    }
}
