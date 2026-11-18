// TopBar entry point: single instance + command-line control.
//   TopBar.exe            start (or do nothing if already running)
//   TopBar.exe --toggle   hide/show the running bar
//   TopBar.exe --reload   reload settings
//   TopBar.exe --exit     quit the running bar
//   TopBar.exe --debug    write a log to %TEMP%\topbar.log
#include "app.h"

extern bool g_debugLog;

namespace {

// Defence in depth for an exe that people unzip into Downloads and run from there:
//  - DLLs load from System32 only, never from the exe's folder, so a stray or planted "pdh.dll"
//    next to TopBar.exe can't be picked up (the linker's /DEPENDENTLOADFLAG covers static imports).
//  - No code from network shares or low-integrity (browser download cache) locations.
//  - Legacy injection points (AppInit_DLLs, Winsock LSPs, global window hooks) are switched off.
// Every call is looked up at runtime and is simply skipped where Windows doesn't support it.
void HardenProcess() {
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    using SetDefaultDllDirectories_t = BOOL(WINAPI*)(DWORD);
    if (auto fn = (SetDefaultDllDirectories_t)GetProcAddress(k32, "SetDefaultDllDirectories"))
        fn(LOAD_LIBRARY_SEARCH_SYSTEM32);
    using SetDllDirectoryW_t = BOOL(WINAPI*)(LPCWSTR);
    if (auto fn = (SetDllDirectoryW_t)GetProcAddress(k32, "SetDllDirectoryW")) fn(L"");

    using SetPolicy_t = BOOL(WINAPI*)(PROCESS_MITIGATION_POLICY, PVOID, SIZE_T);
    auto setPolicy = (SetPolicy_t)GetProcAddress(k32, "SetProcessMitigationPolicy");
    if (!setPolicy) return;
    PROCESS_MITIGATION_IMAGE_LOAD_POLICY img{};
    img.NoRemoteImages = 1;
    img.NoLowMandatoryLabelImages = 1;
    setPolicy(ProcessImageLoadPolicy, &img, sizeof(img));
    PROCESS_MITIGATION_EXTENSION_POINT_DISABLE_POLICY ext{};
    ext.DisableExtensionPoints = 1;
    setPolicy(ProcessExtensionPointDisablePolicy, &ext, sizeof(ext));
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmdLine, int) {
    HardenProcess();

    // The manifest already asks for per-monitor DPI awareness; this is a belt-and-braces call that is
    // looked up at runtime so the exe still starts on early Windows 10 builds.
    using SetCtx_t = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
    if (auto fn = (SetCtx_t)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext"))
        fn(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    std::wstring args = ToLower(cmdLine ? cmdLine : L"");
    RemoteCommand cmd = (RemoteCommand)0;
    if (args.find(L"--exit") != std::wstring::npos || args.find(L"--quit") != std::wstring::npos) cmd = RemoteCommand::Exit;
    else if (args.find(L"--toggle") != std::wstring::npos) cmd = RemoteCommand::Toggle;
    else if (args.find(L"--reload") != std::wstring::npos) cmd = RemoteCommand::Reload;
#ifdef TOPBAR_DRY_RUN
    else if (args.find(L"--snapshot") != std::wstring::npos) cmd = RemoteCommand::Snapshot;
#endif
    g_debugLog = args.find(L"--debug") != std::wstring::npos;

    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\TopBar.SingleInstance.7f3c2a");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND existing = FindWindowW(L"TopBarController", nullptr);
        if (existing && cmd != (RemoteCommand)0) PostMessageW(existing, WM_APP_COMMAND, (WPARAM)cmd, 0);
        if (mutex) CloseHandle(mutex);
        return 0;
    }
    if (cmd == RemoteCommand::Exit || cmd == RemoteCommand::Reload || cmd == RemoteCommand::Snapshot) {
        if (mutex) CloseHandle(mutex);
        return 0;  // nothing running
    }

    App app;
    int rc = app.Run(inst, (int)cmd);
    if (mutex) { ReleaseMutex(mutex); CloseHandle(mutex); }
    return rc;
}
