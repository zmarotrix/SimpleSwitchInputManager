using System.ComponentModel;
using System.Diagnostics;
using System.Text.Json.Nodes;
using System.Windows;

namespace Ssim.Tray;

public partial class MainWindow : Window
{
    // Name of the engine's Windows service.
    private const string ServiceName = "Switch2ProPad";

    private bool _loading;

    public MainWindow(MainVm vm)
    {
        InitializeComponent();
        DataContext = vm;
        LoadLocalSettings();
    }

    private void LoadLocalSettings()
    {
        _loading = true;
        AutostartCheck.IsChecked = Autostart.Enabled;
        var steam = SteamInput.Read();
        SteamCheck.IsChecked = steam == true;
        SteamCheck.IsEnabled = SteamApply.IsEnabled = steam != null;
        SteamStatus.Text = steam == null
            ? "Steam not found."
            : SteamInput.IsRunning ? "Applying restarts Steam." : "";
        _loading = false;
    }

    // Closing the window keeps SSIM in the notification area.
    protected override void OnClosing(CancelEventArgs e)
    {
        e.Cancel = true;
        Hide();
    }

    private static T? Item<T>(object sender) where T : class => (sender as FrameworkElement)?.DataContext as T;

    private async void Identify_Click(object sender, RoutedEventArgs e)
    {
        if (Item<ControllerVm>(sender) is { } c) Show(await App.Engine.IdentifyAsync(c.Serial));
    }

    private async void Release_Click(object sender, RoutedEventArgs e)
    {
        if (Item<ControllerVm>(sender) is { } c) Show(await App.Engine.SetControllerAsync(c.Serial, "managed", false));
    }

    private async void Split_Click(object sender, RoutedEventArgs e)
    {
        if (Item<ControllerVm>(sender) is not { } c) return;
        foreach (var id in c.Members) Show(await App.Engine.SetControllerAsync(id, "joyconSingle", true));
    }

    private async void Pair_Click(object sender, RoutedEventArgs e)
    {
        if (Item<ControllerVm>(sender) is not { } c) return;
        // Clear "single" on every lone Joy-Con, so this one has a partner to pair with.
        var vm = (MainVm)DataContext;
        var ids = vm.Controllers.Where(x => x.IsSingleJoyCon).SelectMany(x => x.Members).Union(c.Members).ToList();
        foreach (var id in ids) Show(await App.Engine.SetControllerAsync(id, "joyconSingle", false));
        if (!((MainVm)DataContext).CombineJoyCons)
            MessageBox.Show(this, "Turn on automatic Joy-Con pairing in Settings so it pairs with a Joy-Con of the other side.",
                Title, MessageBoxButton.OK, MessageBoxImage.Information);
    }

    private DetectedVm? SelectedDetected => ((MainVm)DataContext).SelectedDetected;

    private async void Enable_Click(object sender, RoutedEventArgs e)
    {
        if (SelectedDetected is not { } d) return;
        Show(await App.Engine.SetControllerAsync(d.Id, "managed", true));
    }

    private async void SimpleMode_Click(object sender, RoutedEventArgs e)
    {
        if (SelectedDetected is { } d)
            Show(await App.Engine.SendAsync(new JsonObject { ["cmd"] = "switch1Mode", ["id"] = d.Id, ["mode"] = "simple" }));
    }

    private async void FullMode_Click(object sender, RoutedEventArgs e)
    {
        if (SelectedDetected is { } d)
            Show(await App.Engine.SendAsync(new JsonObject { ["cmd"] = "switch1Mode", ["id"] = d.Id, ["mode"] = "full" }));
    }

    private void Close_Click(object sender, RoutedEventArgs e) => Hide();

    private void Show(JsonObject reply)
    {
        if ((bool?)reply["ok"] != true)
            MessageBox.Show(this, (string?)reply["error"] ?? "Something went wrong.", Title, MessageBoxButton.OK, MessageBoxImage.Warning);
    }

    private async void SteamApply_Click(object sender, RoutedEventArgs e)
    {
        bool want = SteamCheck.IsChecked == true;
        if (SteamInput.IsRunning &&
            MessageBox.Show(this, "Steam has to close to change this. Close Steam now and reopen it afterwards?", Title,
                MessageBoxButton.OKCancel, MessageBoxImage.Question) != MessageBoxResult.OK)
        {
            LoadLocalSettings();
            return;
        }
        SteamApply.IsEnabled = false;
        SteamStatus.Text = "Applying…";
        SteamStatus.Text = await SteamInput.WriteAsync(want);
        SteamApply.IsEnabled = true;
    }

    private void Autostart_Changed(object sender, RoutedEventArgs e)
    {
        if (!_loading) Autostart.Enabled = AutostartCheck.IsChecked == true;
    }

    private void RestartEngine_Click(object sender, RoutedEventArgs e)
    {
        try
        {
            // Restarting a service needs administrator rights, so this asks via UAC.
            Process.Start(new ProcessStartInfo("powershell.exe",
                $"-NoProfile -WindowStyle Hidden -Command Restart-Service {ServiceName}")
            {
                UseShellExecute = true,
                Verb = "runas",
                WindowStyle = ProcessWindowStyle.Hidden,
            });
            ((MainVm)DataContext).RestartNeeded = false;
        }
        catch (System.ComponentModel.Win32Exception)
        {
            // The user declined the UAC prompt.
        }
    }
}
