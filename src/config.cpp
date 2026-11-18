#include "config.h"
#include <shlobj.h>
#include <map>

static const wchar_t* kDefaultConfig =
LR"INI(; TopBar settings. Save this file and the bar reloads it within ~2 seconds.
; Lines starting with ; are comments.

[bar]
; all = one bar on every monitor, primary = only on the main monitor
monitors = all
; Height in pixels at 100% display scaling (scaled automatically per monitor)
height = 30
; 30..100 (100 = solid)
opacity = 100
font = Segoe UI Variable Text
font_size = 13
; Leave empty for the built-in Windows icon font
icon_font =
; Available modules: windows, clock, netspeed, network, cpu, gpu, memory, disk,
;                    volume, bluetooth, battery, power
modules_left = windows
modules_center = clock
modules_right = netspeed, network, cpu, gpu, memory, disk, volume, bluetooth, battery, power
; 1 = each monitor's bar lists only the windows on that monitor
windows_per_monitor = 1
show_active_title = 1
max_title_width = 260
; How often stats refresh (milliseconds). Slower on battery to save power.
interval_ms = 1000
interval_battery_ms = 2000
; Show network speed in bits (Mb/s) instead of bytes (MB/s)
net_bits = 0
; Tiny per-core usage bars inside the CPU module
cpu_core_bars = 1
; time        = share of each second the cores were busy (the direct measurement; default)
; taskmanager = Task Manager's "utility": busy time scaled by clock speed vs. base clock
cpu_usage = time
; auto = the dedicated GPU (or the only one), all = every GPU, or an index 0,1,...
gpu = auto

[behavior]
; Hide the bar when a fullscreen game/video covers that monitor (stats pause too)
hide_in_fullscreen = 1
; In fullscreen video, rest the mouse on the very top edge to peek at the bar.
; Never triggers in games that capture or hide the mouse.
reveal_on_hover = 1
reveal_delay_ms = 350
; Peek during fullscreen / hide-show the bar the rest of the time
peek_hotkey = Ctrl+Alt+B
peek_seconds = 5
; Extra apps that should always hide the bar while focused (comma separated exe names)
game_apps =
; Run in Windows "efficiency mode" (lower CPU priority and power)
efficiency_mode = 1
; Start TopBar automatically when you sign in to Windows (no admin rights needed)
start_with_windows = 1
; Hover fades and tooltip motion. Also off when Windows' "Animation effects" are off.
animations = 1

[clock]
; Date pattern: d dd ddd dddd, M MM MMM MMMM, yy yyyy
date_format = ddd d MMM
seconds = 0
; auto = follow Windows, 1 = 24-hour, 0 = 12-hour
24h = auto

[pet]
; A little animated companion that walks along the empty part of the bar. Off by default.
; It costs about as much as a blinking cursor: only the few pixels around it are redrawn.
enabled = 0
; A GIF (or PNG) on this PC. Relative paths start next to this file. Empty = the built-in blob,
; which takes the accent colour. Files over 32 MB or 2048x2048 are refused.
file =
; walk = strolls back and forth, stay = sits in the middle of the free space
motion = walk
; Walking speed in pixels per second (at 100% scaling)
speed = 24
; Height in pixels at 100% scaling. 0 = fit the bar.
size = 0
; Movement frames per second, 10..60 (the GIF keeps its own frame timing)
fps = 30
; Which way the GIF faces: right or left. It is mirrored when walking the other way.
faces = right
; auto = the widest empty space, left = between the apps and the clock, right = between the clock and the stats
area = auto
; auto = crisp pixels when enlarging (pixel art), smooth when shrinking. 1 = always smooth, 0 = always crisp.
smooth = auto
pause_on_battery = 0

[colors]
; 1 = take the colours below from the wallpaper (like pywal / wallust) and follow it when it changes:
; the background and pills take its main colour, the icons its most vivid colours
from_wallpaper = 0
; How much of the wallpaper's colour the background and pills take: 0 = grey ... 100 = strong
wallpaper_tint = 60
background = #11111b
pill = #1e1e2e
pill_hover = #313244
pill_active = #45475a
text = #cdd6f4
subtext = #7f849c
accent = #89b4fa
clock = #cdd6f4
cpu = #89b4fa
gpu = #a6e3a1
memory = #cba6f7
disk = #f5c2e7
temp = #fab387
hot = #f38ba8
network = #94e2d5
volume = #f9e2af
bluetooth = #74c7ec
battery = #a6e3a1
warn = #f9e2af
power = #f38ba8
)INI";

