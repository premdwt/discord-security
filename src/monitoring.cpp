// monitoring.cpp — Thread background untuk pemantauan folder Discord dan scan proses.
#include "monitoring.h"
#include "console_color.h"
#include "logger.h"
#include "process_utils.h"
#include "utils.h"

#include <algorithm>
#include <sstream>
#include <tlhelp32.h>
#include <vector>

namespace Monitoring {

namespace {

// Mutex console — background thread WAJIB try_lock agar tidak deadlock dengan std::cin menu.
std::mutex g_consoleMutex;

std::atomic<bool> g_monitoringActive{false};

bool TryWriteToConsole(const std::string& line, bool leadingNewline) {
    std::unique_lock<std::mutex> lock(g_consoleMutex, std::try_to_lock);
    if (!lock.owns_lock()) {
        return false;
    }
    ConsoleColor::WriteLine(line, leadingNewline);
    return true;
}

template <typename Fn>
void RunThreadSafe(Fn&& fn, const char* context) {
    try {
        fn();
    } catch (const std::exception& ex) {
        Logger::Error(std::string(context) + " — exception: " + ex.what(),
                      Logger::LogFlags::FromBackground);
    } catch (...) {
        Logger::Error(std::string(context) + " — exception tidak dikenal.",
                      Logger::LogFlags::FromBackground);
    }
}

bool IsSuspiciousProcessName(const std::wstring& processName) {
    if (ProcessUtils::IsDiscordProcess(processName)) {
        return false;
    }

    for (const auto& keyword : ProcessUtils::GetSuspiciousKeywords()) {
        if (Utils::ContainsIgnoreCase(processName, keyword)) {
            return true;
        }
    }

    return false;
}

} // namespace

namespace ConsoleOutput {

void PrintLine(const std::string& line) {
    std::lock_guard<std::mutex> lock(g_consoleMutex);
    ConsoleColor::WriteLine(line, false);
}

void PrintFromBackground(const std::string& line) {
    const bool written = TryWriteToConsole(line, true);
    if (!written) {
        return;
    }

    if (g_monitoringActive.load()) {
        TryWriteToConsole(
            "[TIP] Monitoring aktif di background. "
            "Klik tray icon untuk buka console.",
            false);
    }
}

} // namespace ConsoleOutput

// ---------------------------------------------------------------------------
// FolderWatcher
// ---------------------------------------------------------------------------

FolderWatcher::FolderWatcher(std::wstring directory)
    : m_directory(std::move(directory)) {}

FolderWatcher::~FolderWatcher() {
    Stop();
}

// Memulai thread pemantauan direktori (ReadDirectoryChangesW).
bool FolderWatcher::Start(FolderChangeCallback callback) {
    if (m_running.load()) {
        return true;
    }

    if (m_thread.joinable()) {
        m_thread.join();
    }

    m_callback = std::move(callback);
    m_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!m_stopEvent) {
        Logger::LogWin32Error("Folder watcher: gagal membuat stop event");
        return false;
    }

    m_running.store(true);
    m_thread = std::thread([this]() {
        RunThreadSafe([this]() { WatchLoop(); }, "Folder watcher thread");
    });
    return true;
}

void FolderWatcher::Stop() {
    if (!m_running.load() && !m_thread.joinable()) {
        return;
    }

    m_running.store(false);

    if (m_stopEvent) {
        SetEvent(m_stopEvent);
    }

    if (m_dirHandle != INVALID_HANDLE_VALUE) {
        CancelIoEx(m_dirHandle, &m_overlapped);
    }

    if (m_thread.joinable()) {
        m_thread.join();
    }

    if (m_overlapped.hEvent) {
        CloseHandle(m_overlapped.hEvent);
        m_overlapped.hEvent = nullptr;
    }

    if (m_dirHandle != INVALID_HANDLE_VALUE) {
        CloseHandle(m_dirHandle);
        m_dirHandle = INVALID_HANDLE_VALUE;
    }

    if (m_stopEvent) {
        CloseHandle(m_stopEvent);
        m_stopEvent = nullptr;
    }
}

void FolderWatcher::NotifyChange(const std::wstring& relativePath,
                                 const std::wstring& action) {
    if (!m_callback) {
        return;
    }

    const std::wstring fullPath = m_directory + L"\\" + relativePath;
    m_callback(m_directory, fullPath, action);
}

