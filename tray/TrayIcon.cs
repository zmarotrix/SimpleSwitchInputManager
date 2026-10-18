using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Text;
using System.Runtime.InteropServices;
using System.Windows.Forms;

namespace Ssim.Tray;

/// <summary>The notification-area icon: tooltip summary, menu and balloon notices.</summary>
public sealed class TrayIcon : IDisposable
{
    private readonly NotifyIcon _icon;
    private readonly MainVm _vm;
    private readonly Action _open;
    private readonly Action _quit;

    public TrayIcon(MainVm vm, Action open, Action quit)
    {
        _vm = vm;
        _open = open;
        _quit = quit;
        _icon = new NotifyIcon
        {
            Icon = CreateIcon(),
            Text = "Simple Switch Input Manager",
            Visible = true,
            ContextMenuStrip = new ContextMenuStrip(),
        };
        _icon.MouseClick += (_, e) =>
        {
            if (e.Button == MouseButtons.Left) _open();
        };
        _icon.ContextMenuStrip.Opening += (_, _) => BuildMenu();
    }

    private void BuildMenu()
    {
        var menu = _icon.ContextMenuStrip!;
        menu.Items.Clear();
        if (!_vm.EngineConnected)
        {
            menu.Items.Add(new ToolStripMenuItem("Engine not running") { Enabled = false });
        }
        else if (_vm.Controllers.Count == 0)
        {
            menu.Items.Add(new ToolStripMenuItem("No controllers connected") { Enabled = false });
        }
        foreach (var c in _vm.Controllers.OrderBy(c => c.Slot))
        {
            var item = new ToolStripMenuItem(c.Summary);
            item.Click += (_, _) => _ = App.Engine.IdentifyAsync(c.Serial);
            item.ToolTipText = "Click to identify (lights + buzz)";
            menu.Items.Add(item);
        }
        menu.Items.Add(new ToolStripSeparator());
        menu.Items.Add("Open Simple Switch Input Manager", null, (_, _) => _open());
        menu.Items.Add("Quit tray icon", null, (_, _) => _quit());
    }

    public void UpdateTooltip()
    {
        string text = _vm.EngineConnected
            ? _vm.Controllers.Count == 0
                ? "SSIM: no controllers"
                : "SSIM: " + string.Join(", ", _vm.Controllers.OrderBy(c => c.Slot).Select(c => $"P{c.Slot} {c.Battery}%"))
            : "SSIM: engine not running";
        _icon.Text = text.Length > 63 ? text[..63] : text;  // NotifyIcon limit
    }

    public void Notify(string title, string text) => _icon.ShowBalloonTip(3000, title, text, ToolTipIcon.None);

    // A simple game-controller glyph from Segoe Fluent Icons / MDL2 Assets.
    private static Icon CreateIcon()
    {
        using var bmp = new Bitmap(32, 32);
        using (var g = Graphics.FromImage(bmp))
        {
            g.SmoothingMode = SmoothingMode.AntiAlias;
            g.TextRenderingHint = TextRenderingHint.AntiAliasGridFit;
            using var bg = new SolidBrush(Color.FromArgb(230, 0, 18));  // a "Switch red" disc
            g.FillEllipse(bg, 1, 1, 30, 30);
            string family = FontFamily.Families.Any(f => f.Name == "Segoe Fluent Icons") ? "Segoe Fluent Icons" : "Segoe MDL2 Assets";
            using var font = new Font(family, 15f, GraphicsUnit.Pixel);
            using var fg = new SolidBrush(Color.White);
            var fmt = new StringFormat { Alignment = StringAlignment.Center, LineAlignment = StringAlignment.Center };
            g.DrawString("", font, fg, new RectangleF(0, 1, 32, 32), fmt);  // Game
        }
        IntPtr h = bmp.GetHicon();
        var icon = (Icon)Icon.FromHandle(h).Clone();
        DestroyIcon(h);
        return icon;
    }

    [DllImport("user32.dll")]
    private static extern bool DestroyIcon(IntPtr handle);

    public void Dispose()
    {
        _icon.Visible = false;
        _icon.Dispose();
    }
}
