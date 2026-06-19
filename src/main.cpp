// main.cpp — Entry point Win32: tray icon, console UI, dan monitoring background.
#include "app_version.h"
#include "console_window.h"
#include "dashboard.h"
#include "logger.h"
#include "monitoring.h"
#include "tray.h"
#include "utils.h"

#include <memory>
#include <sstream>
#include <string>

namespace {

constexpr wchar_t kSingleInstanceMutexName[] = L"DiscordSentinel_SingleInstance_Mutex";
constexpr DWORD kTrayStartupDelayMs = 2000;

// Mengelola lifecycle aplikasi: monitoring, console, dan system tray.
struct Application {
    HINSTANCE instance = nullptr;
    HWND messageWindow = nullptr;

    Monitoring::SentinelMonitor monitor;
    std::unique_ptr<Dashboard::ConsoleDashboard> dashboard;

    // Menampilkan console dari tray (klik kiri / menu Show Console).
    void OnShowConsole() {
        if (dashboard) {
            dashboard->Show();
        }
    }

    // Menyembunyikan console — monitoring tetap berjalan di background.
    void OnMinimizeToTray() {
        if (dashboard) {
            dashboard->Hide();
        } else {
            AppConsole::Hide();
        }

        const auto status = monitor.GetBackgroundStatus();
        Logger::Info(
            "Console disembunyikan. Monitoring " +
            std::string(status.monitorRunning ? "AKTIF" : "NONAKTIF") +
            " (scan gen: " + std::to_string(status.scanGeneration) + ").");

        Tray::ShowNotification(
            L"Discord Sentinel",
            L"Console disembunyikan. Monitoring tetap aktif di background.");
    }

    // Exit penuh — hanya dari menu tray.
    void OnExit() {
        Logger::Info("Exit diminta dari system tray.");

        if (monitor.IsRunning()) {
            monitor.Stop();
        }

        if (dashboard) {
            dashboard->Shutdown();
            dashboard.reset();
        }

        Tray::Shutdown();
        AppConsole::Shutdown();
        Logger::Shutdown();
    }

    bool Initialize() {
        if (!Logger::Initialize()) {
            MessageBoxW(nullptr,
                        L"Gagal menginisialisasi logger.",
                        L"Discord Sentinel",
                        MB_ICONERROR | MB_OK);
            return false;
        }

        Logger::Info(AppVersion::DisplayString() + " started (Win32 tray mode).");

        if (!Utils::IsRunningAsAdministrator()) {
            Logger::Warning(
                "Aplikasi tidak berjalan sebagai Administrator — beberapa fitur terbatas.");
        }

        messageWindow = Tray::CreateMessageWindow(instance);
        if (!messageWindow) {
            Logger::Error("Gagal membuat message window.");
            return false;
        }

        AppConsole::SetNotifyWindow(messageWindow);
        AppConsole::SetOnCloseRequested([this]() { OnMinimizeToTray(); });

        if (!AppConsole::InitializeHidden()) {
            Logger::Error("Gagal menginisialisasi console window.");
            return false;
        }

        dashboard = std::make_unique<Dashboard::ConsoleDashboard>(monitor);

        if (!monitor.Start()) {
            Logger::Warning("Monitoring gagal dimulai — buka console untuk mencoba lagi.");
        } else {
            Logger::Info("Monitoring background aktif (independen dari console/tray).");
        }

        Tray::Handlers handlers{};
        handlers.onShowConsole = [this]() { OnShowConsole(); };
        handlers.onMinimizeToTray = [this]() { OnMinimizeToTray(); };
        handlers.onExit = [this]() { OnExit(); };

        if (!Tray::InitializeWithStartupDelay(
                messageWindow, instance, handlers, kTrayStartupDelayMs)) {
            Logger::Error("Gagal menginisialisasi system tray.");
            return false;
        }

        if (monitor.IsRunning()) {
            Tray::ShowNotification(
                L"Discord Sentinel",
                L"Monitoring aktif. Klik tray icon untuk buka console.");
        } else {
            Tray::ShowNotification(
                L"Discord Sentinel",
                L"Monitoring gagal dimulai. Buka console dari tray.");
        }

        return true;
    }
};

} // namespace

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int) {
    HANDLE instanceMutex = CreateMutexW(nullptr, TRUE, kSingleInstanceMutexName);
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(nullptr,
                    L"Discord Sentinel sudah berjalan di system tray.",
                    L"Discord Sentinel",
                    MB_ICONINFORMATION | MB_OK);
        if (instanceMutex) {
            CloseHandle(instanceMutex);
        }
        return 0;
    }

    Application app{};
    app.instance = hInstance;

    if (!app.Initialize()) {
        AppConsole::Shutdown();
        Logger::Shutdown();
        if (instanceMutex) {
            ReleaseMutex(instanceMutex);
            CloseHandle(instanceMutex);
        }
        return 1;
    }

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (instanceMutex) {
        ReleaseMutex(instanceMutex);
        CloseHandle(instanceMutex);
    }

    if (app.messageWindow) {
        DestroyWindow(app.messageWindow);
    }

    return 0;
}