void FolderWatcher::DispatchChanges(const BYTE* buffer, DWORD bytesReturned) {
    if (!buffer || bytesReturned < sizeof(FILE_NOTIFY_INFORMATION)) {
        return;
    }

    const BYTE* end = buffer + bytesReturned;
    const BYTE* ptr = buffer;

    while (ptr < end) {
        const auto* info = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(ptr);

        if (ptr + sizeof(FILE_NOTIFY_INFORMATION) > end) {
            break;
        }

        const DWORD nameBytes = info->FileNameLength;
        if (nameBytes == 0 || nameBytes % sizeof(wchar_t) != 0) {
            break;
        }

        const DWORD nameChars = nameBytes / sizeof(wchar_t);
        if (ptr + sizeof(FILE_NOTIFY_INFORMATION) + nameBytes > end) {
            break;
        }

        std::wstring fileName(info->FileName, nameChars);

        std::wstring action;
        switch (info->Action) {
            case FILE_ACTION_ADDED:            action = L"ADDED"; break;
            case FILE_ACTION_REMOVED:          action = L"REMOVED"; break;
            case FILE_ACTION_MODIFIED:         action = L"MODIFIED"; break;
            case FILE_ACTION_RENAMED_OLD_NAME: action = L"RENAMED_OLD"; break;
            case FILE_ACTION_RENAMED_NEW_NAME: action = L"RENAMED_NEW"; break;
            default:                           action = L"UNKNOWN"; break;
        }

        NotifyChange(fileName, action);

        if (info->NextEntryOffset == 0) {
            break;
        }

        if (info->NextEntryOffset < sizeof(FILE_NOTIFY_INFORMATION) ||
            ptr + info->NextEntryOffset >= end) {
            break;
        }

        ptr += info->NextEntryOffset;
    }
}

void FolderWatcher::WatchLoop() {
    m_dirHandle = CreateFileW(
        m_directory.c_str(),
        FILE_LIST_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
        nullptr);

    if (m_dirHandle == INVALID_HANDLE_VALUE) {
        Logger::LogWin32Error(
            "Folder watcher: gagal membuka direktori Discord — " +
            Utils::WideToUtf8(m_directory));
        m_running.store(false);
        return;
    }

    m_overlapped = {};
    m_overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!m_overlapped.hEvent) {
        Logger::LogWin32Error("Folder watcher: gagal membuat overlapped event");
        m_running.store(false);
        return;
    }

    constexpr DWORD kBufferSize = 64 * 1024;
    std::vector<BYTE> buffer(kBufferSize);
    const HANDLE waitHandles[2] = { m_stopEvent, m_overlapped.hEvent };

    Logger::Info("Memantau folder: " + Utils::WideToUtf8(m_directory));

    while (m_running.load()) {
        ResetEvent(m_overlapped.hEvent);

        const BOOL started = ReadDirectoryChangesW(
            m_dirHandle,
            buffer.data(),
            kBufferSize,
            TRUE,
            FILE_NOTIFY_CHANGE_FILE_NAME |
                FILE_NOTIFY_CHANGE_DIR_NAME |
                FILE_NOTIFY_CHANGE_SIZE |
                FILE_NOTIFY_CHANGE_LAST_WRITE,
            nullptr,
            &m_overlapped,
            nullptr);

        if (!started) {
            const DWORD err = GetLastError();
            if (err != ERROR_IO_PENDING) {
                Logger::LogWin32Error(
                    "Folder watcher: ReadDirectoryChangesW gagal — " +
                    Utils::WideToUtf8(m_directory),
                    err);
                Sleep(1000);
                continue;
            }
        }

        const DWORD waitResult =
            WaitForMultipleObjects(2, waitHandles, FALSE, INFINITE);

        if (waitResult == WAIT_OBJECT_0 || !m_running.load()) {
            break;
        }

        DWORD bytesReturned = 0;
        if (!GetOverlappedResult(m_dirHandle, &m_overlapped,
                                 &bytesReturned, FALSE)) {
            const DWORD err = GetLastError();
            if (err == ERROR_OPERATION_ABORTED || !m_running.load()) {
                break;
            }
            Logger::LogWin32Error("Folder watcher: GetOverlappedResult gagal", err);
            Sleep(500);
            continue;
        }

        if (bytesReturned > 0) {
            DispatchChanges(buffer.data(), bytesReturned);
        }
    }

    m_running.store(false);
}

// ---------------------------------------------------------------------------
// ProcessScanner — thread background independen dari console
// ---------------------------------------------------------------------------

ProcessScanner::ProcessScanner() = default;

ProcessScanner::~ProcessScanner() {
    Stop();
}

