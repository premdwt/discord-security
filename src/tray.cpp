// tray.cpp — System tray: icon, menu, TaskbarCreated refresh, dan console hide relay.
#include "tray.h"
#include "console_window.h"
#include "logger.h"

#include <shellapi.h>
#include <string>

#pragma comment(lib, "shell32.lib")

namespace Tray {

namespace {

constexpr UINT kTrayIconId = 1;
constexpr UINT kTrayCallbackMessage = WM_USER + 1;
constexpr wchar_t kWindowClassName[] = L"DiscordSentinelTrayWnd";

HWND g_messageWindow = nullptr;
HICON g_trayIcon = nullptr;
Handlers g_handlers{};
bool g_initialized = false;
bool g_iconOnTaskbar = false;
UINT g_taskbarCreatedMessage = 0;

HMENU BuildContextMenu() {
    HMENU menu = CreatePopupMenu();
    if (!menu) {
        return nullptr;
    }

    AppendMenuW(menu, MF_STRING, IDM_SHOW_CONSOLE, L"Show Console");
    AppendMenuW(menu, MF_STRING, IDM_MINIMIZE_TO_TRAY, L"Minimize to Tray");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_EXIT, L"Exit");

    return menu;
}

void ShowContextMenu(HWND hwnd) {
    HMENU menu = BuildContextMenu();
    if (!menu) {
        return;
    }

    POINT cursor{};
    GetCursorPos(&cursor);
    SetForegroundWindow(hwnd);

    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_LEFTALIGN,
                   cursor.x, cursor.y, 0, hwnd, nullptr);

    DestroyMenu(menu);
    PostMessageW(hwnd, WM_NULL, 0, 0);
}

NOTIFYICONDATAW MakeTrayIconData(HWND hwnd, HICON icon) {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = kTrayIconId;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = kTrayCallbackMessage;
    nid.hIcon = icon;
    wcscpy_s(nid.szTip, L"Discord Sentinel — Klik untuk buka console");
    return nid;
}

bool AddTrayIcon(HWND hwnd, HICON icon) {
    NOTIFYICONDATAW nid = MakeTrayIconData(hwnd, icon);

    if (Shell_NotifyIconW(NIM_ADD, &nid) != FALSE) {
        g_iconOnTaskbar = true;
        return true;
    }

    // Kadang icon sudah ada (refresh) — coba modify.
    if (Shell_NotifyIconW(NIM_MODIFY, &nid) != FALSE) {
        g_iconOnTaskbar = true;
        return true;
    }

    Logger::LogWin32Error("Tray: Shell_NotifyIcon gagal menambahkan icon");
    g_iconOnTaskbar = false;
    return false;
}

void RemoveTrayIcon(HWND hwnd) {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = kTrayIconId;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    g_iconOnTaskbar = false;
}

void InvokeShowConsole() {
    if (g_handlers.onShowConsole) {
        g_handlers.onShowConsole();
    }
}

void InvokeMinimizeToTray() {
    if (g_handlers.onMinimizeToTray) {
        g_handlers.onMinimizeToTray();
    }
}

bool EnsureTrayIconLoaded() {
    if (g_trayIcon) {
        return true;
    }

    g_trayIcon = LoadIconW(nullptr, IDI_SHIELD);
    if (!g_trayIcon) {
        g_trayIcon = LoadIconW(nullptr, IDI_APPLICATION);
    }

    return g_trayIcon != nullptr;
}

} // namespace

// Membuat hidden window untuk menerima pesan tray dan TaskbarCreated.
HWND CreateMessageWindow(HINSTANCE instance) {
    if (g_taskbarCreatedMessage == 0) {
        g_taskbarCreatedMessage = RegisterWindowMessageW(L"TaskbarCreated");
        if (g_taskbarCreatedMessage == 0) {
            Logger::LogWin32Error("Tray: gagal register TaskbarCreated message");
        }
    }

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = MessageWindowProc;
    wc.hInstance = instance;
    wc.lpszClassName = kWindowClassName;

    if (!RegisterClassExW(&wc)) {
        if (GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            Logger::LogWin32Error("Tray: gagal register window class");
            return nullptr;
        }
    }

    HWND hwnd = CreateWindowExW(
        0,
        kWindowClassName,
        L"Discord Sentinel",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 0, 0,
        nullptr,
        nullptr,
        instance,
        nullptr);

    if (!hwnd) {
        Logger::LogWin32Error("Tray: gagal membuat message window");
        return nullptr;
    }

    ShowWindow(hwnd, SW_HIDE);
    return hwnd;
}

