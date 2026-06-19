// process_utils.cpp — Enumerasi, evaluasi, dan terminasi proses mencurigakan.
#include "process_utils.h"
#include "logger.h"
#include "monitoring.h"
#include "utils.h"

#include <tlhelp32.h>
#include <vector>
#include <wincrypt.h>
#include <wintrust.h>
#include <softpub.h>

#pragma comment(lib, "wintrust.lib")
#pragma comment(lib, "crypt32.lib")

namespace ProcessUtils {

namespace {

// NT API untuk enumerasi handle sistem (user-mode, tanpa driver)
using NTSTATUS = LONG;

#ifndef NT_SUCCESS
#define NT_SUCCESS(Status) (((NTSTATUS)(Status)) >= 0)
#endif

constexpr ULONG SystemExtendedHandleInformation = 64;

typedef struct _SYSTEM_HANDLE_TABLE_ENTRY_INFO_EX {
    PVOID Object;
    ULONG_PTR UniqueProcessId;
    ULONG_PTR HandleValue;
    ULONG GrantedAccess;
    USHORT CreatorBackTraceIndex;
    USHORT ObjectTypeIndex;
    ULONG HandleAttributes;
    ULONG Reserved;
} SYSTEM_HANDLE_TABLE_ENTRY_INFO_EX, *PSYSTEM_HANDLE_TABLE_ENTRY_INFO_EX;

typedef struct _SYSTEM_HANDLE_INFORMATION_EX {
    ULONG_PTR NumberOfHandles;
    ULONG_PTR Reserved;
    SYSTEM_HANDLE_TABLE_ENTRY_INFO_EX Handles[1];
} SYSTEM_HANDLE_INFORMATION_EX, *PSYSTEM_HANDLE_INFORMATION_EX;

typedef NTSTATUS(NTAPI* NtQuerySystemInformationFn)(
    ULONG SystemInformationClass,
    PVOID SystemInformation,
    ULONG SystemInformationLength,
    PULONG ReturnLength);

typedef NTSTATUS(NTAPI* NtQueryObjectFn)(
    HANDLE Handle,
    ULONG ObjectInformationClass,
    PVOID ObjectInformation,
    ULONG ObjectInformationLength,
    PULONG ReturnLength);

constexpr ULONG ObjectNameInformation = 1;

typedef struct _UNICODE_STRING {
    USHORT Length;
    USHORT MaximumLength;
    PWSTR Buffer;
} UNICODE_STRING, *PUNICODE_STRING;

typedef struct _OBJECT_NAME_INFORMATION {
    UNICODE_STRING Name;
} OBJECT_NAME_INFORMATION, *POBJECT_NAME_INFORMATION;

NtQuerySystemInformationFn GetNtQuerySystemInformation() {
    static auto fn = reinterpret_cast<NtQuerySystemInformationFn>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQuerySystemInformation"));
    return fn;
}

NtQueryObjectFn GetNtQueryObject() {
    static auto fn = reinterpret_cast<NtQueryObjectFn>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryObject"));
    return fn;
}

std::wstring QueryObjectName(HANDLE handle) {
    auto ntQueryObject = GetNtQueryObject();
    if (!ntQueryObject) return L"";

    ULONG bufferSize = 0x1000;
    std::vector<BYTE> buffer(bufferSize);

    NTSTATUS status = ntQueryObject(handle, ObjectNameInformation,
                                    buffer.data(), bufferSize, &bufferSize);

    if (status == 0xC0000004) {
        buffer.resize(bufferSize);
        status = ntQueryObject(handle, ObjectNameInformation,
                               buffer.data(), bufferSize, &bufferSize);
    }

    if (!NT_SUCCESS(status)) return L"";

    auto* nameInfo = reinterpret_cast<POBJECT_NAME_INFORMATION>(buffer.data());
    if (!nameInfo->Name.Buffer || nameInfo->Name.Length == 0) return L"";

    return std::wstring(nameInfo->Name.Buffer,
                        nameInfo->Name.Length / sizeof(wchar_t));
}

std::wstring GetProcessImagePath(DWORD pid) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return L"";

