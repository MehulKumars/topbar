#include "winlist.h"
#include "render.h"
#include <shobjidl.h>
#include <shlobj.h>
#include <propsys.h>
#include <appmodel.h>

static const PROPERTYKEY kPKEY_AppUserModel_ID = { { 0x9F4C2855, 0x9F79, 0x4B39, { 0xA8, 0xD0, 0xE1, 0xD4, 0x2D, 0xE1, 0xD5, 0xF3 } }, 5 };

static std::wstring ClassName(HWND h) {
    wchar_t buf[128];
    int n = GetClassNameW(h, buf, _countof(buf));
    return std::wstring(buf, n > 0 ? n : 0);
}

bool IsAppWindow(HWND h) {
    if (!IsWindowVisible(h)) return false;
    LONG ex = GetWindowLongW(h, GWL_EXSTYLE);
    if (!(ex & WS_EX_APPWINDOW)) {
        if (ex & WS_EX_TOOLWINDOW) return false;
        if (ex & WS_EX_NOACTIVATE) return false;
        if (GetWindow(h, GW_OWNER)) return false;
    }
    DWORD cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid == GetCurrentProcessId()) return false;
    std::wstring cls = ClassName(h);
    if (cls == L"Windows.UI.Core.CoreWindow" || cls == L"Progman" || cls == L"WorkerW" ||
        cls == L"Shell_TrayWnd" || cls == L"Shell_SecondaryTrayWnd")
        return false;
    if (GetWindowTextLengthW(h) == 0) {
        wchar_t t[2];
        if (InternalGetWindowText(h, t, 2) == 0) return false;
    }
    return true;
}

static HMONITOR WindowMonitor(HWND h) {
    if (IsIconic(h)) {
        WINDOWPLACEMENT wp{ sizeof(wp) };
        if (GetWindowPlacement(h, &wp)) return MonitorFromRect(&wp.rcNormalPosition, MONITOR_DEFAULTTONEAREST);
    }
    return MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST);
}

struct EnumCtx { std::vector<HWND> found; };

static BOOL CALLBACK EnumProc(HWND h, LPARAM lp) {
    if (IsAppWindow(h)) ((EnumCtx*)lp)->found.push_back(h);
    return TRUE;
}

WinItem* WindowList::Find(HWND h) {
    for (auto& it : items)
        if (it.hwnd == h) return &it;
    return nullptr;
}

bool WindowList::Refresh() {
    EnumCtx ctx;
    EnumWindows(EnumProc, (LPARAM)&ctx);
    bool changed = false;

    // Drop closed windows (keep order of the rest).
    size_t before = items.size();
    items.erase(std::remove_if(items.begin(), items.end(), [&](const WinItem& it) {
                    return std::find(ctx.found.begin(), ctx.found.end(), it.hwnd) == ctx.found.end();
                }), items.end());
    changed |= items.size() != before;

    // EnumWindows is top-to-bottom z-order; append new windows oldest-first so new ones go to the end.
    for (auto it = ctx.found.rbegin(); it != ctx.found.rend(); ++it) {
        if (Find(*it)) continue;
        WinItem w;
        w.hwnd = *it;
        GetWindowThreadProcessId(w.hwnd, &w.pid);
        w.exe = ProcessImageName(w.pid);
        items.push_back(std::move(w));
        changed = true;
    }

    for (auto& w : items) {
        wchar_t buf[512];
        int n = InternalGetWindowText(w.hwnd, buf, _countof(buf));
        std::wstring title(buf, n > 0 ? n : 0);
        if (title != w.title) { w.title = std::move(title); changed = true; }
        bool mini = IsIconic(w.hwnd) != FALSE;
        if (mini != w.minimized) { w.minimized = mini; changed = true; }
        HMONITOR m = WindowMonitor(w.hwnd);
        if (m != w.monitor) { w.monitor = m; changed = true; }
    }

    HWND fg = GetForegroundWindow();
    HWND root = fg ? GetAncestor(fg, GA_ROOTOWNER) : nullptr;
    HWND act = Find(fg) ? fg : (root && Find(root) ? root : nullptr);
    if (act != active_) { active_ = act; changed = true; }
    if (WinItem* a = Find(active_)) {
        if (a->flashing) { a->flashing = false; changed = true; }
    }
    return changed;
}

