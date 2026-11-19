using System.Collections.ObjectModel;
using System.ComponentModel;
using System.Runtime.CompilerServices;
using System.Text.Json.Nodes;
using System.Windows.Media;

namespace Ssim.Tray;

public abstract class Observable : INotifyPropertyChanged
{
    public event PropertyChangedEventHandler? PropertyChanged;

    // True while applying engine status, so setters don't echo it back as commands.
    protected bool Applying;

    protected bool Set<T>(ref T field, T value, [CallerMemberName] string? name = null)
    {
        if (EqualityComparer<T>.Default.Equals(field, value)) return false;
        field = value;
        PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(name));
        return true;
    }

    protected void Raise(string name) => PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(name));
}

public record Choice(string Key, string Label);

public static class Choices
{
    public static readonly Choice[] OutputModes =
    {
        new("dinput", "DirectInput"),
        new("xinput", "XInput (Xbox)"),
        new("dualsense", "DualSense (PS5, gyro)"),
    };

    public static readonly int[] Slots = { 1, 2, 3, 4, 5, 6, 7, 8 };
}

/// <summary>A controller SSIM is driving, or a Switch 1-family controller it is leaving alone.</summary>
public sealed class ControllerVm : Observable
{
    private readonly EngineClient _engine;
    public string Serial { get; }

    public ControllerVm(EngineClient engine, string serial)
    {
        _engine = engine;
        Serial = serial;
    }

    private string _name = "";
    public string Name
    {
        get => _name;
        set { if (Set(ref _name, value) && !Applying) _ = _engine.SetControllerAsync(Serial, "name", value); }
    }

    private string _model = "";
    public string Model { get => _model; private set => Set(ref _model, value); }

    private string _transport = "";
    public string Transport
    {
        get => _transport;
        private set
        {
            if (!Set(ref _transport, value)) return;
            Raise(nameof(TransportLabel));
            Raise(nameof(ShortTransport));
        }
    }
    public string TransportLabel => Transport == "usb" ? "USB" : "Bluetooth";
    public string ShortTransport => Transport == "usb" ? "USB" : "BT";

    private int _battery = -1;
    public int Battery
    {
        get => _battery;
        private set
        {
            if (!Set(ref _battery, value)) return;
            Raise(nameof(BatteryLabel));
            Raise(nameof(ShortBattery));
        }
    }
    private bool _charging;
    public bool Charging
    {
        get => _charging;
        private set
        {
            if (!Set(ref _charging, value)) return;
            Raise(nameof(BatteryLabel));
            Raise(nameof(ShortBattery));
        }
    }
    public string BatteryLabel => Battery < 0 ? "Battery —" : $"Battery {Battery}%{(Charging ? ", charging" : "")}";
    public string ShortBattery => Battery < 0 ? "—" : Charging ? $"{Battery}% ⚡" : $"{Battery}%";

    private int _slot;
    public int Slot
    {
        get => _slot;
        set
        {
            if (!Set(ref _slot, value)) return;
            Raise(nameof(SlotLabel));
            Raise(nameof(SlotText));
            if (!Applying) _ = _engine.SetControllerAsync(Serial, "slot", value);
        }
    }
    public string SlotLabel => Slot > 0 ? $"Player {Slot}" : "No player slot";
    public string SlotText => IsManaged && Slot > 0 ? Slot.ToString() : "—";

    private string _outputMode = "dinput";
    public string OutputMode
    {
        get => _outputMode;
        set
        {
            if (!Set(ref _outputMode, value)) return;
            Raise(nameof(ShortMode));
            if (!Applying) _ = _engine.SetControllerAsync(Serial, "outputMode", value);
        }
    }
    public string ShortMode => !IsManaged ? "Not managed" : OutputMode switch { "xinput" => "XInput", "dualsense" => "DualSense", _ => "DirectInput" };

    private double _rumble = 1.0;
    public double Rumble
    {
        get => _rumble;
        set
        {
            if (!Set(ref _rumble, value)) return;
            Raise(nameof(RumbleLabel));
            if (!Applying) _ = _engine.SetControllerAsync(Serial, "rumble", Math.Round(value, 2));
        }
    }
    public string RumbleLabel => $"{Rumble * 100:0}%";