    wchar_t path[MAX_PATH];
    DWORD size = MAX_PATH;
    std::wstring result;

    if (QueryFullProcessImageNameW(process, 0, path, &size)) {
        result = path;
    }

    CloseHandle(process);
    return result;
}

// WinVerifyTrust di fungsi terpisah (hanya POD) agar bisa dibungkus SEH
LONG VerifyTrustSafe(const wchar_t* filePath) {
    if (!filePath || !*filePath) {
        return static_cast<LONG>(0xFFFFFFFF);
    }

    __try {
        WINTRUST_FILE_INFO fileInfo{};
        fileInfo.cbStruct = sizeof(fileInfo);
        fileInfo.pcwszFilePath = filePath;

        GUID policyGuid = WINTRUST_ACTION_GENERIC_VERIFY_V2;

        WINTRUST_DATA trustData{};
        trustData.cbStruct = sizeof(trustData);
        trustData.dwUIChoice = WTD_UI_NONE;
        trustData.fdwRevocationChecks = WTD_REVOKE_NONE;
        trustData.dwUnionChoice = WTD_CHOICE_FILE;
        trustData.pFile = &fileInfo;
        trustData.dwStateAction = WTD_STATEACTION_VERIFY;
        trustData.dwProvFlags = WTD_SAFER_FLAG;

        const LONG result = WinVerifyTrust(nullptr, &policyGuid, &trustData);

        trustData.dwStateAction = WTD_STATEACTION_CLOSE;
        WinVerifyTrust(nullptr, &policyGuid, &trustData);

        return result;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return static_cast<LONG>(0xFFFFFFFF);
    }
}

bool MatchesSuspiciousKeyword(const std::wstring& processName) {
    for (const auto& keyword : GetSuspiciousKeywords()) {
        if (Utils::ContainsIgnoreCase(processName, keyword)) {
            return true;
        }
    }
    return false;
}

bool IsSuspiciousLocation(const std::wstring& path) {
    if (path.empty()) {
        return false;
    }

    const std::wstring lower = Utils::ToLower(path);
    return lower.find(L"\\appdata\\roaming\\") != std::wstring::npos ||
           lower.find(L"\\appdata\\local\\temp\\") != std::wstring::npos ||
           lower.find(L"\\temp\\") != std::wstring::npos ||
           lower.find(L"\\downloads\\") != std::wstring::npos ||
           lower.find(L"\\programdata\\") != std::wstring::npos;
}

bool IsSentinelExecutableName(const std::wstring& processName) {
    const std::wstring lower = Utils::ToLower(processName);
    return lower == L"discordsentinel.exe";
}

bool NeedsSignatureCheck(const ProcessInfo& info) {
    if (MatchesSuspiciousKeyword(info.name)) {
        return false;
    }

    if (Utils::ContainsIgnoreCase(info.name, L"discord")) {
        return true;
    }

    if (!info.fullPath.empty() &&
        IsSuspiciousLocation(info.fullPath) &&
        !IsTrustedProcessLocation(info.fullPath)) {
        return true;
    }

    return false;
}

// Bangun teks alasan skip untuk logging manual scan
std::string BuildTrustedSkipReason(const std::wstring& skipKind,
                                   const DigitalSignatureResult* signature) {
    if (signature && signature->isValid) {
        const std::string publisher = Utils::WideToUtf8(signature->publisher);
        if (signature->isTrustedPublisher && !publisher.empty()) {
            return Utils::WideToUtf8(skipKind) + " (" + publisher + " signed)";
        }
        if (!publisher.empty()) {
            return Utils::WideToUtf8(skipKind) + " (signed: " + publisher + ")";
        }
        return Utils::WideToUtf8(skipKind) + " (digitally signed)";
    }
    return Utils::WideToUtf8(skipKind);
}

bool IsWindowsHostProcess(const std::wstring& processName) {
    const std::wstring lower = Utils::ToLower(processName);
    return lower.size() > 8 &&
           lower.compare(lower.size() - 8, 8, L"host.exe") == 0;
}

// Tentukan apakah proses legitimate harus dilewati (bukan mencurigakan)
bool ShouldSkipAsTrustedProcess(ProcessInfo& info,
                                bool manualScan,
                                std::wstring* skipKindOut) {
    if (info.fullPath.empty()) {
        info.fullPath = GetProcessImagePath(info.pid);
    }

    if (IsWhitelistedWindowsProcess(info.name)) {
        if (skipKindOut) {
            *skipKindOut = L"Windows process whitelist";
        }

        if (manualScan) {
            const DigitalSignatureResult signature =
                info.fullPath.empty()
                    ? DigitalSignatureResult{}
                    : VerifyDigitalSignature(info.fullPath);
            info.isSigned = signature.isValid;

            const DigitalSignatureResult* sigPtr =
                signature.isValid ? &signature : nullptr;
            Logger::LogSkippedTrustedProcess(
                Utils::WideToUtf8(info.name),
                info.pid,
                BuildTrustedSkipReason(L"Windows process whitelist", sigPtr));
        }
        return true;
    }

    if (!info.fullPath.empty() && IsTrustedProcessLocation(info.fullPath)) {
        const DigitalSignatureResult signature = VerifyDigitalSignature(info.fullPath);
        info.isSigned = signature.isValid;

        if (skipKindOut) {
            *skipKindOut = L"trusted location";
        }

        // Log hanya proses Windows shell/UWP yang relevan — hindari spam log
        if (manualScan &&
            (signature.isTrustedPublisher || IsWindowsHostProcess(info.name))) {
            Logger::LogSkippedTrustedProcess(
                Utils::WideToUtf8(info.name),
                info.pid,
                BuildTrustedSkipReason(L"trusted location", &signature));
        }
        return true;
    }

    if (!info.fullPath.empty()) {
        const DigitalSignatureResult signature = VerifyDigitalSignature(info.fullPath);
        info.isSigned = signature.isValid;

        if (signature.isValid) {
            if (skipKindOut) {
                *skipKindOut = signature.isTrustedPublisher
                    ? L"trusted publisher"
                    : L"valid digital signature";
            }

            if (manualScan && signature.isTrustedPublisher) {
                Logger::LogSkippedTrustedProcess(
                    Utils::WideToUtf8(info.name),
                    info.pid,
                    BuildTrustedSkipReason(L"trusted publisher", &signature));
            }
            return true;
        }
    }

    return false;
}

} // namespace

