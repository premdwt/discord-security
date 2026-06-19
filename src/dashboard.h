#pragma once

#include "monitoring.h"

#include <atomic>
#include <thread>
#include <windows.h>

namespace Dashboard {

// Console dashboard — show/hide tidak mempengaruhi monitoring background
class ConsoleDashboard {
public:
    explicit ConsoleDashboard(Monitoring::SentinelMonitor& monitor);
    ~ConsoleDashboard();

    ConsoleDashboard(const ConsoleDashboard&) = delete;
    ConsoleDashboard& operator=(const ConsoleDashboard&) = delete;

    void Show();
    void Hide();

    bool IsVisible() const { return m_visible.load(); }

    void Shutdown();

private:
    void EnsureConsoleReady();
    void MenuLoop();
    void WaitForReturnToMenu();

    Monitoring::SentinelMonitor& m_monitor;
    std::atomic<bool> m_appRunning{true};
    std::atomic<bool> m_visible{false};
    std::atomic<bool> m_consoleReady{false};
    std::thread m_menuThread;
};

} // namespace Dashboard