// Memulai thread polling proses mencurigakan.
bool ProcessScanner::Start() {
    if (m_running.load() && m_thread.joinable()) {
        return true;
    }

    if (m_thread.joinable()) {
        m_running.store(false);
        m_thread.join();
    }

    m_running.store(true);
    m_thread = std::thread([this]() {
        RunThreadSafe([this]() { ScanLoop(); }, "Process scanner thread");
    });
    return m_thread.joinable();
}

void ProcessScanner::Stop() {
    if (!m_running.load() && !m_thread.joinable()) {
        return;
    }

    m_running.store(false);

    if (m_thread.joinable()) {
        m_thread.join();
    }
}

// Satu putaran scan + kill berdasarkan keyword nama proses.
int ProcessScanner::ScanAndKillSuspiciousSafe() {
    int killed = 0;
    int scanned = 0;

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        Logger::LogWin32Error("Process scanner: CreateToolhelp32Snapshot gagal");
        m_lastScannedCount.store(0);
        return 0;
    }

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);

    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (!m_running.load()) {
                break;
            }

            ++scanned;

            const DWORD pid = entry.th32ProcessID;
            if (pid == 0 || pid <= 4) {
                continue;
            }

            const std::wstring processName = entry.szExeFile;
            if (!IsSuspiciousProcessName(processName)) {
                continue;
            }

            std::wstring error;
            if (ProcessUtils::KillProcess(pid, error)) {
                ++killed;
                Logger::LogProcessKilled(
                    Utils::WideToUtf8(processName),
                    pid,
                    "Suspicious process name",
                    "Monitoring");
            } else {
                Logger::Warning(
                    "Monitoring: gagal kill " + Utils::WideToUtf8(processName) +
                    " (PID: " + std::to_string(pid) + ") — " +
                    Utils::WideToUtf8(error),
                    Logger::LogFlags::FromBackground);
            }
        } while (Process32NextW(snapshot, &entry));
    }

    CloseHandle(snapshot);
    m_lastScannedCount.store(scanned);
    return killed;
}

void ProcessScanner::ScanLoop() {
    Logger::Info("Process scanner thread dimulai (interval: " +
                 std::to_string(m_intervalSec) + "s) — independen dari console UI.");

    int heartbeatCounter = 0;
    const int heartbeatEvery =
        std::max(1, kStatusHeartbeatSec / m_intervalSec);

    while (m_running.load()) {
        m_scanGeneration.fetch_add(1, std::memory_order_relaxed);
        ScanAndKillSuspiciousSafe();

        ++heartbeatCounter;
        if (heartbeatCounter >= heartbeatEvery) {
            heartbeatCounter = 0;
            const int scanned = m_lastScannedCount.load();
            std::ostringstream oss;
            oss << "Background monitoring is still active... (scanned "
                << scanned << " processes, interval "
                << m_intervalSec << "s)";
            Logger::Info(oss.str());
        }

        for (int i = 0; i < m_intervalSec * 10 && m_running.load(); ++i) {
            Sleep(100);
        }
    }

    Logger::Info("Process scanner thread berhenti.");
}

// ---------------------------------------------------------------------------
// SentinelMonitor
// ---------------------------------------------------------------------------

SentinelMonitor::SentinelMonitor() = default;

SentinelMonitor::~SentinelMonitor() {
    Stop();
}

void SentinelMonitor::SetProcessScanInterval(int seconds) {
    if (seconds >= 3 && seconds <= 60) {
        m_processScanner.SetIntervalSeconds(seconds);
    }
}

BackgroundStatus SentinelMonitor::GetBackgroundStatus() const {
    BackgroundStatus status{};
    status.monitorRunning = m_running.load();
    status.processScannerRunning = m_processScanner.IsRunning();
    status.lastScannedProcesses = m_processScanner.GetLastScannedProcessCount();
    status.scanGeneration = m_processScanner.GetScanGeneration();

    for (const auto& watcher : m_watchers) {
        if (watcher && watcher->IsRunning()) {
            ++status.folderWatchersActive;
        }
    }

    return status;
}

void SentinelMonitor::EnsureProcessScannerAlive() {
    if (!m_running.load()) {
        return;
    }

    if (m_processScanner.IsRunning() && m_processScanner.GetScanGeneration() > 0) {
        return;
    }

    Logger::Warning("Process scanner tidak aktif — mencoba restart thread.");
    m_processScanner.Stop();
    if (!m_processScanner.Start()) {
        Logger::Error("Gagal restart process scanner thread.");
    }
}