const std::vector<std::wstring>& GetWindowsProcessWhitelist() {
    static const std::vector<std::wstring> whitelist = {
        L"textinputhost.exe",
        L"searchhost.exe",
        L"startmenuexperiencehost.exe",
        L"shellexperiencehost.exe",
        L"applicationframehost.exe",
        L"backgroundtaskhost.exe",
        L"runtimebroker.exe",
        L"lockapp.exe",
        L"dwm.exe",
        L"explorer.exe",
        L"sihost.exe",
        L"taskhostw.exe",
    };
    return whitelist;
}

bool IsWhitelistedWindowsProcess(const std::wstring& processName) {
    const std::wstring lower = Utils::ToLower(processName);
    for (const auto& allowed : GetWindowsProcessWhitelist()) {
        if (lower == allowed) {
            return true;
        }
    }
    return false;
}

bool IsTrustedProcessLocation(const std::wstring& fullPath) {
    if (fullPath.empty()) {
        return false;
    }

    const std::wstring lower = Utils::ToLower(fullPath);

    // Folder sistem & instalasi resmi — seluruh C:\Windows\ termasuk SystemApps
    if (lower.find(L"\\windows\\") != std::wstring::npos ||
        lower.find(L"\\program files\\") != std::wstring::npos ||
        lower.find(L"\\program files (x86)\\") != std::wstring::npos) {
        return true;
    }

    // Instalasi per-user umum (Discord, VS Code, dll.)
    if (lower.find(L"\\appdata\\local\\programs\\") != std::wstring::npos) {
        return true;
    }

    // Folder instalasi Discord Sentinel sendiri
    const std::wstring sentinelDir = Utils::ToLower(Utils::GetExecutableDirectory());
    if (!sentinelDir.empty() && lower.rfind(sentinelDir, 0) == 0) {
        return true;
    }

    return false;
}