    private Brush _color = Brushes.Gray;
    public Brush Color { get => _color; private set => Set(ref _color, value); }

    public bool IsSwitch1 => IsManaged && Serial.StartsWith("SW1-") && Kind == "pro";

    // False for a Switch 1-family controller SSIM lists but leaves alone until enabled.
    private bool _isManaged = true;
    public bool IsManaged
    {
        get => _isManaged;
        private set
        {
            if (!Set(ref _isManaged, value)) return;
            Raise(nameof(IsUnmanaged));
            Raise(nameof(IsSwitch1));
            Raise(nameof(ShortMode));
            Raise(nameof(SlotText));
        }
    }
    public bool IsUnmanaged => !IsManaged;

    /// <summary>Shows a detected controller SSIM is not managing.</summary>
    public void ApplyDetected(string model)
    {
        Applying = true;
        try
        {
            IsManaged = false;
            Name = model;
            Model = model;
            Transport = "bluetooth";
            Battery = -1;
            Kind = "pro";
            Members = new();
        }
        finally
        {
            Applying = false;
        }
    }

    // Joy-Cons: "joycon" = one Joy-Con on its own, "joyconPair" = L + R together.
    private string _kind = "pro";
    public string Kind
    {
        get => _kind;
        private set
        {
            if (!Set(ref _kind, value)) return;
            Raise(nameof(IsJoyConPair));
            Raise(nameof(IsSingleJoyCon));
            Raise(nameof(IsSwitch1));
        }
    }
    public bool IsJoyConPair => Kind == "joyconPair";
    public bool IsSingleJoyCon => Kind == "joycon";
    public List<string> Members { get; private set; } = new();

    public void Apply(JsonObject c)
    {
        Applying = true;
        try
        {
            IsManaged = true;
            Name = (string?)c["name"] ?? "";
            Model = (string?)c["model"] ?? "";
            Transport = (string?)c["transport"] ?? "";
            Battery = (int?)c["battery"] ?? -1;
            Charging = (bool?)c["charging"] ?? false;
            Slot = (int?)c["slot"] ?? 0;
            OutputMode = (string?)c["outputMode"] ?? "dinput";
            Rumble = (double?)c["rumble"] ?? 1.0;
            Color = ParseColor((string?)c["bodyColor"]);
            Kind = (string?)c["kind"] ?? "pro";
            Members = (c["members"] as JsonArray)?.Select(m => (string?)m ?? "").ToList() ?? new();
        }
        finally
        {
            Applying = false;
        }
    }

    public string Summary => $"P{Slot} {Name} · {Battery}% · {Choices.OutputModes.FirstOrDefault(m => m.Key == OutputMode)?.Label}";

    private static Brush ParseColor(string? hex)
    {
        try
        {
            if (!string.IsNullOrEmpty(hex)) return new SolidColorBrush((System.Windows.Media.Color)ColorConverter.ConvertFromString(hex));
        }
        catch (FormatException) { }
        return Brushes.Gray;
    }
}

public sealed class MainVm : Observable
{
    private readonly EngineClient _engine;

    public ObservableCollection<ControllerVm> Controllers { get; } = new();

    public MainVm(EngineClient engine) => _engine = engine;

    private bool _engineConnected;
    public bool EngineConnected
    {
        get => _engineConnected;
        set { if (Set(ref _engineConnected, value)) Raise(nameof(EngineLabel)); }
    }
    public string EngineLabel => EngineConnected ? "Engine running" : "Engine not running — is the SSIM service installed?";

    private ControllerVm? _selected;
    public ControllerVm? Selected
    {
        get => _selected;
        set { if (Set(ref _selected, value)) Raise(nameof(HasSelection)); }
    }
    public bool HasSelection => Selected != null;

    public bool HasControllers => Controllers.Count > 0;
    public IEnumerable<ControllerVm> Managed => Controllers.Where(c => c.IsManaged);

    // ---- Global settings ----
    private string _defaultMode = "dinput";
    public string DefaultMode
    {
        get => _defaultMode;
        set { if (Set(ref _defaultMode, value) && !Applying) _ = _engine.SetGlobalAsync("defaultOutputMode", value); }
    }

    private bool _bluetooth = true;
    public bool Bluetooth
    {
        get => _bluetooth;
        set { if (Set(ref _bluetooth, value) && !Applying) _ = SetGlobalNeedsRestart("bluetooth", value); }
    }

