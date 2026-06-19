#include "console_window.h"
#include "app_version.h"
#include "logger.h"

#include <cstdio>
#include <iostream>

namespace AppConsole {

namespace {

bool g_initialized = false;
HWND g_notifyWindow = nullptr;
CloseCallback g_closeCallback;
WNDPROC g_originalConsoleWndProc = nullptr;

void RequestConsoleHide() {
    if (g_notifyWindow) {
        PostMessageW(g_notifyWindow, WM_APP_CONSOLE_HIDE, 0, 0);
        return;
    }

    if (g_closeCallback) {
        g_closeCallback();
        return;
    }

    Hide();
}

BOOL WINAPI ConsoleCtrlHandler(DWORD ctrlType) {
    switch (ctrlType) {
        case CTRL_CLOSE_EVENT:
            // Klik tombol X — sembunyikan console, jangan terminate proses.
            RequestConsoleHide();
            return TRUE;

        default:
            return FALSE;
    }
}

LRESULT CALLBACK ConsoleWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_CLOSE) {
        RequestConsoleHide();
        return 0;
    }

    if (msg == WM_SYSCOMMAND && (wParam & 0xFFF0) == SC_CLOSE) {
        RequestConsoleHide();
        return 0;
    }

    if (g_originalConsoleWndProc) {
        return CallWindowProcW(g_originalConsoleWndProc, hwnd, msg, wParam, lParam);
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void HookConsoleWindowProc() {
    const HWND consoleWnd = GetConsoleWindow();
    if (!consoleWnd || g_originalConsoleWndProc) {
        return;
    }

    g_originalConsoleWndProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
        consoleWnd,
        GWLP_WNDPROC,
        reinterpret_cast<LONG_PTR>(ConsoleWindowProc)));

    if (!g_originalConsoleWndProc) {
        Logger::LogWin32Error("Console: gagal subclass console window");
    }
}

// SetForegroundWindow sering gagal dari tray/background — pakai AttachThreadInput.
void BringConsoleToForeground(HWND hwnd) {
    if (!hwnd) {
        return;
    }

    HWND foreground = GetForegroundWindow();
    const DWORD foregroundThread =
        foreground ? GetWindowThreadProcessId(foreground, nullptr) : 0;
    const DWORD currentThread = GetCurrentThreadId();

    if (foregroundThread != 0 && foregroundThread != currentThread) {
        AttachThreadInput(currentThread, foregroundThread, TRUE);
    }

    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);

    if (foregroundThread != 0 && foregroundThread != currentThread) {
        AttachThreadInput(currentThread, foregroundThread, FALSE);
    }
}

bool AttachStandardStreams() {
    FILE* stdoutFile = nullptr;
    FILE* stderrFile = nullptr;
    FILE* stdinFile = nullptr;

    if (freopen_s(&stdoutFile, "CONOUT$", "w", stdout) != 0 || !stdoutFile) {
        Logger::LogWin32Error("Console: gagal menghubungkan stdout");
        return false;
    }
    if (freopen_s(&stderrFile, "CONOUT$", "w", stderr) != 0 || !stderrFile) {
        Logger::LogWin32Error("Console: gagal menghubungkan stderr");
        return false;
    }
    if (freopen_s(&stdinFile, "CONIN$", "r", stdin) != 0 || !stdinFile) {
        Logger::LogWin32Error("Console: gagal menghubungkan stdin");
        return false;
    }

    std::ios::sync_with_stdio(false);
    std::cout.clear();
    std::cerr.clear();
    std::cin.clear();

    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    SetConsoleTitleW(AppVersion::kConsoleTitle);

    return true;
}

} // namespace

bool InitializeHidden() {
    if (g_initialized) {
        return true;
    }

    if (!AllocConsole()) {
        if (GetConsoleWindow() != nullptr) {
            g_initialized = true;
            if (!AttachStandardStreams()) {
                return false;
            }
            InstallCloseProtection();
            const HWND consoleWnd = GetConsoleWindow();
            if (consoleWnd) {
                ShowWindow(consoleWnd, SW_HIDE);
            }
            return true;
        }
        Logger::LogWin32Error("Console: AllocConsole gagal");
        return false;
    }

    if (!AttachStandardStreams()) {
        FreeConsole();
        return false;
    }

    InstallCloseProtection();

    const HWND consoleWnd = GetConsoleWindow();
    if (consoleWnd) {
        ShowWindow(consoleWnd, SW_HIDE);
    }

    g_initialized = true;
    Logger::Info("Console dialokasikan (hidden) — monitoring tetap di background.");
    return true;
}

void Show() {
    if (!g_initialized) {
        if (!InitializeHidden()) {
            return;
        }
    }

    BringConsoleToForeground(GetConsoleWindow());
}

void Hide() {
    const HWND consoleWnd = GetConsoleWindow();
    if (consoleWnd) {
        ShowWindow(consoleWnd, SW_HIDE);
    }
}

void Shutdown() {
    if (!g_initialized) {
        return;
    }

    Hide();
    FreeConsole();
    g_initialized = false;
    g_originalConsoleWndProc = nullptr;
}

bool IsReady() {
    return g_initialized;
}

HWND GetWindowHandle() {
    return GetConsoleWindow();
}

void SetNotifyWindow(HWND hwnd) {
    g_notifyWindow = hwnd;
}

void SetOnCloseRequested(CloseCallback callback) {
    g_closeCallback = std::move(callback);
}

void InstallCloseProtection() {
    if (!SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE)) {
        Logger::LogWin32Error("Console: SetConsoleCtrlHandler gagal");
    }
    HookConsoleWindowProc();
}

} // namespace AppConsole