bool IsTrustedPublisher(const std::wstring& publisher) {
    if (publisher.empty()) {
        return false;
    }

    const std::wstring lower = Utils::ToLower(publisher);
    static const std::vector<std::wstring> trustedPublishers = {
        L"microsoft corporation",
        L"microsoft windows",
        L"microsoft windows publisher",
        L"microsoft code signing pca",
        L"microsoft windows hardware compatibility publisher",
    };

    for (const auto& trusted : trustedPublishers) {
        if (lower.find(trusted) != std::wstring::npos) {
            return true;
        }
    }
    return false;
}

// Ekstrak publisher — hanya POD di dalam __try (C2712-safe)
BOOL QueryPublisherSafeImpl(const wchar_t* filePath,
                            wchar_t* publisherOut,
                            DWORD publisherCap) {
    if (!filePath || !*filePath || !publisherOut || publisherCap == 0) {
        return FALSE;
    }

    publisherOut[0] = L'\0';

    __try {
        HCERTSTORE certStore = nullptr;
        HCRYPTMSG cryptMsg = nullptr;

        if (!CryptQueryObject(
                CERT_QUERY_OBJECT_FILE,
                filePath,
                CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED_EMBED,
                CERT_QUERY_FORMAT_FLAG_BINARY,
                0,
                nullptr,
                nullptr,
                nullptr,
                &certStore,
                &cryptMsg,
                nullptr)) {
            return FALSE;
        }

        DWORD signerInfoSize = 0;
        if (!CryptMsgGetParam(cryptMsg, CMSG_SIGNER_INFO_PARAM, 0,
                              nullptr, &signerInfoSize) ||
            signerInfoSize == 0) {
            CryptMsgClose(cryptMsg);
            CertCloseStore(certStore, 0);
            return FALSE;
        }

        BYTE* signerInfoBuffer = static_cast<BYTE*>(
            HeapAlloc(GetProcessHeap(), 0, signerInfoSize));
        if (!signerInfoBuffer) {
            CryptMsgClose(cryptMsg);
            CertCloseStore(certStore, 0);
            return FALSE;
        }

        BOOL success = FALSE;
        if (CryptMsgGetParam(cryptMsg, CMSG_SIGNER_INFO_PARAM, 0,
                              signerInfoBuffer, &signerInfoSize)) {
            const auto* signerInfo =
                reinterpret_cast<PCMSG_SIGNER_INFO>(signerInfoBuffer);

            CERT_INFO certInfo{};
            certInfo.Issuer = signerInfo->Issuer;
            certInfo.SerialNumber = signerInfo->SerialNumber;

            const PCCERT_CONTEXT signerCert = CertFindCertificateInStore(
                certStore,
                X509_ASN_ENCODING | PKCS_7_ASN_ENCODING,
                0,
                CERT_FIND_SUBJECT_CERT,
                &certInfo,
                nullptr);

            if (signerCert) {
                if (CertGetNameStringW(
                        signerCert,
                        CERT_NAME_SIMPLE_DISPLAY_TYPE,
                        0,
                        nullptr,
                        publisherOut,
                        publisherCap) > 1) {
                    success = TRUE;
                }
                CertFreeCertificateContext(signerCert);
            }
        }

        HeapFree(GetProcessHeap(), 0, signerInfoBuffer);
        CryptMsgClose(cryptMsg);
        CertCloseStore(certStore, 0);
        return success;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return FALSE;
    }
}

