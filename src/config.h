#pragma once
#include "common.h"

struct Theme {
    Color background, pill, pillHover, pillActive, text, subtext, accent;
    Color cpu, gpu, memory, disk, temp, hot, network, volume, bluetooth, battery, warn, power, clock;
};

struct Config {
    // [bar]
    std::wstring monitors = L"all";          // all | primary
    int height = 30;                          // DIPs (pixels at 100% scaling)
    int opacity = 100;                        // 30..100
    std::wstring font = L"Segoe UI Variable Text";
    float fontSize = 13.f;
    std::wstring iconFont;                    // empty = auto (Segoe Fluent Icons / Segoe MDL2 Assets)
    std::vector<std::wstring> modulesLeft{ L"windows" };
    std::vector<std::wstring> modulesCenter{ L"clock" };
    std::vector<std::wstring> modulesRight{ L"netspeed", L"network", L"cpu", L"gpu", L"memory", L"disk",
                                            L"volume", L"bluetooth", L"battery", L"power" };
    bool windowsPerMonitor = true;
    bool showActiveTitle = true;
    int maxTitleWidth = 260;
    int intervalMs = 1000;
    int intervalBatteryMs = 2000;
    bool netBits = false;
    bool cpuCoreBars = true;
    std::wstring gpuSelect = L"auto";         // auto | all | <index>

    // [behavior]
    bool hideInFullscreen = true;
    bool revealOnHover = true;
    int revealDelayMs = 350;
    std::wstring peekHotkey = L"Ctrl+Alt+B";
    int peekSeconds = 5;
    std::vector<std::wstring> gameApps;       // lowercase exe names that always hide the bar
    bool efficiencyMode = true;

    // [clock]
    std::wstring dateFormat = L"ddd d MMM";
    bool clockSeconds = false;
    std::wstring clock24h = L"auto";          // auto | 1 | 0

    Theme theme;

    std::wstring path;                        // where the config was loaded from
    FILETIME writeTime{};

    void LoadDefaultsTheme();
    // Loads from disk (creating the default file if missing). Returns true if parsed.
    bool Load();
    // Has the file changed on disk since Load()?
    bool ChangedOnDisk() const;
    // Persist one key (rewrites the line in place, keeping comments).
    void SetValue(const wchar_t* section, const wchar_t* key, const std::wstring& value);

    static std::wstring ResolvePath();
};
