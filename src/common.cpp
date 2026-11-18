#include "common.h"
#include <objbase.h>
#include <cstdarg>
#include <cstdio>

std::wstring Fmt(const wchar_t* fmt, ...) {
    wchar_t buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = _vsnwprintf_s(buf, _countof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    if (n < 0) n = (int)wcslen(buf);
    return std::wstring(buf, (size_t)n);
}

std::wstring FormatBytes(uint64_t bytes, int decimals) {
    const wchar_t* units[] = { L"B", L"KB", L"MB", L"GB", L"TB" };
    double v = (double)bytes;
    int u = 0;
    while (v >= 1024.0 && u < 4) { v /= 1024.0; ++u; }
    if (u == 0) return Fmt(L"%llu B", bytes);
    if (v >= 100.0) decimals = 0;
    return Fmt(L"%.*f %s", decimals, v, units[u]);
}

std::wstring FormatRate(double bps, bool bits) {
    if (bps < 0) bps = 0;
    if (bits) {
        double v = bps * 8.0;
        const wchar_t* units[] = { L"b/s", L"Kb/s", L"Mb/s", L"Gb/s" };
        int u = 0;
        while (v >= 1000.0 && u < 3) { v /= 1000.0; ++u; }
        if (u == 0) return Fmt(L"%.0f %s", v, units[u]);
        return Fmt(v < 10.0 ? L"%.1f %s" : L"%.0f %s", v, units[u]);
    }
    const wchar_t* units[] = { L"B/s", L"KB/s", L"MB/s", L"GB/s" };
    double v = bps;
    int u = 0;
    while (v >= 1000.0 && u < 3) { v /= 1024.0; ++u; }
    if (u == 0) return Fmt(L"%.0f %s", v, units[u]);
    return Fmt(v < 10.0 ? L"%.1f %s" : L"%.0f %s", v, units[u]);
}

std::wstring FormatDuration(uint64_t s) {
    uint64_t d = s / 86400, h = (s % 86400) / 3600, m = (s % 3600) / 60;
    if (d > 0) return Fmt(L"%llud %lluh", d, h);
    if (h > 0) return Fmt(L"%lluh %llum", h, m);
    return Fmt(L"%llum", m);
}

std::wstring ToLower(std::wstring s) {
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}

std::wstring Trim(const std::wstring& s) {
    size_t a = 0, b = s.size();
    while (a < b && iswspace(s[a])) ++a;
    while (b > a && iswspace(s[b - 1])) --b;
    return s.substr(a, b - a);
}

std::vector<std::wstring> Split(const std::wstring& s, wchar_t sep) {
    std::vector<std::wstring> out;
    size_t start = 0;
    while (start <= s.size()) {
        size_t p = s.find(sep, start);
        if (p == std::wstring::npos) p = s.size();
        std::wstring part = Trim(s.substr(start, p - start));
        if (!part.empty()) out.push_back(part);
        start = p + 1;
    }
    return out;
}

std::wstring ExePath() {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, buf, _countof(buf));
    return std::wstring(buf, n);
}

std::wstring ExeDirectory() {
    std::wstring p = ExePath();
    size_t slash = p.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : p.substr(0, slash);
}

std::wstring ProcessImagePath(DWORD pid) {
    std::wstring result;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (h) {
        wchar_t buf[MAX_PATH * 2];
        DWORD len = _countof(buf);
        if (QueryFullProcessImageNameW(h, 0, buf, &len)) result.assign(buf, len);
        CloseHandle(h);
    }
    return result;
}

std::wstring ProcessImageName(DWORD pid) {
    std::wstring p = ProcessImagePath(pid);
    size_t slash = p.find_last_of(L"\\/");
    if (slash != std::wstring::npos) p = p.substr(slash + 1);
    return ToLower(p);
}

static int HexVal(wchar_t c) {
    if (c >= L'0' && c <= L'9') return c - L'0';
    if (c >= L'a' && c <= L'f') return c - L'a' + 10;
    if (c >= L'A' && c <= L'F') return c - L'A' + 10;
    return -1;
}

bool ParseColor(const std::wstring& in, Color& out) {
    std::wstring s = Trim(in);
    if (!s.empty() && s[0] == L'#') s = s.substr(1);
    if (s.size() != 6 && s.size() != 8) return false;
    int v[8];
    for (size_t i = 0; i < s.size(); ++i) {
        v[i] = HexVal(s[i]);
        if (v[i] < 0) return false;
    }
    out.r = (v[0] * 16 + v[1]) / 255.f;
    out.g = (v[2] * 16 + v[3]) / 255.f;
    out.b = (v[4] * 16 + v[5]) / 255.f;
    out.a = s.size() == 8 ? (v[6] * 16 + v[7]) / 255.f : 1.f;
    return true;
}

double NowSeconds() {
    static LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)freq.QuadPart;
}

bool RunInBackground(std::function<void()> job) {
    auto* fn = new std::function<void()>(std::move(job));
    auto proc = [](LPVOID arg) -> DWORD {
        std::unique_ptr<std::function<void()>> f((std::function<void()>*)arg);
        // Background mode lowers CPU, I/O and memory priority so decoding never competes with games.
        SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        (*f)();
        if (SUCCEEDED(hr)) CoUninitialize();
        return 0;
    };
    HANDLE h = CreateThread(nullptr, 256 * 1024, proc, fn, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (!h) { delete fn; return false; }
    CloseHandle(h);
    return true;
}

bool SystemAnimationsEnabled() {
    BOOL on = TRUE;
    if (!SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &on, 0)) return true;
    return on != FALSE;
}

bool g_debugLog = false;

void DebugLog(const wchar_t* fmt, ...) {
    if (!g_debugLog) return;
    wchar_t buf[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(buf, _countof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    OutputDebugStringW(buf);
    OutputDebugStringW(L"\n");
    wchar_t path[MAX_PATH];
    GetTempPathW(MAX_PATH, path);
    wcscat_s(path, L"topbar.log");
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"a, ccs=UTF-8") == 0 && f) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        fwprintf(f, L"%02d:%02d:%02d.%03d %s\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, buf);
        fclose(f);
    }
}