bool QueryPublisherSafe(const wchar_t* filePath, std::wstring& publisherOut) {
    publisherOut.clear();
    if (!filePath || !*filePath) {
        return false;
    }

    wchar_t buffer[512] = {};
    if (!QueryPublisherSafeImpl(filePath, buffer, 512)) {
        return false;
    }

    publisherOut = buffer;
    return !publisherOut.empty();
}

DigitalSignatureResult VerifyDigitalSignature(const std::wstring& filePath) {
    DigitalSignatureResult result;

    if (filePath.empty()) {
        return result;
    }

    if (GetFileAttributesW(filePath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        return result;
    }

    const LONG trustResult = VerifyTrustSafe(filePath.c_str());
    if (trustResult == static_cast<LONG>(0xFFFFFFFF)) {
        Logger::Warning("WinVerifyTrust gagal/exception: " + Utils::WideToUtf8(filePath));
        return result;
    }

    result.isValid = (trustResult == ERROR_SUCCESS);
    if (result.isValid) {
        QueryPublisherSafe(filePath.c_str(), result.publisher);
        result.isTrustedPublisher = IsTrustedPublisher(result.publisher);
    }

    return result;
}

const std::vector<std::wstring>& GetSuspiciousKeywords() {
    static const std::vector<std::wstring> keywords = {
        // Token stealer / grabber umum
        L"grabber", L"injector", L"stealer", L"token",
        L"discordstealer", L"discord-stealer", L"tokenstealer",
        L"infostealer", L"info-stealer", L"accountstealer",
        L"discordgrab", L"tokengrab", L"webhook",
        // Malware / RAT / trojan
        L"malware", L"trojan", L"backdoor", L"rat",
        L"keylog", L"keylogger", L"payload", L"stub",
        // Nama generik mencurigakan (hindari substring terlalu pendek seperti "grab")
        L"logger", L"miner", L"cryptominer",
        L"discordinject", L"inject", L"exploit",
    };
    return keywords;
}

bool IsSentinelProcess(DWORD pid, const std::wstring& processName) {
    if (pid == GetCurrentProcessId()) {
        return true;
    }

    if (!processName.empty() && IsSentinelExecutableName(processName)) {
        return true;
    }

    return false;
}

bool IsDiscordProcess(const std::wstring& processName,
                      const std::wstring& fullPath) {
    const std::wstring lower = Utils::ToLower(processName);

    if (lower == L"discord.exe" ||
        lower == L"discordptb.exe" ||
        lower == L"discordcanary.exe") {
        return true;
    }

    if (lower == L"update.exe" && !fullPath.empty()) {
        const std::wstring pathLower = Utils::ToLower(fullPath);
        return pathLower.find(L"\\discord\\") != std::wstring::npos ||
               pathLower.find(L"\\discordptb\\") != std::wstring::npos ||
               pathLower.find(L"\\discordcanary\\") != std::wstring::npos;
    }

    return false;
}

bool IsDigitallySigned(const std::wstring& filePath) {
    return VerifyDigitalSignature(filePath).isValid;
}

// Menilai apakah proses mencurigakan (keyword, lokasi, signature).
bool EvaluateSuspicion(ProcessInfo& info, bool checkSignature, bool manualScan) {
    info.isSuspicious = false;
    info.suspicionReason.clear();

    if (info.pid <= 4) {
        return false;
    }

    // Jangan pernah flag/kill proses Discord Sentinel sendiri
    if (IsSentinelProcess(info.pid, info.name)) {
        return false;
    }

    if (IsDiscordProcess(info.name, info.fullPath)) {
        return false;
    }

    // --- Kriteria 1: nama proses mengandung kata kunci mencurigakan ---
    for (const auto& keyword : GetSuspiciousKeywords()) {
        if (Utils::ContainsIgnoreCase(info.name, keyword)) {
            info.isSuspicious = true;
            info.suspicionReason = L"Suspicious name keyword: " + keyword;
            return true;
        }
    }

    // --- Lewati proses Windows legitimate (whitelist / trusted path / signed) ---
    std::wstring skipKind;
    if (ShouldSkipAsTrustedProcess(info, manualScan, &skipKind)) {
        return false;
    }

    if (!checkSignature) {
        return false;
    }

    if (!manualScan && !NeedsSignatureCheck(info)) {
        return false;
    }

    const DigitalSignatureResult signature = VerifyDigitalSignature(info.fullPath);
    info.isSigned = signature.isValid;

    // Proses dengan signature valid (termasuk Microsoft) dianggap aman
    if (signature.isValid) {
        return false;
    }

    // --- Kriteria 2: unsigned yang meniru Discord ---
    if (Utils::ContainsIgnoreCase(info.name, L"discord")) {
        info.isSuspicious = true;
        info.suspicionReason = L"Unsigned process mimicking Discord";
        return true;
    }

    // --- Kriteria 3: unsigned di lokasi mencurigakan (Temp, Roaming, Downloads) ---
    if (IsSuspiciousLocation(info.fullPath)) {
        info.isSuspicious = true;
        info.suspicionReason = L"Unsigned executable in suspicious location";
        return true;
    }

    return false;
}

std::vector<ProcessInfo> EnumerateProcesses() {
    std::vector<ProcessInfo> processes;

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        Logger::LogWin32Error("EnumerateProcesses: CreateToolhelp32Snapshot gagal");
        return processes;
    }

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);

    if (Process32FirstW(snapshot, &entry)) {
        do {
            ProcessInfo info;
            info.pid = entry.th32ProcessID;
            info.name = entry.szExeFile;

            if (EvaluateSuspicion(info, false)) {
                processes.push_back(std::move(info));
                continue;
            }

            if (NeedsSignatureCheck(info)) {
                info.fullPath = GetProcessImagePath(info.pid);
                EvaluateSuspicion(info, true);
            }

            processes.push_back(std::move(info));
        } while (Process32NextW(snapshot, &entry));
    }

    CloseHandle(snapshot);
    return processes;
}