void Config::LoadDefaultsTheme() {
    auto c = [](const wchar_t* hex) { Color col; ParseColor(hex, col); return col; };
    theme.background = c(L"#11111b");
    theme.pill = c(L"#1e1e2e");
    theme.pillHover = c(L"#313244");
    theme.pillActive = c(L"#45475a");
    theme.text = c(L"#cdd6f4");
    theme.subtext = c(L"#7f849c");
    theme.accent = c(L"#89b4fa");
    theme.clock = c(L"#cdd6f4");
    theme.cpu = c(L"#89b4fa");
    theme.gpu = c(L"#a6e3a1");
    theme.memory = c(L"#cba6f7");
    theme.disk = c(L"#f5c2e7");
    theme.temp = c(L"#fab387");
    theme.hot = c(L"#f38ba8");
    theme.network = c(L"#94e2d5");
    theme.volume = c(L"#f9e2af");
    theme.bluetooth = c(L"#74c7ec");
    theme.battery = c(L"#a6e3a1");
    theme.warn = c(L"#f9e2af");
    theme.power = c(L"#f38ba8");
}

static bool FileExists(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static bool WriteUtf8File(const std::wstring& path, const std::wstring& text) {
    int n = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0, nullptr, nullptr);
    std::string utf8((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), utf8.data(), n, nullptr, nullptr);
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    BOOL ok = WriteFile(h, utf8.data(), (DWORD)utf8.size(), &written, nullptr);
    CloseHandle(h);
    return ok && written == utf8.size();
}

static bool ReadTextFile(const std::wstring& path, std::wstring& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    GetFileSizeEx(h, &size);
    if (size.QuadPart > 1 << 20) { CloseHandle(h); return false; }
    std::string bytes((size_t)size.QuadPart, '\0');
    DWORD read = 0;
    BOOL ok = ReadFile(h, bytes.data(), (DWORD)bytes.size(), &read, nullptr);
    CloseHandle(h);
    if (!ok) return false;
    bytes.resize(read);
    if (bytes.size() >= 2 && (unsigned char)bytes[0] == 0xFF && (unsigned char)bytes[1] == 0xFE) {
        out.assign((const wchar_t*)(bytes.data() + 2), (bytes.size() - 2) / 2);
        return true;
    }
    size_t off = (bytes.size() >= 3 && (unsigned char)bytes[0] == 0xEF && (unsigned char)bytes[1] == 0xBB &&
                  (unsigned char)bytes[2] == 0xBF) ? 3 : 0;
    int n = MultiByteToWideChar(CP_UTF8, 0, bytes.data() + off, (int)(bytes.size() - off), nullptr, 0);
    out.assign((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, bytes.data() + off, (int)(bytes.size() - off), out.data(), n);
    return true;
}

std::wstring Config::ResolvePath() {
    std::wstring local = ExeDirectory() + L"\\topbar.ini";
    if (FileExists(local)) return local;
    // Portable: try to create next to the exe.
    if (WriteUtf8File(local, kDefaultConfig)) return local;
    // Otherwise use %APPDATA%\TopBar.
    PWSTR appdata = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appdata))) {
        dir = std::wstring(appdata) + L"\\TopBar";
        CoTaskMemFree(appdata);
    } else {
        dir = ExeDirectory();
    }
    CreateDirectoryW(dir.c_str(), nullptr);
    std::wstring p = dir + L"\\topbar.ini";
    if (!FileExists(p)) WriteUtf8File(p, kDefaultConfig);
    return p;
}

static FILETIME GetWriteTime(const std::wstring& p) {
    WIN32_FILE_ATTRIBUTE_DATA d{};
    if (GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &d)) return d.ftLastWriteTime;
    return FILETIME{};
}

bool Config::ChangedOnDisk() const {
    FILETIME ft = GetWriteTime(path);
    return CompareFileTime(&ft, &writeTime) != 0;
}

static std::wstring StripComment(const std::wstring& v) {
    // Inline comments need whitespace before ';' so values like "a;b" survive.
    for (size_t i = 1; i < v.size(); ++i)
        if (v[i] == L';' && iswspace(v[i - 1])) return Trim(v.substr(0, i));
    return Trim(v);
}

