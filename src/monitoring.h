#pragma once

// monitoring.h — Background monitoring: folder watcher + process scanner.
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <windows.h>

namespace Monitoring {

// Interval heartbeat background (detik) — ditulis ke file log
inline constexpr int kStatusHeartbeatSec = 30;

// ---------------------------------------------------------------------------
// ConsoleOutput — output console thread-safe (tidak boleh memblokir monitoring)
// ---------------------------------------------------------------------------
namespace ConsoleOutput {

void PrintLine(const std::string& line);

// Pesan dari background thread (try_lock — tidak memblokir scanner).
void PrintFromBackground(const std::string& line);

} // namespace ConsoleOutput

using FolderChangeCallback =
    std::function<void(const std::wstring& watchDir,
                       const std::wstring& filePath,
                       const std::wstring& action)>;

// Monitor satu direktori — thread independen, tidak terkait console UI
class FolderWatcher {
public:
    explicit FolderWatcher(std::wstring directory);
    ~FolderWatcher();

    FolderWatcher(const FolderWatcher&) = delete;
    FolderWatcher& operator=(const FolderWatcher&) = delete;

    bool Start(FolderChangeCallback callback);
    void Stop();

    const std::wstring& Directory() const { return m_directory; }
    bool IsRunning() const { return m_running.load(); }

private:
    void WatchLoop();
    void DispatchChanges(const BYTE* buffer, DWORD bytesReturned);
    void NotifyChange(const std::wstring& relativePath, const std::wstring& action);

    std::wstring m_directory;
    std::atomic<bool> m_running{false};
    std::thread m_thread;

    HANDLE m_stopEvent = nullptr;
    HANDLE m_dirHandle = INVALID_HANDLE_VALUE;
    OVERLAPPED m_overlapped{};
    FolderChangeCallback m_callback;
};

// Thread polling proses — berjalan mandiri meski console di-hide
class ProcessScanner {
public:
    ProcessScanner();
    ~ProcessScanner();

    ProcessScanner(const ProcessScanner&) = delete;
    ProcessScanner& operator=(const ProcessScanner&) = delete;

    bool Start();
    void Stop();

    void SetIntervalSeconds(int seconds) { m_intervalSec = seconds; }
    int GetIntervalSeconds() const { return m_intervalSec; }
    bool IsRunning() const { return m_running.load(); }

    int GetLastScannedProcessCount() const { return m_lastScannedCount.load(); }
    std::uint64_t GetScanGeneration() const { return m_scanGeneration.load(); }

private:
    void ScanLoop();
    int ScanAndKillSuspiciousSafe();

    std::atomic<bool> m_running{false};
    std::atomic<int> m_lastScannedCount{0};
    std::atomic<std::uint64_t> m_scanGeneration{0};
    std::thread m_thread;
    int m_intervalSec = 5;
};

struct BackgroundStatus {
    bool monitorRunning = false;
    bool processScannerRunning = false;
    int folderWatchersActive = 0;
    int lastScannedProcesses = 0;
    std::uint64_t scanGeneration = 0;
};

// Orchestrator — Start()/Stop() eksplisit; hide console tidak menghentikan thread.
class SentinelMonitor {
public:
    SentinelMonitor();
    ~SentinelMonitor();

    // Idempotent — tidak membuat thread duplikat jika sudah aktif
    bool Start();
    void Stop();

    bool IsRunning() const { return m_running.load(); }
    BackgroundStatus GetBackgroundStatus() const;

    void SetProcessScanInterval(int seconds);

private:
    void OnFolderChange(const std::wstring& watchDir,
                        const std::wstring& filePath,
                        const std::wstring& action);
    void SupervisorLoop();
    void EnsureProcessScannerAlive();

    std::atomic<bool> m_running{false};
    std::atomic<bool> m_supervisorRunning{false};
    std::vector<std::unique_ptr<FolderWatcher>> m_watchers;
    ProcessScanner m_processScanner;
    std::thread m_supervisorThread;
};

} // namespace Monitoring