bool IsProcessRunning(const std::wstring& exeName) {
    return !FindProcessIdsByExecutableName(exeName).empty();
}

std::vector<DWORD> FindProcessIdsByExecutableName(const std::wstring& exeName) {
    std::vector<DWORD> pids;

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return pids;
    }

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);

    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, exeName.c_str()) == 0) {
                pids.push_back(entry.th32ProcessID);
            }
        } while (Process32NextW(snapshot, &entry));
    }

    CloseHandle(snapshot);
    return pids;
}

ExecutableKillResult KillProcessesByExecutableName(const std::wstring& exeName) {
    ExecutableKillResult result;
    result.exeName = exeName;

    Utils::EnableDebugPrivilege();

    const std::vector<DWORD> pids = FindProcessIdsByExecutableName(exeName);
    const DWORD selfPid = GetCurrentProcessId();

    for (const DWORD pid : pids) {
        if (pid == selfPid || pid <= 4) {
            continue;
        }

        std::wstring error;
        if (KillProcess(pid, error)) {
            ++result.killed;
            Logger::Info("Proses dihentikan: " + Utils::WideToUtf8(exeName) +
                         " (PID: " + std::to_string(pid) + ")");
        } else {
            ++result.failed;
            Logger::Warning("Gagal hentikan " + Utils::WideToUtf8(exeName) +
                            " (PID: " + std::to_string(pid) + ") — " +
                            Utils::WideToUtf8(error));
        }
    }

    return result;
}

std::vector<ExecutableKillResult> KillProcessesByExecutableNames(
    const std::vector<std::wstring>& exeNames) {

    std::vector<ExecutableKillResult> results;

    for (const auto& exeName : exeNames) {
        if (exeName.empty()) {
            continue;
        }

        bool alreadyQueued = false;
        for (const auto& existing : results) {
            if (_wcsicmp(existing.exeName.c_str(), exeName.c_str()) == 0) {
                alreadyQueued = true;
                break;
            }
        }
        if (alreadyQueued) {
            continue;
        }

        results.push_back(KillProcessesByExecutableName(exeName));
    }

    return results;
}