bool Config::Load() {
    *this = Config();
    LoadDefaultsTheme();
    path = ResolvePath();
    writeTime = GetWriteTime(path);

    std::wstring text;
    if (!ReadTextFile(path, text)) return false;

    std::map<std::wstring, std::wstring> kv;  // "section.key" -> value
    std::wstring section;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t eol = text.find(L'\n', pos);
        if (eol == std::wstring::npos) eol = text.size();
        std::wstring line = Trim(text.substr(pos, eol - pos));
        pos = eol + 1;
        if (line.empty() || line[0] == L';' || line[0] == L'#') continue;
        if (line[0] == L'[') {
            size_t close = line.find(L']');
            section = ToLower(Trim(line.substr(1, close == std::wstring::npos ? std::wstring::npos : close - 1)));
            continue;
        }
        size_t eq = line.find(L'=');
        if (eq == std::wstring::npos) continue;
        kv[section + L"." + ToLower(Trim(line.substr(0, eq)))] = StripComment(line.substr(eq + 1));
    }

    auto str = [&](const wchar_t* k, std::wstring& out) {
        auto it = kv.find(k);
        if (it != kv.end()) out = it->second;
    };
    auto num = [&](const wchar_t* k, int& out, int lo, int hi) {
        auto it = kv.find(k);
        if (it != kv.end() && !it->second.empty()) out = std::clamp(_wtoi(it->second.c_str()), lo, hi);
    };
    auto flt = [&](const wchar_t* k, float& out, float lo, float hi) {
        auto it = kv.find(k);
        if (it != kv.end() && !it->second.empty()) out = std::clamp((float)_wtof(it->second.c_str()), lo, hi);
    };
    auto boolean = [&](const wchar_t* k, bool& out) {
        auto it = kv.find(k);
        if (it == kv.end() || it->second.empty()) return;
        std::wstring v = ToLower(it->second);
        out = (v == L"1" || v == L"true" || v == L"yes" || v == L"on");
    };
    auto list = [&](const wchar_t* k, std::vector<std::wstring>& out, bool lower) {
        auto it = kv.find(k);
        if (it == kv.end()) return;
        out = Split(lower ? ToLower(it->second) : it->second, L',');
    };
    auto color = [&](const wchar_t* k, Color& out) {
        auto it = kv.find(k);
        if (it != kv.end()) ParseColor(it->second, out);
    };

    str(L"bar.monitors", monitors);
    monitors = ToLower(monitors);
    num(L"bar.height", height, 18, 80);
    num(L"bar.opacity", opacity, 30, 100);
    str(L"bar.font", font);
    if (font.empty()) font = L"Segoe UI Variable Text";
    flt(L"bar.font_size", fontSize, 8.f, 32.f);
    str(L"bar.icon_font", iconFont);
    list(L"bar.modules_left", modulesLeft, true);
    list(L"bar.modules_center", modulesCenter, true);
    list(L"bar.modules_right", modulesRight, true);
    boolean(L"bar.windows_per_monitor", windowsPerMonitor);
    boolean(L"bar.show_active_title", showActiveTitle);
    num(L"bar.max_title_width", maxTitleWidth, 0, 2000);
    num(L"bar.interval_ms", intervalMs, 250, 10000);
    num(L"bar.interval_battery_ms", intervalBatteryMs, 250, 20000);
    boolean(L"bar.net_bits", netBits);
    boolean(L"bar.cpu_core_bars", cpuCoreBars);
    {
        std::wstring mode;
        str(L"bar.cpu_usage", mode);
        if (!mode.empty()) cpuTaskManager = ToLower(mode) == L"taskmanager";
    }
    str(L"bar.gpu", gpuSelect);
    gpuSelect = ToLower(gpuSelect);

    boolean(L"behavior.hide_in_fullscreen", hideInFullscreen);
    boolean(L"behavior.reveal_on_hover", revealOnHover);
    num(L"behavior.reveal_delay_ms", revealDelayMs, 0, 5000);
    str(L"behavior.peek_hotkey", peekHotkey);
    num(L"behavior.peek_seconds", peekSeconds, 1, 120);
    list(L"behavior.game_apps", gameApps, true);
    boolean(L"behavior.efficiency_mode", efficiencyMode);
    boolean(L"behavior.start_with_windows", startWithWindows);
    boolean(L"behavior.animations", animations);

    str(L"clock.date_format", dateFormat);
    boolean(L"clock.seconds", clockSeconds);
    str(L"clock.24h", clock24h);
    clock24h = ToLower(clock24h);

    boolean(L"pet.enabled", petEnabled);
    str(L"pet.file", petFile);
    petFile = ResolveUserPath(petFile);
    {
        std::wstring v;
        str(L"pet.motion", v);
        if (!v.empty()) petWalk = ToLower(v) != L"stay";
        v.clear();
        str(L"pet.faces", v);
        if (!v.empty()) petFacesLeft = ToLower(v) == L"left";
        v.clear();
        str(L"pet.smooth", v);
        v = ToLower(v);
        if (v == L"1" || v == L"yes" || v == L"on" || v == L"true") petSmooth = 1;
        else if (v == L"0" || v == L"no" || v == L"off" || v == L"false") petSmooth = 0;
    }
    flt(L"pet.speed", petSpeed, 0.f, 400.f);
    num(L"pet.size", petSize, 0, 80);
    num(L"pet.fps", petFps, 10, 60);
    str(L"pet.area", petArea);
    petArea = ToLower(petArea);
    boolean(L"pet.pause_on_battery", petPauseOnBattery);

    boolean(L"colors.from_wallpaper", fromWallpaper);
    num(L"colors.wallpaper_tint", wallpaperTint, 0, 100);
    color(L"colors.background", theme.background);
    color(L"colors.pill", theme.pill);
    color(L"colors.pill_hover", theme.pillHover);
    color(L"colors.pill_active", theme.pillActive);
    color(L"colors.text", theme.text);
    color(L"colors.subtext", theme.subtext);
    color(L"colors.accent", theme.accent);
    color(L"colors.clock", theme.clock);
    color(L"colors.cpu", theme.cpu);
    color(L"colors.gpu", theme.gpu);
    color(L"colors.memory", theme.memory);
    color(L"colors.disk", theme.disk);
    color(L"colors.temp", theme.temp);
    color(L"colors.hot", theme.hot);
    color(L"colors.network", theme.network);
    color(L"colors.volume", theme.volume);
    color(L"colors.bluetooth", theme.bluetooth);
    color(L"colors.battery", theme.battery);
    color(L"colors.warn", theme.warn);
    color(L"colors.power", theme.power);
    return true;
}

