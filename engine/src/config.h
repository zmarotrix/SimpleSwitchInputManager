#pragma once

#include "output.h"

#include <mutex>
#include <optional>
#include <string>
#include <vector>

struct ControllerSettings {
    std::string name;                      // user-visible label
    OutputMode mode = OutputMode::DirectInput;
    int slot = 0;                          // preferred player slot 1..8, 0 = first free
    float rumble = 1.0f;                   // 0..1 strength
};

struct GlobalSettings {
    OutputMode defaultMode = OutputMode::DirectInput;  // for controllers seen for the first time
    bool bluetooth = true;
    bool pairBluetooth = true;
    bool dsu = true;
    int dsuPort = 26760;
    int rumbleSafetySeconds = 20;  // stop rumble a game leaves untouched this long (0 = never)
};

// Settings persisted in %ProgramData%\SSIM\config.json, keyed by controller
// serial number so a controller keeps its settings on USB and Bluetooth.
class Config {
public:
    static Config& Get();

    void Load();
    GlobalSettings Global();
    void SetGlobal(const GlobalSettings& g);

    // Returns the stored settings, creating defaults for a new controller.
    ControllerSettings ForController(const std::string& serial, const std::string& defaultName);
    void SetController(const std::string& serial, const ControllerSettings& s);
    std::vector<std::pair<std::string, ControllerSettings>> AllControllers();

private:
    void SaveLocked();
    std::wstring Path() const;

    std::mutex m_lock;
    GlobalSettings m_global;
    std::vector<std::pair<std::string, ControllerSettings>> m_controllers;
    bool m_readOnly = false;  // set when the file exists but could not be parsed
};

const char* OutputModeKey(OutputMode m);  // "dinput" / "xinput" / "dualsense"
std::optional<OutputMode> ParseOutputMode(const std::string& s);