// Terminate proses by PID — tidak akan kill Discord Sentinel sendiri.
bool KillProcess(DWORD pid, std::wstring& errorOut) {
    if (IsSentinelProcess(pid)) {
        errorOut = L"Refusing to kill Discord Sentinel process.";
        return false;
    }

    HANDLE process = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
    if (!process) {
        errorOut = L"OpenProcess gagal (PID " + std::to_wstring(pid) +
                   L"), error: " + std::to_wstring(GetLastError());
        return false;
    }

    BOOL result = TerminateProcess(process, 1);
    DWORD err = GetLastError();
    CloseHandle(process);

    if (!result) {
        errorOut = L"TerminateProcess gagal (PID " + std::to_wstring(pid) +
                   L"), error: " + std::to_wstring(err);
        return false;
    }

    return true;
}

// Scan manual sekali (Menu 3): deteksi + kill proses mencurigakan.
ManualScanResult ScanAndKillSuspicious() {
    ManualScanResult result;

    Logger::Info("Manual scan dimulai.");

    Utils::EnableDebugPrivilege();

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        Logger::LogWin32Error("Manual Scan: gagal membuat snapshot proses");
        return result;
    }

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);

    if (!Process32FirstW(snapshot, &entry)) {
        Logger::LogWin32Error("Manual Scan: gagal enumerasi proses");
        CloseHandle(snapshot);
        return result;
    }

    do {
        ProcessInfo info;
        info.pid = entry.th32ProcessID;
        info.name = entry.szExeFile;

        // Lewati proses kernel/sistem ring 0 dan Discord Sentinel sendiri
        if (info.pid <= 4 || IsSentinelProcess(info.pid, info.name)) {
            continue;
        }

        ++result.scanned;

        if (IsDiscordProcess(info.name)) {
            info.fullPath = GetProcessImagePath(info.pid);
            if (IsDiscordProcess(info.name, info.fullPath)) {
                continue;
            }
        }

        // Scan manual: evaluasi dua tahap (keyword cepat, lalu signature + lokasi)
        bool suspicious = EvaluateSuspicion(info, false, true);
        if (!suspicious) {
            suspicious = EvaluateSuspicion(info, true, true);
        }

        if (!suspicious) {
            continue;
        }

        ++result.found;

        SuspiciousProcessEntry entryInfo;
        entryInfo.name = info.name;
        entryInfo.pid = info.pid;
        entryInfo.reason = info.suspicionReason;

        const std::string nameUtf8 = Utils::WideToUtf8(info.name);
        const std::string reasonUtf8 = Utils::WideToUtf8(info.suspicionReason);

        Monitoring::ConsoleOutput::PrintLine(
            "[ALERT] Proses mencurigakan ditemukan: " + nameUtf8 +
            " (PID: " + std::to_string(info.pid) + ")" +
            (reasonUtf8.empty() ? "" : " — " + reasonUtf8));

        Logger::Alert(
            "Manual Scan — Proses mencurigakan ditemukan: " + nameUtf8 +
            " (PID: " + std::to_string(info.pid) + ")" +
            (reasonUtf8.empty() ? "" : " - Reason: " + reasonUtf8));

        std::wstring error;
        if (KillProcess(info.pid, error)) {
            ++result.killed;
            entryInfo.killed = true;
            Logger::LogManualScanKill(nameUtf8, info.pid, reasonUtf8);
        } else {
            ++result.failed;
            Logger::Error(
                "Manual Scan — Gagal kill " + nameUtf8 +
                " (PID: " + std::to_string(info.pid) + ") — " +
                Utils::WideToUtf8(error));
        }

        result.entries.push_back(std::move(entryInfo));
    } while (Process32NextW(snapshot, &entry));

    CloseHandle(snapshot);

    Logger::Info("Manual scan selesai — discan: " + std::to_string(result.scanned) +
                 ", ditemukan: " + std::to_string(result.found) +
                 ", dihentikan: " + std::to_string(result.killed) +
                 ", gagal: " + std::to_string(result.failed) + ".");

    return result;
}

