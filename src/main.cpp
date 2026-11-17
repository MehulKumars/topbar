// TopBar entry point: single instance + command-line control.
//   TopBar.exe            start (or do nothing if already running)
//   TopBar.exe --toggle   hide/show the running bar
//   TopBar.exe --reload   reload settings
//   TopBar.exe --exit     quit the running bar
//   TopBar.exe --debug    write a log to %TEMP%\topbar.log
#include "app.h"

extern bool g_debugLog;

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmdLine, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    std::wstring args = ToLower(cmdLine ? cmdLine : L"");
    RemoteCommand cmd = (RemoteCommand)0;
    if (args.find(L"--exit") != std::wstring::npos || args.find(L"--quit") != std::wstring::npos) cmd = RemoteCommand::Exit;
    else if (args.find(L"--toggle") != std::wstring::npos) cmd = RemoteCommand::Toggle;
    else if (args.find(L"--reload") != std::wstring::npos) cmd = RemoteCommand::Reload;
    g_debugLog = args.find(L"--debug") != std::wstring::npos;

    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\TopBar.SingleInstance.7f3c2a");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND existing = FindWindowW(L"TopBarController", nullptr);
        if (existing && cmd != (RemoteCommand)0) PostMessageW(existing, WM_APP_COMMAND, (WPARAM)cmd, 0);
        if (mutex) CloseHandle(mutex);
        return 0;
    }
    if (cmd == RemoteCommand::Exit || cmd == RemoteCommand::Reload) {
        if (mutex) CloseHandle(mutex);
        return 0;  // nothing running
    }

    App app;
    int rc = app.Run(inst, (int)cmd);
    if (mutex) { ReleaseMutex(mutex); CloseHandle(mutex); }
    return rc;
}