    private bool _pairBluetooth = true;
    public bool PairBluetooth
    {
        get => _pairBluetooth;
        set { if (Set(ref _pairBluetooth, value) && !Applying) _ = _engine.SetGlobalAsync("pairBluetooth", value); }
    }

    private bool _dsu = true;
    public bool Dsu
    {
        get => _dsu;
        set { if (Set(ref _dsu, value) && !Applying) _ = SetGlobalNeedsRestart("dsu", value); }
    }

    private int _dsuPort = 26760;
    public int DsuPort
    {
        get => _dsuPort;
        set { if (Set(ref _dsuPort, value) && !Applying) _ = SetGlobalNeedsRestart("dsuPort", value); }
    }

    private bool _combineJoyCons = true;
    public bool CombineJoyCons
    {
        get => _combineJoyCons;
        set { if (Set(ref _combineJoyCons, value) && !Applying) _ = _engine.SetGlobalAsync("combineJoyCons", value); }
    }

    private bool _swapFaceButtons;
    public bool SwapFaceButtons
    {
        get => _swapFaceButtons;
        set { if (Set(ref _swapFaceButtons, value) && !Applying) _ = _engine.SetGlobalAsync("swapFaceButtons", value); }
    }

    private int _rumbleSafety = 20;
    public int RumbleSafety
    {
        get => _rumbleSafety;
        set { if (Set(ref _rumbleSafety, value) && !Applying) _ = _engine.SetGlobalAsync("rumbleSafetySeconds", value); }
    }

    private bool _restartNeeded;
    public bool RestartNeeded { get => _restartNeeded; set => Set(ref _restartNeeded, value); }

    private async Task SetGlobalNeedsRestart(string field, JsonNode value)
    {
        var reply = await _engine.SetGlobalAsync(field, value);
        if ((bool?)reply["restartRequired"] == true) RestartNeeded = true;
    }

    /// <summary>Merges an engine status message into the collections without rebuilding them.</summary>
    public void Apply(JsonObject status)
    {
        var live = status["controllers"] as JsonArray ?? new JsonArray();
        var seen = new HashSet<string>();
        foreach (var node in live.OfType<JsonObject>())
        {
            string serial = (string?)node["serial"] ?? "";
            seen.Add(serial);
            var vm = Controllers.FirstOrDefault(c => c.Serial == serial);
            if (vm == null)
            {
                vm = new ControllerVm(_engine, serial);
                Controllers.Add(vm);
            }
            vm.Apply(node);
        }

        // Switch 1-family controllers SSIM is leaving alone: listed, greyed out.
        foreach (var d in (status["detected"] as JsonArray ?? new JsonArray()).OfType<JsonObject>())
        {
            string id = (string?)d["id"] ?? "";
            if ((bool?)d["managed"] == true || !seen.Add(id)) continue;
            var vm = Controllers.FirstOrDefault(c => c.Serial == id);
            if (vm == null)
            {
                vm = new ControllerVm(_engine, id);
                Controllers.Add(vm);
            }
            vm.ApplyDetected((string?)d["model"] ?? "Switch controller");
        }

        foreach (var gone in Controllers.Where(c => !seen.Contains(c.Serial)).ToList()) Controllers.Remove(gone);
        if (Selected == null || !Controllers.Contains(Selected)) Selected = Controllers.FirstOrDefault();

        if (status["global"] is JsonObject g)
        {
            Applying = true;
            try
            {
                DefaultMode = (string?)g["defaultOutputMode"] ?? "dinput";
                Bluetooth = (bool?)g["bluetooth"] ?? true;
                PairBluetooth = (bool?)g["pairBluetooth"] ?? true;
                Dsu = (bool?)g["dsu"] ?? true;
                DsuPort = (int?)g["dsuPort"] ?? 26760;
                RumbleSafety = (int?)g["rumbleSafetySeconds"] ?? 20;
                CombineJoyCons = (bool?)g["combineJoyCons"] ?? true;
                SwapFaceButtons = (bool?)g["swapFaceButtons"] ?? false;
            }
            finally
            {
                Applying = false;
            }
        }
        Raise(nameof(HasControllers));
    }
}