std::vector<ProcessInfo> FindProcessesAccessingPath(const std::wstring& targetPath) {
    std::vector<ProcessInfo> result;

    auto ntQuerySystemInfo = GetNtQuerySystemInformation();
    if (!ntQuerySystemInfo) {
        Logger::Warning("NtQuerySystemInformation tidak tersedia.");
        return result;
    }

    ULONG bufferSize = 0x10000;
    std::vector<BYTE> buffer(bufferSize);
    ULONG returnLength = 0;

    NTSTATUS status = ntQuerySystemInfo(
        SystemExtendedHandleInformation,
        buffer.data(), bufferSize, &returnLength);

    while (status == 0xC0000004) {
        bufferSize = returnLength + 0x10000;
        buffer.resize(bufferSize);
        status = ntQuerySystemInfo(
            SystemExtendedHandleInformation,
            buffer.data(), bufferSize, &returnLength);
    }

    if (!NT_SUCCESS(status)) {
        Logger::Warning("NtQuerySystemInformation gagal.");
        return result;
    }

    auto* handleInfo = reinterpret_cast<PSYSTEM_HANDLE_INFORMATION_EX>(buffer.data());
    const DWORD currentPid = GetCurrentProcessId();

    for (ULONG_PTR i = 0; i < handleInfo->NumberOfHandles; ++i) {
        const auto& entry = handleInfo->Handles[i];
        const DWORD ownerPid = static_cast<DWORD>(entry.UniqueProcessId);

        if (ownerPid == 0 || ownerPid == currentPid) continue;

        HANDLE srcProcess = OpenProcess(PROCESS_DUP_HANDLE, FALSE, ownerPid);
        if (!srcProcess) continue;

        HANDLE dupHandle = nullptr;
        if (!DuplicateHandle(srcProcess,
                             reinterpret_cast<HANDLE>(entry.HandleValue),
                             GetCurrentProcess(), &dupHandle,
                             0, FALSE, DUPLICATE_SAME_ACCESS)) {
            CloseHandle(srcProcess);
            continue;
        }

        std::wstring objectName = QueryObjectName(dupHandle);
        CloseHandle(dupHandle);
        CloseHandle(srcProcess);

        if (objectName.empty()) continue;

        std::wstring normalizedName = objectName;
        if (normalizedName.rfind(L"\\Device\\", 0) == 0) {
            wchar_t drives[512];
            if (GetLogicalDriveStringsW(512, drives)) {
                wchar_t* drive = drives;
                while (*drive) {
                    wchar_t deviceName[512];
                    if (QueryDosDeviceW(drive, deviceName, 512)) {
                        std::wstring deviceStr = deviceName;
                        if (normalizedName.rfind(L"\\" + deviceStr, 0) == 0) {
                            normalizedName = std::wstring(drive) +
                                normalizedName.substr(deviceStr.length() + 1);
                            break;
                        }
                    }
                    drive += wcslen(drive) + 1;
                }
            }
        }

        if (!Utils::IsPathInsideDirectory(normalizedName, targetPath)) continue;

        const std::wstring procName = Utils::GetProcessNameByPid(ownerPid);
        const std::wstring procPath = GetProcessImagePath(ownerPid);
        if (IsDiscordProcess(procName, procPath)) continue;

        bool found = false;
        for (const auto& existing : result) {
            if (existing.pid == ownerPid) { found = true; break; }
        }
        if (found) continue;

        ProcessInfo info;
        info.pid = ownerPid;
        info.name = procName;
        info.fullPath = procPath;
        info.isSuspicious = true;
        info.suspicionReason = L"Unauthorized access to Discord folder";
        result.push_back(std::move(info));
    }

    return result;
}

} // namespace ProcessUtils