// Menambahkan icon ke system tray (idempotent jika sudah ada).
bool Initialize(HWND messageWindow, HINSTANCE instance, const Handlers& handlers) {
    (void)instance;

    if (g_initialized && g_iconOnTaskbar) {
        return true;
    }

    g_handlers = handlers;
    g_messageWindow = messageWindow;

    if (!EnsureTrayIconLoaded()) {
        Logger::Error("Tray: gagal memuat icon.");
        return false;
    }

    if (!AddTrayIcon(messageWindow, g_trayIcon)) {
        return false;
    }

    g_initialized = true;
    Logger::Info("System tray icon aktif.");
    return true;
}

// Delay startup agar explorer/taskbar siap sebelum Shell_NotifyIcon.
bool InitializeWithStartupDelay(HWND messageWindow,
                                HINSTANCE instance,
                                const Handlers& handlers,
                                DWORD delayMs) {
    if (delayMs > 0) {
        Logger::Info("Menunggu taskbar siap (" + std::to_string(delayMs) + " ms)...");
        Sleep(delayMs);
    }

    if (Initialize(messageWindow, instance, handlers)) {
        return true;
    }

    // Satu kali retry — explorer kadang belum siap meski sudah delay.
    Logger::Warning("Tray icon gagal pertama — retry setelah 1000 ms.");
    Sleep(1000);
    return Initialize(messageWindow, instance, handlers);
}

// Re-add icon setelah explorer mengirim TaskbarCreated.
bool RefreshTrayIcon() {
    if (!g_messageWindow || !EnsureTrayIconLoaded()) {
        return false;
    }

    RemoveTrayIcon(g_messageWindow);

    if (!AddTrayIcon(g_messageWindow, g_trayIcon)) {
        Logger::Warning("Tray: gagal refresh icon setelah TaskbarCreated.");
        return false;
    }

    g_initialized = true;
    Logger::Info("Tray icon di-refresh (TaskbarCreated).");
    return true;
}

void Shutdown() {
    if (!g_messageWindow) {
        return;
    }

    RemoveTrayIcon(g_messageWindow);

    if (g_trayIcon) {
        DestroyIcon(g_trayIcon);
        g_trayIcon = nullptr;
    }

    g_handlers = {};
    g_initialized = false;
    g_messageWindow = nullptr;
}

void ShowNotification(const wchar_t* title, const wchar_t* message) {
    if (!g_initialized || !g_messageWindow || !g_iconOnTaskbar) {
        return;
    }

    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(NOTIFYICONDATAW);
    nid.hWnd = g_messageWindow;
    nid.uID = kTrayIconId;
    nid.uFlags = NIF_INFO;
    nid.dwInfoFlags = NIIF_INFO;
    wcsncpy_s(nid.szInfoTitle, title, _TRUNCATE);
    wcsncpy_s(nid.szInfo, message, _TRUNCATE);

    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

// Window procedure: tray klik, menu, console hide, dan TaskbarCreated.
LRESULT CALLBACK MessageWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (g_taskbarCreatedMessage != 0 && msg == g_taskbarCreatedMessage) {
        RefreshTrayIcon();
        return 0;
    }

    if (msg == AppConsole::WM_APP_CONSOLE_HIDE) {
        InvokeMinimizeToTray();
        return 0;
    }

    switch (msg) {
        case kTrayCallbackMessage:
            switch (LOWORD(lParam)) {
                case WM_LBUTTONUP:
                case WM_LBUTTONDBLCLK:
                    InvokeShowConsole();
                    return 0;

                case WM_RBUTTONUP:
                case WM_CONTEXTMENU:
                    ShowContextMenu(hwnd);
                    return 0;
            }
            break;

        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case IDM_SHOW_CONSOLE:
                    InvokeShowConsole();
                    return 0;

                case IDM_MINIMIZE_TO_TRAY:
                    InvokeMinimizeToTray();
                    return 0;

                case IDM_EXIT:
                    if (g_handlers.onExit) {
                        g_handlers.onExit();
                    }
                    PostQuitMessage(0);
                    return 0;
            }
            break;

        case WM_DESTROY:
            Tray::Shutdown();
            return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace Tray