std::wstring Config::ResolveUserPath(const std::wstring& in) const {
    std::wstring p = Trim(in);
    if (p.size() >= 2 && p.front() == L'"' && p.back() == L'"') p = p.substr(1, p.size() - 2);
    if (p.empty()) return p;
    wchar_t buf[MAX_PATH * 2];
    DWORD n = ExpandEnvironmentStringsW(p.c_str(), buf, _countof(buf));
    if (n > 0 && n <= _countof(buf)) p = buf;
    bool absolute = (p.size() >= 3 && iswalpha(p[0]) && p[1] == L':' && (p[2] == L'\\' || p[2] == L'/')) ||
                    (p.size() >= 2 && (p[0] == L'\\' || p[0] == L'/') && (p[1] == L'\\' || p[1] == L'/'));
    if (!absolute) {
        size_t slash = path.find_last_of(L"\\/");
        p = (slash == std::wstring::npos ? ExeDirectory() : path.substr(0, slash)) + L"\\" + p;
    }
    n = GetFullPathNameW(p.c_str(), _countof(buf), buf, nullptr);
    if (n > 0 && n < _countof(buf)) p = buf;
    return p;
}

void Config::SetValue(const wchar_t* sectionName, const wchar_t* key, const std::wstring& value) {
    std::wstring text;
    if (!ReadTextFile(path, text)) return;
    std::wstring wantSection = ToLower(sectionName), wantKey = ToLower(key);
    std::vector<std::wstring> lines;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t eol = text.find(L'\n', pos);
        if (eol == std::wstring::npos) eol = text.size();
        std::wstring line = text.substr(pos, eol - pos);
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        lines.push_back(line);
        pos = eol + 1;
    }
    std::wstring section;
    int sectionEnd = -1;
    bool done = false;
    for (size_t i = 0; i < lines.size() && !done; ++i) {
        std::wstring t = Trim(lines[i]);
        if (!t.empty() && t[0] == L'[') {
            size_t close = t.find(L']');
            section = ToLower(Trim(t.substr(1, close == std::wstring::npos ? std::wstring::npos : close - 1)));
            continue;
        }
        if (section == wantSection) sectionEnd = (int)i;
        if (section != wantSection || t.empty() || t[0] == L';') continue;
        size_t eq = t.find(L'=');
        if (eq == std::wstring::npos) continue;
        if (ToLower(Trim(t.substr(0, eq))) == wantKey) {
            lines[i] = std::wstring(key) + L" = " + value;
            done = true;
        }
    }
    if (!done) {
        if (sectionEnd >= 0) {
            lines.insert(lines.begin() + sectionEnd + 1, std::wstring(key) + L" = " + value);
        } else {
            lines.push_back(L"");
            lines.push_back(L"[" + std::wstring(sectionName) + L"]");
            lines.push_back(std::wstring(key) + L" = " + value);
        }
    }
    std::wstring out;
    for (size_t i = 0; i < lines.size(); ++i) {
        out += lines[i];
        if (i + 1 < lines.size()) out += L"\r\n";
    }
    WriteUtf8File(path, out);
}