bool WindowList::UpdateTitle(HWND h) {
    WinItem* w = Find(h);
    if (!w) return false;
    wchar_t buf[512];
    int n = InternalGetWindowText(h, buf, _countof(buf));
    std::wstring title(buf, n > 0 ? n : 0);
    if (title == w->title) return false;
    w->title = std::move(title);
    return true;
}

void WindowList::MarkIconDirty(HWND h) {
    if (WinItem* w = Find(h)) w->iconDirty = true;
}

void WindowList::SetFlashing(HWND h, bool on) {
    if (WinItem* w = Find(h)) w->flashing = on && h != active_;
}

// --- icons ----------------------------------------------------------------------------------

static void Premultiply(std::vector<uint32_t>& px, bool force) {
    bool straight = force;
    if (!straight) {
        for (uint32_t p : px) {
            uint32_t a = p >> 24, r = (p >> 16) & 255, g = (p >> 8) & 255, b = p & 255;
            if (r > a || g > a || b > a) { straight = true; break; }
        }
    }
    if (!straight) return;
    for (uint32_t& p : px) {
        uint32_t a = p >> 24, r = (p >> 16) & 255, g = (p >> 8) & 255, b = p & 255;
        r = r * a / 255; g = g * a / 255; b = b * a / 255;
        p = (a << 24) | (r << 16) | (g << 8) | b;
    }
}

static bool BitmapPixels(HBITMAP hb, std::vector<uint32_t>& px, int& w, int& h) {
    BITMAP bm{};
    if (!GetObjectW(hb, sizeof(bm), &bm) || bm.bmWidth <= 0 || bm.bmHeight == 0) return false;
    w = bm.bmWidth;
    h = std::abs(bm.bmHeight);
    BITMAPINFO bi{};
    bi.bmiHeader = { sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB };
    px.assign((size_t)w * h, 0);
    HDC dc = GetDC(nullptr);
    int lines = GetDIBits(dc, hb, 0, (UINT)h, px.data(), &bi, DIB_RGB_COLORS);
    ReleaseDC(nullptr, dc);
    return lines == h;
}

static bool IconPixels(HICON icon, std::vector<uint32_t>& px, int& w, int& h) {
    ICONINFO ii{};
    if (!GetIconInfo(icon, &ii)) return false;
    bool ok = false;
    if (ii.hbmColor && BitmapPixels(ii.hbmColor, px, w, h)) {
        bool hasAlpha = false;
        for (uint32_t p : px) if (p >> 24) { hasAlpha = true; break; }
        if (!hasAlpha && ii.hbmMask) {
            std::vector<uint32_t> mask;
            int mw = 0, mh = 0;
            if (BitmapPixels(ii.hbmMask, mask, mw, mh) && mw == w && mh >= h) {
                for (size_t i = 0; i < px.size(); ++i) px[i] = (mask[i] & 0xFFFFFF) ? 0 : (px[i] | 0xFF000000u);
            } else {
                for (auto& p : px) p |= 0xFF000000u;
            }
        }
        Premultiply(px, true);
        ok = true;
    }
    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    if (ii.hbmMask) DeleteObject(ii.hbmMask);
    return ok;
}

static BOOL CALLBACK FindCoreWindow(HWND h, LPARAM lp) {
    if (ClassName(h) == L"Windows.UI.Core.CoreWindow") {
        *(HWND*)lp = h;
        return FALSE;
    }
    return TRUE;
}

static std::wstring WindowAumid(HWND h) {
    std::wstring id;
    ComPtr<IPropertyStore> ps;
    if (SUCCEEDED(SHGetPropertyStoreForWindow(h, IID_PPV_ARGS(&ps)))) {
        PROPVARIANT pv;
        PropVariantInit(&pv);
        if (SUCCEEDED(ps->GetValue(kPKEY_AppUserModel_ID, &pv)) && pv.vt == VT_LPWSTR && pv.pwszVal) id = pv.pwszVal;
        PropVariantClear(&pv);
    }
    if (id.empty() && ClassName(h) == L"ApplicationFrameWindow") {
        HWND core = nullptr;
        EnumChildWindows(h, FindCoreWindow, (LPARAM)&core);
        if (core) {
            DWORD pid = 0;
            GetWindowThreadProcessId(core, &pid);
            HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            if (p) {
                wchar_t buf[APPLICATION_USER_MODEL_ID_MAX_LENGTH];
                UINT32 len = _countof(buf);
                if (GetApplicationUserModelId(p, &len, buf) == ERROR_SUCCESS) id = buf;
                CloseHandle(p);
            }
        }
    }
    return id;
}

