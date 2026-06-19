// dashboard.cpp — Console menu interaktif (terpisah dari thread monitoring).
#include "dashboard.h"
#include "app_version.h"
#include "console_color.h"
#include "console_window.h"
#include "logger.h"
#include "process_utils.h"
#include "token_clear.h"
#include "utils.h"

#include <conio.h>
#include <iostream>
#include <limits>
#include <string>

namespace Dashboard {

namespace {

ConsoleDashboard* g_activeDashboard = nullptr;

void FlushConsoleInput() {
    std::cin.clear();

    if (std::cin.rdbuf()->in_avail() > 0) {
        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    }

    const HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    if (input == INVALID_HANDLE_VALUE || input == nullptr) {
        return;
    }

    DWORD pending = 0;
    while (GetNumberOfConsoleInputEvents(input, &pending) && pending > 0) {
        INPUT_RECORD records[16]{};
        DWORD read = 0;
        const DWORD toRead = (pending > 16) ? 16 : pending;
        if (!ReadConsoleInputW(input, records, toRead, &read) || read == 0) {
            break;
        }
    }
}

void PrintBanner() {
    ConsoleColor::Reset();
    std::cout << R"(
  ____  _                       _   ____             _            _
 |  _ \(_)___  ___ ___  _ __ __| | / ___|  ___ _ __ | |_ _ __ __ _| |_ ___  _ __
 | | | | / __|/ __/ _ \| '__/ _` | \___ \ / _ \ '_ \| __| '__/ _` | __/ _ \| '__|
 | |_| | \__ \ (_| (_) | | | (_| |  ___) |  __/ | | | |_| | | (_| | || (_) | |
 |____/|_|___/\___\___/|_|  \__,_| |____/ \___|_| |_|\__|_|  \__,_|\__\___/|_|

              )" << AppVersion::kVersionLabel << R"( — Token Stealer Protection
)" << std::endl;
}

void PrintMenu(bool monitoringActive) {
    std::cout << '\n';
    ConsoleColor::SetColor(static_cast<WORD>(ConsoleColor::Attr::Status));
    if (monitoringActive) {
        std::cout << "----------------------------------------\n"
                  << "  [AKTIF] Monitoring berjalan di background\n"
                  << "  Tutup window (X) / Minimize to Tray = hide console\n"
                  << "  Monitoring hanya berhenti saat Exit dari tray\n"
                  << "----------------------------------------\n";
    } else {
        std::cout << "----------------------------------------\n"
                  << "  [NONAKTIF] Monitoring tidak berjalan\n"
                  << "----------------------------------------\n";
    }
    ConsoleColor::Reset();

    std::cout << "=== Menu ===\n"
              << "  [1] Mulai Monitoring\n"
              << "  [2] Hentikan Monitoring\n"
              << "  [3] Scan Proses Mencurigakan\n"
              << "  [4] Info Log & Folder Discord\n"
              << "  [5] Sembunyikan ke Tray\n"
              << "  [6] Clear Discord Tokens\n";
    std::cout.flush();
}

// Input non-blocking — hindari std::cin blokir thread menu & std::cout monitoring.
bool ReadMenuChoice(int& choice,
                    const std::atomic<bool>& visible,
                    const std::atomic<bool>& appRunning) {
    ConsoleColor::WritePrompt("Pilih: ");

    while (appRunning.load()) {
        if (!visible.load()) {
            return false;
        }

        if (_kbhit()) {
            const int key = _getch();

            if (key >= '1' && key <= '6') {
                choice = key - '0';
                std::cout << static_cast<char>(key) << '\n';
                std::cout.flush();
                return true;
            }

            if (key == '\r' || key == '\n' || key == 3) {
                continue;
            }

            std::cout << '\n';
            Logger::Warning("Input menu tidak valid — masukkan angka 1-6.",
                            Logger::LogFlags::ForceConsole);
            ConsoleColor::WritePrompt("Pilih: ");
        }

        Sleep(50);
    }

    return false;
}

bool ReadYesNoConfirm(char& confirm,
                      const std::atomic<bool>& visible,
                      const std::atomic<bool>& appRunning) {
    while (appRunning.load()) {
        if (!visible.load()) {
            return false;
        }

        if (_kbhit()) {
            confirm = static_cast<char>(_getch());
            if (confirm == '\r' || confirm == '\n') {
                continue;
            }
            std::cout << confirm << '\n';
            std::cout.flush();
            return true;
        }

        Sleep(50);
    }

    return false;
}

void LogManualScanSummary(const ProcessUtils::ManualScanResult& result) {
    Logger::Info("--- Ringkasan Scan Manual ---", Logger::LogFlags::ForceConsole);
    Logger::Info("  Discan     : " + std::to_string(result.scanned),
                 Logger::LogFlags::ForceConsole);
    Logger::Info("  Mencurigakan: " + std::to_string(result.found),
                 Logger::LogFlags::ForceConsole);
    Logger::Info("  Dihentikan : " + std::to_string(result.killed),
                 Logger::LogFlags::ForceConsole);
    if (result.failed > 0) {
        Logger::Warning("  Gagal kill : " + std::to_string(result.failed),
                        Logger::LogFlags::ForceConsole);
    }
}

} // namespace

ConsoleDashboard::ConsoleDashboard(Monitoring::SentinelMonitor& monitor)
    : m_monitor(monitor) {
    g_activeDashboard = this;
}

ConsoleDashboard::~ConsoleDashboard() {
    Shutdown();
    if (g_activeDashboard == this) {
        g_activeDashboard = nullptr;
    }
}

void ConsoleDashboard::EnsureConsoleReady() {
    if (m_consoleReady.load()) {
        return;
    }

    if (!AppConsole::IsReady()) {
        Logger::Error("Console belum dialokasikan — tidak bisa menampilkan menu.");
        return;
    }

    Logger::SetConsoleSink([](const std::string& line) {
        Monitoring::ConsoleOutput::PrintLine(line);
    });
    Logger::SetBackgroundConsoleSink([](const std::string& line) {
        Monitoring::ConsoleOutput::PrintFromBackground(line);
    });

    m_consoleReady.store(true);
}

void ConsoleDashboard::Show() {
    EnsureConsoleReady();
    if (!m_consoleReady.load()) {
        return;
    }

    AppConsole::Show();
    m_visible.store(true);

    if (!m_menuThread.joinable()) {
        m_menuThread = std::thread([this]() { MenuLoop(); });
    } else {
        PrintBanner();
        PrintMenu(m_monitor.IsRunning());
    }
}

void ConsoleDashboard::Hide() {
    m_visible.store(false);
    FlushConsoleInput();
    std::cin.clear();

    AppConsole::Hide();

    const auto status = m_monitor.GetBackgroundStatus();
    Logger::Info(
        "Console disembunyikan — monitoring tetap berjalan di background "
        "(scan generation: " + std::to_string(status.scanGeneration) + ").");
}

void ConsoleDashboard::Shutdown() {
    m_appRunning.store(false);
    m_visible.store(false);

    if (m_menuThread.joinable()) {
        m_menuThread.join();
    }

    m_consoleReady.store(false);
}

void ConsoleDashboard::WaitForReturnToMenu() {
    if (!m_visible.load()) {
        return;
    }

    ConsoleColor::WritePrompt("\nPress any key to return to main menu...");

    while (m_visible.load() && m_appRunning.load()) {
        if (_kbhit()) {
            (void)_getch();
            std::cout << '\n';
            return;
        }
        Sleep(50);
    }
}

void ConsoleDashboard::MenuLoop() {
    try {
        PrintBanner();

        if (!Utils::IsRunningAsAdministrator()) {
            Logger::Warning(
                "Aplikasi TIDAK berjalan sebagai Administrator. "
                "Beberapa fitur mungkin gagal.",
                Logger::LogFlags::ForceConsole);
        } else {
            Logger::Info("Hak Administrator terdeteksi.",
                         Logger::LogFlags::ForceConsole);
        }

        Logger::Info("Log file: " + Logger::GetLogFilePath(),
                     Logger::LogFlags::ForceConsole);

        while (m_appRunning.load()) {
            if (!m_visible.load()) {
                Sleep(200);
                continue;
            }

            PrintMenu(m_monitor.IsRunning());

            int choice = 0;
            if (!ReadMenuChoice(choice, m_visible, m_appRunning)) {
                continue;
            }

            bool pauseBeforeMenu = true;

            switch (choice) {
                case 1:
                    try {
                        if (m_monitor.IsRunning()) {
                            Logger::Info("Monitoring sudah berjalan.",
                                         Logger::LogFlags::ForceConsole);
                        } else if (!m_monitor.Start()) {
                            Logger::Error("Monitoring gagal dimulai.",
                                          Logger::LogFlags::ForceConsole);
                        }
                    } catch (...) {
                        Logger::Error("Monitoring start exception.",
                                      Logger::LogFlags::ForceConsole);
                    }
                    break;

                case 2:
                    try {
                        if (!m_monitor.IsRunning()) {
                            Logger::Info("Monitoring tidak aktif.",
                                         Logger::LogFlags::ForceConsole);
                        } else {
                            m_monitor.Stop();
                        }
                    } catch (...) {
                        Logger::Error("Monitoring stop exception.",
                                      Logger::LogFlags::ForceConsole);
                    }
                    break;

                case 3: {
                    try {
                        Logger::Info("[3] Memindai semua proses...",
                                     Logger::LogFlags::ForceConsole);
                        const auto scanResult = ProcessUtils::ScanAndKillSuspicious();
                        LogManualScanSummary(scanResult);
                    } catch (...) {
                        Logger::Error("Manual scan gagal — lihat log file untuk detail.",
                                      Logger::LogFlags::ForceConsole);
                    }
                    break;
                }

                case 4: {
                    Logger::Info("--- Info Sistem ---", Logger::LogFlags::ForceConsole);
                    Logger::Info("  Log file : " + Logger::GetLogFilePath(),
                                 Logger::LogFlags::ForceConsole);
                    Logger::Info("  Versi    : " + AppVersion::DisplayString(),
                                 Logger::LogFlags::ForceConsole);
                    const auto paths = Utils::GetDiscordWatchPaths();
                    Logger::Info("  Folder Discord (" + std::to_string(paths.size()) + "):",
                                 Logger::LogFlags::ForceConsole);
                    for (const auto& p : paths) {
                        Logger::Info("    - " + Utils::WideToUtf8(p),
                                     Logger::LogFlags::ForceConsole);
                    }
                    break;
                }

                case 5:
                    Hide();
                    pauseBeforeMenu = false;
                    break;

                case 6: {
                    try {
                        Monitoring::ConsoleOutput::PrintLine(
                            "[6] Memindai lokasi token Discord...");

                        const auto scanReport = TokenClear::ScanDiscordTokenLocations();

                        Monitoring::ConsoleOutput::PrintLine(
                            "--- Hasil Scan Token ---");

                        if (!scanReport.HasAnyTokenData()) {
                            Monitoring::ConsoleOutput::PrintLine(
                                "[INFO] Tidak ada data token Discord ditemukan.");
                            break;
                        }

                        int idx = 1;
                        for (const auto& target : scanReport.targets) {
                            if (!target.hasTokenData) continue;
                            Monitoring::ConsoleOutput::PrintLine(
                                "  " + std::to_string(idx++) + ". " +
                                target.displayName + " — " + target.detailSummary);
                        }

                        ConsoleColor::WritePrompt(
                            "\n[PERINGATAN] Token akan dihapus. Lanjutkan? (Y/N): ");

                        char confirm = '\0';
                        if (!ReadYesNoConfirm(confirm, m_visible, m_appRunning) ||
                            (confirm != 'Y' && confirm != 'y')) {
                            Monitoring::ConsoleOutput::PrintLine(
                                "[INFO] Pembersihan dibatalkan.");
                            break;
                        }

                        const auto execReport =
                            TokenClear::ExecuteTokenClear(scanReport);

                        Monitoring::ConsoleOutput::PrintLine(
                            "--- Hasil Pembersihan ---");
                        for (const auto& item : execReport.clearResults) {
                            const std::string prefix = item.success ? "[OK] " : "[GAGAL] ";
                            Monitoring::ConsoleOutput::PrintLine(
                                prefix + item.targetName + " — " + item.message);
                        }
                    } catch (...) {
                        Logger::Error("Token clear exception.",
                                      Logger::LogFlags::ForceConsole);
                    }
                    break;
                }

                default:
                    Logger::Warning("Pilihan tidak valid. Masukkan 1-6.",
                                    Logger::LogFlags::ForceConsole);
                    pauseBeforeMenu = false;
                    break;
            }

            if (pauseBeforeMenu && m_visible.load()) {
                WaitForReturnToMenu();
            } else {
                FlushConsoleInput();
            }
        }
    } catch (...) {
        Logger::Error("Dashboard menu loop exception.");
    }
}

} // namespace Dashboard