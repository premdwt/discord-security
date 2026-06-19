#pragma once

// tray.h — System tray icon, menu konteks, dan notifikasi balloon.
#include <functional>
#include <windows.h>

namespace Tray {

enum MenuId : UINT {
    IDM_SHOW_CONSOLE     = 1001,
    IDM_MINIMIZE_TO_TRAY = 1002,
    IDM_EXIT             = 1003,
};

struct Handlers {
    std::function<void()> onShowConsole;
    std::function<void()> onMinimizeToTray;
    std::function<void()> onExit;
};

HWND CreateMessageWindow(HINSTANCE instance);

bool Initialize(HWND messageWindow, HINSTANCE instance, const Handlers& handlers);

bool InitializeWithStartupDelay(HWND messageWindow,
                                HINSTANCE instance,
                                const Handlers& handlers,
                                DWORD delayMs = 2000);

bool RefreshTrayIcon();

void Shutdown();

void ShowNotification(const wchar_t* title, const wchar_t* message);

LRESULT CALLBACK MessageWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

} // namespace Tray