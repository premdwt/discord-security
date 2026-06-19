#pragma once

// token_clear.h — Scan dan pembersihan data token Discord (Menu 6).
#include "process_utils.h"

#include <string>
#include <vector>

namespace TokenClear {

// Jeda setelah kill proses sebelum pembersihan file (ms)
inline constexpr DWORD kPostKillDelayMs = 2000;

// Target yang terdeteksi memiliki data token Discord
struct DetectedTokenTarget {
    std::string displayName;
    std::wstring processExeName;

    // Path Discord Desktop (roaming + local)
    std::wstring roamingBasePath;
    std::wstring localBasePath;

    // Path profil browser (Chromium / Firefox)
    std::wstring profilePath;

    bool isFirefox = false;
    bool hasTokenData = false;
    bool processRunning = false;

    bool hasLevelDb = false;
    bool hasLocalState = false;
    int discordCookieCount = 0;
    bool hasDiscordLocalStorage = false;
    bool hasDiscordIndexedDb = false;

    std::string detailSummary;
};

// Hasil fase scan (deteksi saja, tanpa kill/clear)
struct TokenScanReport {
    std::vector<DetectedTokenTarget> targets;

    bool HasAnyTokenData() const;
    std::vector<std::wstring> GetProcessNamesToKill() const;
    int CountRunningProcesses() const;
};

// Hasil pembersihan per target
struct ClearTargetResult {
    std::string targetName;
    bool success = false;
    std::string message;
};

// Hasil eksekusi penuh (kill + clear)
struct TokenClearExecutionReport {
    std::vector<ProcessUtils::ExecutableKillResult> killResults;
    std::vector<ClearTargetResult> clearResults;

    bool AnyClearSuccess() const;
    bool HasClearFailures() const;
};

// Fase 1: scan lokasi token tanpa mengubah apapun
TokenScanReport ScanDiscordTokenLocations();

// Fase 2: kill proses terkait, tunggu, lalu bersihkan token yang terdeteksi
TokenClearExecutionReport ExecuteTokenClear(const TokenScanReport& scan);

} // namespace TokenClear