void SentinelMonitor::SupervisorLoop() {
    std::uint64_t lastGeneration = m_processScanner.GetScanGeneration();
    int staleChecks = 0;

    while (m_supervisorRunning.load()) {
        Sleep(10000);

        if (!m_running.load()) {
            break;
        }

        const std::uint64_t currentGeneration =
            m_processScanner.GetScanGeneration();

        if (currentGeneration == lastGeneration) {
            ++staleChecks;
            if (staleChecks >= 3) {
                Logger::Warning(
                    "Supervisor: process scanner tampak tidak berjalan — restart.");
                EnsureProcessScannerAlive();
                staleChecks = 0;
            }
        } else {
            staleChecks = 0;
            lastGeneration = currentGeneration;
        }
    }
}

// Memulai semua watcher + process scanner (idempotent).
bool SentinelMonitor::Start() {
    if (m_running.load()) {
        Logger::Info("Monitoring sudah aktif — thread background tidak di-restart.");
        return true;
    }

    try {
        if (!Utils::EnableDebugPrivilege()) {
            Logger::Warning(
                "SeDebugPrivilege tidak aktif — kemampuan kill/inspect proses terbatas.");
        }

        const auto paths = Utils::GetDiscordWatchPaths();
        if (paths.empty()) {
            Logger::Warning(
                "Folder Discord tidak ditemukan — monitoring folder tidak aktif.");
        } else {
            Logger::Info(std::to_string(paths.size()) + " folder Discord dipantau.");
        }

        int watchersStarted = 0;
        for (const auto& path : paths) {
            auto watcher = std::make_unique<FolderWatcher>(path);
            if (watcher->Start([this](const std::wstring& dir,
                                      const std::wstring& filePath,
                                      const std::wstring& action) {
                    OnFolderChange(dir, filePath, action);
                })) {
                m_watchers.push_back(std::move(watcher));
                ++watchersStarted;
            } else {
                Logger::Warning("Gagal memulai watcher: " + Utils::WideToUtf8(path));
            }
        }

        if (!paths.empty() && watchersStarted == 0) {
            Logger::Warning(
                "Folder watcher gagal — process monitoring tetap berjalan di background.");
        }

        if (!m_processScanner.Start()) {
            Logger::Error("Gagal memulai process scanner thread.");
            Stop();
            return false;
        }

        m_running.store(true);
        g_monitoringActive.store(true);

        m_supervisorRunning.store(true);
        if (m_supervisorThread.joinable()) {
            m_supervisorThread.join();
        }
        m_supervisorThread = std::thread([this]() {
            RunThreadSafe([this]() { SupervisorLoop(); }, "Monitoring supervisor thread");
        });

        const int scanInterval = m_processScanner.GetIntervalSeconds();
        Logger::Info("Monitoring dimulai. " + std::to_string(watchersStarted) +
                     " folder dipantau, scan interval " +
                     std::to_string(scanInterval) + "s.");
        Logger::Info("Background monitoring thread aktif — independen dari console window.");

        Logger::Info(
            "Monitoring berjalan di background. Buka console dari tray untuk aksi lain.",
            Logger::LogFlags::ForceConsole);
        return true;
    } catch (const std::exception& ex) {
        Logger::Error(std::string("Start monitoring exception: ") + ex.what(),
                      Logger::LogFlags::ForceConsole);
        Stop();
        return false;
    } catch (...) {
        Logger::Error("Start monitoring exception tidak dikenal.",
                      Logger::LogFlags::ForceConsole);
        Stop();
        return false;
    }
}

void SentinelMonitor::Stop() {
    // Hanya dipanggil saat user memilih Exit dari tray — bukan saat hide console.
    const bool wasActive = m_running.exchange(false);
    g_monitoringActive.store(false);

    m_supervisorRunning.store(false);
    if (m_supervisorThread.joinable()) {
        m_supervisorThread.join();
    }

    for (auto& watcher : m_watchers) {
        if (watcher) {
            watcher->Stop();
        }
    }
    m_watchers.clear();

    m_processScanner.Stop();

    if (wasActive) {
        Logger::Info("Monitoring dihentikan.", Logger::LogFlags::ForceConsole);
    } else {
        Logger::Info("Monitoring dihentikan.");
    }
}

void SentinelMonitor::OnFolderChange(const std::wstring& watchDir,
                                     const std::wstring& filePath,
                                     const std::wstring& action) {
    const std::string dirUtf8 = Utils::WideToUtf8(watchDir);
    const std::string pathUtf8 = Utils::WideToUtf8(filePath);
    const std::string actionUtf8 = Utils::WideToUtf8(action);

    Logger::Debug("Folder [" + actionUtf8 + "] " + pathUtf8 +
                  " (watch: " + dirUtf8 + ")");
}

} // namespace Monitoring