static bool AumidIconPixels(const std::wstring& aumid, std::vector<uint32_t>& px, int& w, int& h) {
    ComPtr<IShellItem> item;
    if (FAILED(SHCreateItemInKnownFolder(FOLDERID_AppsFolder, KF_FLAG_DONT_VERIFY, aumid.c_str(), IID_PPV_ARGS(&item))))
        return false;
    ComPtr<IShellItemImageFactory> factory;
    if (FAILED(item.As(&factory))) return false;
    HBITMAP hb = nullptr;
    if (FAILED(factory->GetImage(SIZE{ 32, 32 }, SIIGBF_ICONONLY, &hb)) || !hb) return false;
    bool ok = BitmapPixels(hb, px, w, h);
    DeleteObject(hb);
    if (ok) Premultiply(px, false);
    return ok;
}

static HICON WindowIcon(HWND h, bool& owned) {
    owned = false;
    DWORD_PTR r = 0;
    if (SendMessageTimeoutW(h, WM_GETICON, ICON_BIG, 0, SMTO_ABORTIFHUNG, 40, &r) && r) return (HICON)r;
    if (HICON c = (HICON)GetClassLongPtrW(h, GCLP_HICON)) return c;
    return nullptr;
}

static HICON SmallWindowIcon(HWND h) {
    DWORD_PTR r = 0;
    if (SendMessageTimeoutW(h, WM_GETICON, ICON_SMALL2, 0, SMTO_ABORTIFHUNG, 40, &r) && r) return (HICON)r;
    if (SendMessageTimeoutW(h, WM_GETICON, ICON_SMALL, 0, SMTO_ABORTIFHUNG, 40, &r) && r) return (HICON)r;
    return (HICON)GetClassLongPtrW(h, GCLP_HICONSM);
}

void WindowList::UpdateIcons(Renderer& r) {
    for (auto& w : items) {
        if (!w.iconDirty && w.icon && w.iconGeneration == r.Generation()) continue;
        w.iconDirty = false;
        w.iconGeneration = r.Generation();
        std::vector<uint32_t> px;
        int iw = 0, ih = 0;
        bool ok = false;

        bool uwp = ClassName(w.hwnd) == L"ApplicationFrameWindow";
        if (uwp) {
            std::wstring aumid = WindowAumid(w.hwnd);
            if (!aumid.empty()) ok = AumidIconPixels(aumid, px, iw, ih);
        }
        if (!ok) {
            bool owned = false;
            if (HICON ic = WindowIcon(w.hwnd, owned)) ok = IconPixels(ic, px, iw, ih);
        }
        if (!ok) {
            std::wstring path = ProcessImagePath(w.pid);
            SHFILEINFOW sfi{};
            if (!path.empty() && SHGetFileInfoW(path.c_str(), 0, &sfi, sizeof(sfi), SHGFI_ICON | SHGFI_LARGEICON) && sfi.hIcon) {
                ok = IconPixels(sfi.hIcon, px, iw, ih);
                DestroyIcon(sfi.hIcon);
            }
        }
        if (!ok) {
            if (HICON ic = SmallWindowIcon(w.hwnd)) ok = IconPixels(ic, px, iw, ih);
        }
        w.icon = ok ? r.CreateBitmap(px.data(), iw, ih) : nullptr;
    }
}

void ActivateOrMinimize(HWND h) {
    if (!IsWindow(h)) return;
    HWND fg = GetForegroundWindow();
    if (fg == h && !IsIconic(h)) {
        ShowWindowAsync(h, SW_MINIMIZE);
        return;
    }
    if (IsIconic(h)) ShowWindowAsync(h, SW_RESTORE);
    if (!SetForegroundWindow(h)) {
        // Foreground lock fallback: a synthetic Alt tap lets us take the foreground legitimately
        // (we just received the user's click, so this matches their intent).
        INPUT in[2] = {};
        in[0].type = in[1].type = INPUT_KEYBOARD;
        in[0].ki.wVk = in[1].ki.wVk = VK_MENU;
        in[1].ki.dwFlags = KEYEVENTF_KEYUP;
        SendInput(2, in, sizeof(INPUT));
        SetForegroundWindow(h);
    }
}
