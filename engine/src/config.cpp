#include "config.h"

#include "nlohmann/json.hpp"

#include <shlobj.h>

#include <algorithm>
#include <fstream>

using json = nlohmann::json;

const char* OutputModeKey(OutputMode m)
{
    switch (m) {
    case OutputMode::XInput: return "xinput";
    case OutputMode::DualSense: return "dualsense";
    default: return "dinput";
    }
}

std::optional<OutputMode> ParseOutputMode(const std::string& s)
{
    if (s == "dinput" || s == "DirectInput") return OutputMode::DirectInput;
    if (s == "xinput" || s == "XInput") return OutputMode::XInput;
    if (s == "dualsense" || s == "DualSense") return OutputMode::DualSense;
    return std::nullopt;
}

Config& Config::Get()
{
    static Config c;
    return c;
}

std::wstring Config::Path() const
{
    PWSTR base = nullptr;
    std::wstring dir = L"C:\\ProgramData";
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramData, 0, nullptr, &base))) dir = base;
    CoTaskMemFree(base);
    dir += L"\\SSIM";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\config.json";
}

// Older dev builds kept a few settings in HKLM\SOFTWARE\Switch2Pro; use them as
// first-run defaults.
static DWORD LegacySetting(const wchar_t* name, DWORD fallback)
{
    DWORD v = fallback, size = sizeof(v);
    RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Switch2Pro", name, RRF_RT_REG_DWORD, nullptr, &v, &size);
    return v;
}

void Config::Load()
{
    std::lock_guard<std::mutex> lock(m_lock);
    std::ifstream f(Path());
    if (!f) {
        m_global.defaultMode = (OutputMode)std::clamp<DWORD>(LegacySetting(L"OutputMode", 0), 0, 2);
        m_global.bluetooth = LegacySetting(L"EnableBluetooth", 1) != 0;
        m_global.pairBluetooth = LegacySetting(L"PairBluetooth", 1) != 0;
        SaveLocked();
        Log("Config: created %ls", Path().c_str());
        return;
    }
    try {
        json j = json::parse(f);
        auto& g = j.at("global");
        m_global.defaultMode = ParseOutputMode(g.value("defaultOutputMode", "dinput")).value_or(OutputMode::DirectInput);
        m_global.bluetooth = g.value("bluetooth", true);
        m_global.pairBluetooth = g.value("pairBluetooth", true);
        m_global.dsu = g.value("dsu", true);
        m_global.dsuPort = std::clamp(g.value("dsuPort", 26760), 1024, 65535);
        m_global.rumbleSafetySeconds = std::clamp(g.value("rumbleSafetySeconds", 20), 0, 3600);
        m_controllers.clear();
        json controllers = j.value("controllers", json::object());  // keep alive while iterating
        for (auto& [serial, c] : controllers.items()) {
            ControllerSettings s;
            s.name = c.value("name", "");
            s.mode = ParseOutputMode(c.value("outputMode", "dinput")).value_or(OutputMode::DirectInput);
            s.slot = std::clamp(c.value("slot", 0), 0, 8);
            s.rumble = std::clamp(c.value("rumble", 1.0f), 0.f, 1.f);
            m_controllers.emplace_back(serial, s);
        }
    } catch (const std::exception& e) {
        // Keep the user's file untouched (saves go elsewhere until it is fixed).
        Log("Config: could not read config.json (%s); using defaults", e.what());
        m_readOnly = true;
    }
}

void Config::SaveLocked()
{
    if (m_readOnly) return;
    json j;
    j["version"] = 1;
    j["global"] = {{"defaultOutputMode", OutputModeKey(m_global.defaultMode)},
                   {"bluetooth", m_global.bluetooth},
                   {"pairBluetooth", m_global.pairBluetooth},
                   {"dsu", m_global.dsu},
                   {"dsuPort", m_global.dsuPort},
                   {"rumbleSafetySeconds", m_global.rumbleSafetySeconds}};
    json cs = json::object();
    for (auto& [serial, s] : m_controllers) {
        cs[serial] = {{"name", s.name}, {"outputMode", OutputModeKey(s.mode)}, {"slot", s.slot}, {"rumble", s.rumble}};
    }
    j["controllers"] = cs;

    // Write to a temp file then swap, so a crash never leaves a half-written config.
    std::wstring path = Path(), tmp = path + L".tmp";
    {
        std::ofstream f(tmp, std::ios::trunc);
        f << j.dump(2);
        if (!f.good()) {
            Log("Config: write failed");
            return;
        }
    }
    MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
}

GlobalSettings Config::Global()
{
    std::lock_guard<std::mutex> lock(m_lock);
    return m_global;
}

void Config::SetGlobal(const GlobalSettings& g)
{
    std::lock_guard<std::mutex> lock(m_lock);
    m_global = g;
    SaveLocked();
}

ControllerSettings Config::ForController(const std::string& serial, const std::string& defaultName)
{
    std::lock_guard<std::mutex> lock(m_lock);
    for (auto& [k, s] : m_controllers)
        if (k == serial) return s;
    ControllerSettings s;
    s.name = defaultName;
    s.mode = m_global.defaultMode;
    m_controllers.emplace_back(serial, s);
    SaveLocked();
    return s;
}

void Config::SetController(const std::string& serial, const ControllerSettings& s)
{
    std::lock_guard<std::mutex> lock(m_lock);
    for (auto& [k, v] : m_controllers) {
        if (k == serial) {
            v = s;
            SaveLocked();
            return;
        }
    }
    m_controllers.emplace_back(serial, s);
    SaveLocked();
}

std::vector<std::pair<std::string, ControllerSettings>> Config::AllControllers()
{
    std::lock_guard<std::mutex> lock(m_lock);
    return m_controllers;
}
