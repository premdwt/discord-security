#pragma once

// process_utils.h — API enumerasi, verifikasi, dan kill proses.
#include <string>
#include <vector>
#include <windows.h>

namespace ProcessUtils {

struct ProcessInfo {
    DWORD pid = 0;
    std::wstring name;
    std::wstring fullPath;
    bool isSigned = false;
    bool isSuspicious = false;
    std::wstring suspicionReason;
};

// Entri proses yang terdeteksi pada scan manual
struct SuspiciousProcessEntry {
    std::wstring name;
    DWORD pid = 0;
    std::wstring reason;
    bool killed = false;
};

// Hasil verifikasi tanda tangan digital
struct DigitalSignatureResult {
    bool isValid = false;
    bool isTrustedPublisher = false;
    std::wstring publisher;
};

// Hasil scan manual (Menu 3)
struct ManualScanResult {
    int scanned = 0;
    int found = 0;
    int killed = 0;
    int failed = 0;
    std::vector<SuspiciousProcessEntry> entries;
};

// Daftar kata kunci mencurigakan pada nama proses
const std::vector<std::wstring>& GetSuspiciousKeywords();

// Proses Discord yang diizinkan mengakses folder Discord
bool IsDiscordProcess(const std::wstring& processName,
                      const std::wstring& fullPath = L"");

// Enumerasi semua proses yang sedang berjalan (signature dicek lazy bila diperlukan)
std::vector<ProcessInfo> EnumerateProcesses();

// Daftar proses Windows resmi yang tidak boleh dihentikan
const std::vector<std::wstring>& GetWindowsProcessWhitelist();

// Proses ada di whitelist Windows (case-insensitive)
bool IsWhitelistedWindowsProcess(const std::wstring& processName);

// Path executable berada di folder terpercaya (Windows, Program Files, SystemApps, dll.)
bool IsTrustedProcessLocation(const std::wstring& fullPath);

// Verifikasi tanda tangan digital + ekstrak nama publisher (WinVerifyTrust + CryptQueryObject)
DigitalSignatureResult VerifyDigitalSignature(const std::wstring& filePath);

// Publisher termasuk vendor terpercaya (mis. Microsoft)
bool IsTrustedPublisher(const std::wstring& publisher);

// Cek apakah file executable memiliki tanda tangan digital valid
bool IsDigitallySigned(const std::wstring& filePath);

// Proses ini adalah Discord Sentinel sendiri — jangan pernah dihentikan
bool IsSentinelProcess(DWORD pid, const std::wstring& processName = L"");

// Evaluasi apakah proses mencurigakan
// manualScan=true: aturan penuh (keyword + unsigned + lokasi mencurigakan)
bool EvaluateSuspicion(ProcessInfo& info,
                       bool checkSignature = true,
                       bool manualScan = false);

// Terminate proses berdasarkan PID
bool KillProcess(DWORD pid, std::wstring& errorOut);

// Cek apakah executable sedang berjalan (nama file, case-insensitive)
bool IsProcessRunning(const std::wstring& exeName);

// Temukan semua PID dengan nama executable tertentu
std::vector<DWORD> FindProcessIdsByExecutableName(const std::wstring& exeName);

// Hasil kill per nama executable
struct ExecutableKillResult {
    std::wstring exeName;
    int killed = 0;
    int failed = 0;
};

// Hentikan semua instance proses dengan TerminateProcess
ExecutableKillResult KillProcessesByExecutableName(const std::wstring& exeName);

// Hentikan beberapa jenis proses sekaligus
std::vector<ExecutableKillResult> KillProcessesByExecutableNames(
    const std::vector<std::wstring>& exeNames);

// Scan manual sekali: deteksi + kill proses mencurigakan
ManualScanResult ScanAndKillSuspicious();

// Temukan proses (selain Discord) yang membuka handle ke path tertentu
std::vector<ProcessInfo> FindProcessesAccessingPath(const std::wstring& targetPath);

} // namespace ProcessUtils