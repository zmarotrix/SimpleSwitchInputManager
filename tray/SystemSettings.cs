using System.Diagnostics;
using System.IO;
using System.Text.RegularExpressions;
using Microsoft.Win32;

namespace Ssim.Tray;

/// <summary>Steam's own "Steam Input for Switch controllers" setting (config.vdf).</summary>
public static class SteamInput
{
    private static readonly Regex Setting = new("(\"SteamController_SwitchSupport\"\\s*\")(\\d)(\")");

    private static string? SteamPath =>
        Registry.CurrentUser.OpenSubKey(@"Software\Valve\Steam")?.GetValue("SteamPath") as string;

    private static string? SteamExe =>
        Registry.CurrentUser.OpenSubKey(@"Software\Valve\Steam")?.GetValue("SteamExe") as string;

    private static string? ConfigPath => SteamPath is { } p ? Path.Combine(p, "config", "config.vdf") : null;

    public static bool IsRunning => Process.GetProcessesByName("steam").Length > 0;

    /// <summary>null when Steam or the setting isn't found.</summary>
    public static bool? Read()
    {
        try
        {
            if (ConfigPath is not { } path || !File.Exists(path)) return null;
            var m = Setting.Match(File.ReadAllText(path));
            return m.Success ? m.Groups[2].Value == "1" : null;
        }
        catch (IOException)
        {
            return null;
        }
    }

    /// <summary>Changes the setting, closing and reopening Steam if it is running.</summary>
    public static async Task<string> WriteAsync(bool enabled)
    {
        if (ConfigPath is not { } path) return "Steam was not found.";
        bool wasRunning = IsRunning;
        if (wasRunning && SteamExe is { } exe)
        {
            Process.Start(new ProcessStartInfo(exe, "-shutdown") { UseShellExecute = true });
            for (int i = 0; i < 120 && IsRunning; i++) await Task.Delay(250);
            if (IsRunning) return "Steam did not close, so nothing was changed.";
            await Task.Delay(500);
        }

        string text = await File.ReadAllTextAsync(path);
        if (!Setting.IsMatch(text)) return "Steam's Switch controller setting was not found.";
        string backup = path + ".ssim.bak";
        if (!File.Exists(backup)) File.Copy(path, backup);  // first change only
        text = Setting.Replace(text, m => m.Groups[1].Value + (enabled ? "1" : "0") + m.Groups[3].Value, 1);
        await File.WriteAllTextAsync(path, text);

        if (wasRunning && SteamExe is { } exe2) Process.Start(new ProcessStartInfo(exe2) { UseShellExecute = true });
        return enabled ? "Steam Input for Switch controllers is on." : "Steam will leave Switch controllers alone.";
    }
}

/// <summary>"Start with Windows" for the tray app (per user).</summary>
public static class Autostart
{
    private const string RunKey = @"Software\Microsoft\Windows\CurrentVersion\Run";
    private const string Name = "SimpleSwitchInputManager";

    public static bool Enabled
    {
        get => Registry.CurrentUser.OpenSubKey(RunKey)?.GetValue(Name) != null;
        set
        {
            using var key = Registry.CurrentUser.CreateSubKey(RunKey);
            if (value) key.SetValue(Name, $"\"{Environment.ProcessPath}\" --tray");
            else key.DeleteValue(Name, false);
        }
    }
}
