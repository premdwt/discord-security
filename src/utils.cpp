#include "utils.h"

#include <algorithm>
#include <shlwapi.h>
#include <sstream>

#pragma comment(lib, "shlwapi.lib")

namespace Utils {

std::wstring GetEnvPath(const wchar_t* varName) {
    wchar_t buffer[MAX_PATH];
    DWORD len = GetEnvironmentVariableW(varName, buffer, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        return L"";
    }
    return std::wstring(buffer);
}

std::vector<std::wstring> GetDiscordWatchPaths() {
    std::vector<std::wstring> paths;

    const std::wstring appData = GetEnvPath(L"APPDATA");
    const std::wstring localAppData = GetEnvPath(L"LOCALAPPDATA");

    if (!appData.empty()) {
        // Discord stable + PTB + Canary variants
        const wchar_t* variants[] = {
            L"discord\\Local Storage\\leveldb",
            L"discordptb\\Local Storage\\leveldb",
            L"discordcanary\\Local Storage\\leveldb",
        };
        for (const auto* variant : variants) {
            std::wstring path = appData + L"\\" + variant;
            if (PathFileExistsW(path.c_str())) {
                paths.push_back(path);
            }
        }
    }

    if (!localAppData.empty()) {
        const wchar_t* localVariants[] = {
            L"Discord",
            L"DiscordPTB",
            L"DiscordCanary",
        };
        for (const auto* variant : localVariants) {
            std::wstring path = localAppData + L"\\" + variant;
            if (PathFileExistsW(path.c_str())) {
                paths.push_back(path);
            }
        }
    }

    return paths;
}

std::string WideToUtf8(const std::wstring& wide) {
    if (wide.empty()) return {};

    int size = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(),
                                   static_cast<int>(wide.size()),
                                   nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};

    std::string result(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(),
                        static_cast<int>(wide.size()),
                        result.data(), size, nullptr, nullptr);
    return result;
}

std::wstring Utf8ToWide(const std::string& utf8) {
    if (utf8.empty()) return {};

    int size = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(),
                                   static_cast<int>(utf8.size()),
                                   nullptr, 0);
    if (size <= 0) return {};

    std::wstring result(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(),
                        static_cast<int>(utf8.size()),
                        result.data(), size);
    return result;
}

std::wstring ToLower(const std::wstring& str) {
    std::wstring result = str;
    std::transform(result.begin(), result.end(), result.begin(), ::towlower);
    return result;
}

bool ContainsIgnoreCase(const std::wstring& haystack, const std::wstring& needle) {
    if (needle.empty()) return true;
    return ToLower(haystack).find(ToLower(needle)) != std::wstring::npos;
}

std::wstring GetExecutableDirectory() {
    wchar_t buffer[MAX_PATH];
    DWORD len = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        return L".";
    }
    PathRemoveFileSpecW(buffer);
    return std::wstring(buffer);
}

bool EnableDebugPrivilege() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(),
                          TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
        return false;
    }

    TOKEN_PRIVILEGES tp{};
    if (!LookupPrivilegeValueW(nullptr, SE_DEBUG_NAME, &tp.Privileges[0].Luid)) {
        CloseHandle(token);
        return false;
    }

    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

    BOOL ok = AdjustTokenPrivileges(token, FALSE, &tp, sizeof(tp), nullptr, nullptr);
    CloseHandle(token);
    return ok != FALSE;
}

std::wstring GetProcessNameByPid(DWORD pid) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return L"";

    wchar_t name[MAX_PATH];
    DWORD size = MAX_PATH;
    std::wstring result;

    if (QueryFullProcessImageNameW(process, 0, name, &size)) {
        result = name;
        // Ambil hanya nama file
        const wchar_t* fileName = PathFindFileNameW(result.c_str());
        if (fileName) result = fileName;
    }

    CloseHandle(process);
    return result;
}

bool IsPathInsideDirectory(const std::wstring& path, const std::wstring& directory) {
    if (path.empty() || directory.empty()) return false;

    wchar_t normalizedPath[MAX_PATH];
    wchar_t normalizedDir[MAX_PATH];

    if (!PathCanonicalizeW(normalizedPath, path.c_str())) return false;
    if (!PathCanonicalizeW(normalizedDir, directory.c_str())) return false;

    // Pastikan direktori diakhiri backslash untuk perbandingan prefix
    std::wstring dirPrefix = normalizedDir;
    if (dirPrefix.back() != L'\\') {
        dirPrefix += L'\\';
    }

    std::wstring pathLower = ToLower(normalizedPath);
    std::wstring dirLower = ToLower(dirPrefix);

    return pathLower.rfind(dirLower, 0) == 0;
}

bool IsRunningAsAdministrator() {
    BOOL isAdmin = FALSE;
    PSID adminGroup = nullptr;

    SID_IDENTIFIER_AUTHORITY ntAuthority = SECURITY_NT_AUTHORITY;
    if (!AllocateAndInitializeSid(
            &ntAuthority,
            2,
            SECURITY_BUILTIN_DOMAIN_RID,
            DOMAIN_ALIAS_RID_ADMINS,
            0, 0, 0, 0, 0, 0,
            &adminGroup)) {
        return false;
    }

    CheckTokenMembership(nullptr, adminGroup, &isAdmin);
    FreeSid(adminGroup);
    return isAdmin != FALSE;
}

std::string FormatWin32Error(DWORD errorCode) {
    if (errorCode == 0) {
        return "No error";
    }

    wchar_t* messageBuffer = nullptr;
    const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER |
                        FORMAT_MESSAGE_FROM_SYSTEM |
                        FORMAT_MESSAGE_IGNORE_INSERTS;

    const DWORD length = FormatMessageW(
        flags,
        nullptr,
        errorCode,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPWSTR>(&messageBuffer),
        0,
        nullptr);

    if (length == 0 || !messageBuffer) {
        std::ostringstream oss;
        oss << "Unknown error (code " << errorCode << ")";
        return oss.str();
    }

    std::wstring wide(messageBuffer, length);
    LocalFree(messageBuffer);

    // Buang newline di akhir pesan sistem
    while (!wide.empty() && (wide.back() == L'\n' || wide.back() == L'\r')) {
        wide.pop_back();
    }

    return WideToUtf8(wide);
}

} // namespace Utils