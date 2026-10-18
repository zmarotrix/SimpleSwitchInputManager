using System.Windows;

namespace Ssim.Tray;

public partial class App : System.Windows.Application
{
    public static EngineClient Engine { get; } = new();

    private Mutex? _single;
    private EventWaitHandle? _showSignal;
    private MainVm? _vm;
    private TrayIcon? _tray;
    private MainWindow? _window;
    private bool _primed;  // first status after (re)connecting: remember, don't announce

    protected override void OnStartup(StartupEventArgs e)
    {
        base.OnStartup(e);

        // One tray app per user: a second launch just opens the existing window.
        _single = new Mutex(true, @"Local\SSIM.Tray", out bool first);
        _showSignal = new EventWaitHandle(false, EventResetMode.AutoReset, @"Local\SSIM.Tray.Show");
        if (!first)
        {
            _showSignal.Set();
            Shutdown();
            return;
        }
        new Thread(() =>
        {
            while (_showSignal.WaitOne()) Dispatcher.Invoke(ShowWindow);
        }) { IsBackground = true }.Start();

        _vm = new MainVm(Engine);
        _tray = new TrayIcon(_vm, ShowWindow, Quit);

        var known = new Dictionary<string, (int slot, bool low)>();
        Engine.ConnectionChanged += connected => Dispatcher.Invoke(() =>
        {
            _vm.EngineConnected = connected;
            _primed = false;
            if (!connected) _vm.Apply(new System.Text.Json.Nodes.JsonObject());
            _tray.UpdateTooltip();
        });
        Engine.StatusReceived += status => Dispatcher.Invoke(() =>
        {
            _vm.Apply(status);
            _tray.UpdateTooltip();
            NotifyChanges(known);
        });
        Engine.Start();

        if (!e.Args.Contains("--tray")) ShowWindow();
    }

    // Balloon notices for connects, disconnects and low battery.
    private void NotifyChanges(Dictionary<string, (int slot, bool low)> known)
    {
        if (!_primed)
        {
            known.Clear();
            foreach (var c in _vm!.Controllers) known[c.Serial] = (c.Slot, c.Battery is >= 0 and <= 15);
            _primed = true;
            return;
        }
        foreach (var c in _vm!.Controllers)
        {
            bool low = c.Battery is >= 0 and <= 15;
            if (!known.TryGetValue(c.Serial, out var prev))
            {
                _tray!.Notify($"Player {c.Slot} connected", $"{c.Name} · {c.TransportLabel}");
            }
            else if (low && !prev.low)
            {
                _tray!.Notify($"Player {c.Slot} battery low", $"{c.Name} is at {c.Battery}%");
            }
            known[c.Serial] = (c.Slot, low);
        }
        foreach (var gone in known.Keys.Where(k => _vm.Controllers.All(c => c.Serial != k)).ToList())
        {
            _tray!.Notify($"Player {known[gone].slot} disconnected", "");
            known.Remove(gone);
        }
    }

    private void ShowWindow()
    {
        if (_window == null)
        {
            _window = new MainWindow(_vm!);
            _window.Closed += (_, _) => _window = null;
        }
        _window.Show();
        if (_window.WindowState == WindowState.Minimized) _window.WindowState = WindowState.Normal;
        _window.Activate();
    }

    private void Quit()
    {
        _tray?.Dispose();
        Engine.Dispose();
        Shutdown();
    }
}
