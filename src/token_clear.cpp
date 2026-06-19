// token_clear.cpp — Deteksi dan pembersihan token Discord di desktop/browser.
#include "token_clear.h"
#include "logger.h"
#include "utils.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <shlwapi.h>
#include <sstream>
#include <vector>
#include <windows.h>

#pragma comment(lib, "shlwapi.lib")

namespace fs = std::filesystem;

namespace TokenClear {

namespace {

constexpr const char* kDiscordOriginA = "_https://discord.com";
constexpr const char* kDiscordAppOriginA = "_https://discordapp.com";

// ---------------------------------------------------------------------------
// SQLite (winsqlite3.dll)
// ---------------------------------------------------------------------------

struct SqliteApi {
    HMODULE module = nullptr;

    using sqlite3 = struct sqlite3;
    using sqlite3_stmt = struct sqlite3_stmt;

    int (*open_v2)(const char*, sqlite3**, int, const char*) = nullptr;
    int (*close)(sqlite3*) = nullptr;
    int (*exec)(sqlite3*, const char*, int(*)(void*,int,char**,char**), void*, char**) = nullptr;
    int (*prepare_v2)(sqlite3*, const char*, int, sqlite3_stmt**, const char**) = nullptr;
    int (*step)(sqlite3_stmt*) = nullptr;
    int (*finalize)(sqlite3_stmt*) = nullptr;
    int (*changes)(sqlite3*) = nullptr;
    int (*column_int)(sqlite3_stmt*, int) = nullptr;

    bool Load() {
        if (module) return true;

        module = LoadLibraryW(L"winsqlite3.dll");
        if (!module) {
            module = LoadLibraryW(L"sqlite3.dll");
        }
        if (!module) {
            Logger::LogWin32Error("Token clear: SQLite library tidak ditemukan");
            return false;
        }

#define LOAD_FN(name) \
    name = reinterpret_cast<decltype(name)>(GetProcAddress(module, "sqlite3_" #name)); \
    if (!name) return false

        LOAD_FN(open_v2);
        LOAD_FN(close);
        LOAD_FN(exec);
        LOAD_FN(prepare_v2);
        LOAD_FN(step);
        LOAD_FN(finalize);
        LOAD_FN(changes);
        LOAD_FN(column_int);
#undef LOAD_FN

        return true;
    }
};

SqliteApi g_sqlite;

bool FileExists(const std::wstring& path) {
    return PathFileExistsW(path.c_str()) != FALSE;
}

bool DirectoryHasFiles(const std::wstring& dirPath) {
    if (!FileExists(dirPath)) {
        return false;
    }

    std::error_code ec;
    if (!fs::exists(dirPath, ec) || ec) {
        return false;
    }
    return !fs::is_empty(dirPath, ec);
}

bool ContainsDiscordOrigin(const std::vector<char>& data) {
    const auto contains = [&](const char* needle) {
        const size_t len = strlen(needle);
        if (len == 0 || data.size() < len) return false;
        return std::search(data.begin(), data.end(),
                           needle, needle + len) != data.end();
    };

    return contains(kDiscordOriginA) || contains(kDiscordAppOriginA);
}

bool LevelDbHasDiscordData(const std::wstring& levelDbPath) {
    if (!FileExists(levelDbPath)) {
        return false;
    }

    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(levelDbPath, ec)) {
        if (ec || !entry.is_regular_file()) {
            continue;
        }

        const std::wstring ext = entry.path().extension().wstring();
        if (ext != L".ldb" && ext != L".log" &&
            entry.path().filename() != "LOG") {
            continue;
        }

        std::ifstream in(entry.path(), std::ios::binary);
        if (!in.is_open()) {
            continue;
        }

        std::vector<char> data((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
        if (ContainsDiscordOrigin(data)) {
            return true;
        }
    }

    return false;
}

bool IndexedDbHasDiscordFolders(const std::wstring& profilePath) {
    const std::wstring indexedDbRoot = profilePath + L"\\IndexedDB";
    if (!FileExists(indexedDbRoot)) {
        return false;
    }

    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(indexedDbRoot, ec)) {
        if (ec || !entry.is_directory()) {
            continue;
        }

        const std::wstring folderName = Utils::ToLower(entry.path().filename().wstring());
        if (folderName.find(L"discord.com") != std::wstring::npos ||
            folderName.find(L"discordapp.com") != std::wstring::npos) {
            return true;
        }
    }

    return false;
}

bool FirefoxStorageHasDiscord(const std::wstring& profilePath) {
    const std::wstring storageRoot = profilePath + L"\\storage\\default";
    if (!FileExists(storageRoot)) {
        return false;
    }

    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(storageRoot, ec)) {
        if (ec || !entry.is_directory()) {
            continue;
        }

        const std::wstring folderName = Utils::ToLower(entry.path().filename().wstring());
        if (folderName.find(L"discord") != std::wstring::npos) {
            return true;
        }
    }

    return false;
}

int CountDiscordCookies(const std::wstring& cookiesPath,
                        const char* countSql,
                        std::string& errorOut) {
    if (!g_sqlite.Load()) {
        errorOut = "SQLite tidak tersedia.";
        return -1;
    }

    if (!FileExists(cookiesPath)) {
        return 0;
    }

    const std::string pathUtf8 = Utils::WideToUtf8(cookiesPath);
    SqliteApi::sqlite3* db = nullptr;

    constexpr int kSqliteOpenReadOnly = 0x00000001;
    if (g_sqlite.open_v2(pathUtf8.c_str(), &db, kSqliteOpenReadOnly, nullptr) != 0) {
        errorOut = "Database cookies tidak bisa dibuka (mungkin terkunci).";
        return -1;
    }

    SqliteApi::sqlite3_stmt* stmt = nullptr;
    if (g_sqlite.prepare_v2(db, countSql, -1, &stmt, nullptr) != 0) {
        errorOut = "Query COUNT cookies gagal disiapkan.";
        g_sqlite.close(db);
        return -1;
    }

    int count = 0;
    if (g_sqlite.step(stmt) == 100 /* SQLITE_ROW */) {
        count = g_sqlite.column_int(stmt, 0);
    }

    g_sqlite.finalize(stmt);
    g_sqlite.close(db);
    return count;
}

void RecordClearResult(std::vector<ClearTargetResult>& results,
                       const std::string& target,
                       bool success,
                       const std::string& message) {
    results.push_back({target, success, message});

    const std::string logLine = "Token clear [" + target + "]: " + message;
    if (success) {
        Logger::Info(logLine);
    } else {
        Logger::Error(logLine);
    }
}

bool OverwriteFileWithZeros(const std::wstring& path, std::string& errorOut) {
    SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);

    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
        errorOut = "Gagal overwrite file: " + Utils::WideToUtf8(path);
        Logger::Error("Token clear: " + errorOut);
        return false;
    }

    file.put('\0');
    file.close();
    return true;
}

bool ClearDirectoryContents(const std::wstring& dirPath,
                            int& deletedCount,
                            std::string& errorOut) {
    deletedCount = 0;

    if (!FileExists(dirPath)) {
        return true;
    }

    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dirPath, ec)) {
        if (ec) {
            errorOut = "Gagal membaca folder: " + Utils::WideToUtf8(dirPath) +
                       " — " + ec.message();
            Logger::Error("Token clear: " + errorOut);
            return false;
        }

        std::error_code removeEc;
        const auto removed = fs::remove_all(entry.path(), removeEc);
        if (removeEc) {
            errorOut = "Gagal hapus: " + Utils::WideToUtf8(entry.path().wstring()) +
                       " — " + removeEc.message();
            Logger::Error("Token clear: " + errorOut);
            return false;
        }

        deletedCount += static_cast<int>(removed);
    }

    return true;
}

bool ScrubLevelDbDiscordEntries(const std::wstring& levelDbPath,
                                int& scrubbedFiles,
                                std::string& errorOut) {
    scrubbedFiles = 0;

    if (!FileExists(levelDbPath)) {
        return true;
    }

    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(levelDbPath, ec)) {
        if (ec || !entry.is_regular_file()) {
            continue;
        }

        const std::wstring ext = entry.path().extension().wstring();
        if (ext != L".ldb" && ext != L".log" && entry.path().filename() != "LOG") {
            continue;
        }

        std::ifstream in(entry.path(), std::ios::binary);
        if (!in.is_open()) {
            continue;
        }

        std::vector<char> data((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
        in.close();

        if (!ContainsDiscordOrigin(data)) {
            continue;
        }

        auto scrubAround = [&](const char* origin) {
            const size_t originLen = strlen(origin);
            auto it = data.begin();
            while (it != data.end()) {
                it = std::search(it, data.end(), origin, origin + originLen);
                if (it == data.end()) break;

                const size_t pos = static_cast<size_t>(std::distance(data.begin(), it));
                const size_t start = (pos > 128) ? pos - 128 : 0;
                const size_t end = std::min(data.size(), pos + originLen + 512);
                std::fill(data.begin() + static_cast<ptrdiff_t>(start),
                          data.begin() + static_cast<ptrdiff_t>(end), '\0');
                it = data.begin() + static_cast<ptrdiff_t>(end);
            }
        };

        scrubAround(kDiscordOriginA);
        scrubAround(kDiscordAppOriginA);

        std::ofstream out(entry.path(), std::ios::binary | std::ios::trunc);
        if (!out.is_open()) {
            errorOut = "Gagal menulis LevelDB: " +
                       Utils::WideToUtf8(entry.path().wstring());
            Logger::Error("Token clear: " + errorOut);
            return false;
        }

        out.write(data.data(), static_cast<std::streamsize>(data.size()));
        out.close();
        ++scrubbedFiles;
    }

    return true;
}

bool DeleteDiscordIndexedDbFolders(const std::wstring& profilePath,
                                   int& deletedFolders) {
    deletedFolders = 0;
    const std::wstring indexedDbRoot = profilePath + L"\\IndexedDB";

    if (!FileExists(indexedDbRoot)) {
        return true;
    }

    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(indexedDbRoot, ec)) {
        if (ec || !entry.is_directory()) {
            continue;
        }

        const std::wstring folderName = Utils::ToLower(entry.path().filename().wstring());
        if (folderName.find(L"discord.com") != std::wstring::npos ||
            folderName.find(L"discordapp.com") != std::wstring::npos) {
            std::error_code removeEc;
            fs::remove_all(entry.path(), removeEc);
            if (!removeEc) {
                ++deletedFolders;
            }
        }
    }

    return true;
}

bool DeleteFirefoxDiscordStorage(const std::wstring& profilePath,
                                 int& deletedFolders) {
    deletedFolders = 0;
    const std::wstring storageRoot = profilePath + L"\\storage\\default";

    if (!FileExists(storageRoot)) {
        return true;
    }

    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(storageRoot, ec)) {
        if (ec || !entry.is_directory()) {
            continue;
        }

        const std::wstring folderName = Utils::ToLower(entry.path().filename().wstring());
        if (folderName.find(L"discord") != std::wstring::npos) {
            std::error_code removeEc;
            fs::remove_all(entry.path(), removeEc);
            if (!removeEc) {
                ++deletedFolders;
            }
        }
    }

    return true;
}

bool DeleteDiscordCookiesSqlite(const std::wstring& cookiesPath,
                                const char* deleteSql,
                                int& deletedRows,
                                std::string& errorOut) {
    deletedRows = 0;

    if (!g_sqlite.Load()) {
        errorOut = "SQLite tidak tersedia.";
        return false;
    }

    if (!FileExists(cookiesPath)) {
        return true;
    }

    const std::string pathUtf8 = Utils::WideToUtf8(cookiesPath);
    SqliteApi::sqlite3* db = nullptr;

    constexpr int kSqliteOpenReadWrite = 0x00000002;
    if (g_sqlite.open_v2(pathUtf8.c_str(), &db, kSqliteOpenReadWrite, nullptr) != 0) {
        errorOut = "Database cookies terkunci.";
        return false;
    }

    SqliteApi::sqlite3_stmt* stmt = nullptr;
    if (g_sqlite.prepare_v2(db, deleteSql, -1, &stmt, nullptr) != 0) {
        errorOut = "Query DELETE cookies gagal disiapkan.";
        g_sqlite.close(db);
        return false;
    }

    const int stepResult = g_sqlite.step(stmt);
    g_sqlite.finalize(stmt);

    if (stepResult != 101 /* SQLITE_DONE */) {
        errorOut = "Query DELETE cookies gagal dijalankan.";
        g_sqlite.close(db);
        return false;
    }

    deletedRows = g_sqlite.changes(db);
    g_sqlite.exec(db, "PRAGMA wal_checkpoint(TRUNCATE);", nullptr, nullptr, nullptr);
    g_sqlite.close(db);
    return true;
}

std::wstring GetChromiumCookiesPath(const std::wstring& profilePath) {
    const std::wstring networkCookies = profilePath + L"\\Network\\Cookies";
    if (FileExists(networkCookies)) {
        return networkCookies;
    }

    const std::wstring legacyCookies = profilePath + L"\\Cookies";
    if (FileExists(legacyCookies)) {
        return legacyCookies;
    }

    return L"";
}

std::wstring GetDiscordLevelDbPath(const std::wstring& basePath) {
    return basePath + L"\\Local Storage\\leveldb";
}

std::wstring GetDiscordLocalStatePath(const std::wstring& basePath) {
    return basePath + L"\\Local State";
}

void AppendDetail(std::ostringstream& oss, const std::string& part) {
    if (part.empty()) {
        return;
    }
    if (!oss.str().empty()) {
        oss << ", ";
    }
    oss << part;
}

void ScanDiscordDesktopPaths(const std::wstring& roamingFolder,
                             const std::wstring& localFolder,
                             const std::string& displayName,
                             const std::wstring& processExe,
                             TokenScanReport& report) {
    DetectedTokenTarget target;
    target.displayName = displayName;
    target.processExeName = processExe;

    const std::wstring appData = Utils::GetEnvPath(L"APPDATA");
    const std::wstring localAppData = Utils::GetEnvPath(L"LOCALAPPDATA");

    if (!appData.empty()) {
        target.roamingBasePath = appData + L"\\" + roamingFolder;
    }
    if (!localAppData.empty()) {
        target.localBasePath = localAppData + L"\\" + localFolder;
    }

    target.processRunning = ProcessUtils::IsProcessRunning(processExe);

    const std::vector<std::wstring> bases = {
        target.roamingBasePath,
        target.localBasePath,
    };

    for (const auto& base : bases) {
        if (base.empty()) {
            continue;
        }

        const std::wstring levelDb = GetDiscordLevelDbPath(base);
        const std::wstring localState = GetDiscordLocalStatePath(base);

        if (DirectoryHasFiles(levelDb)) {
            target.hasLevelDb = true;
        }
        if (FileExists(localState)) {
            target.hasLocalState = true;
        }
    }

    target.hasTokenData = target.hasLevelDb || target.hasLocalState;

    std::ostringstream detail;
    if (target.hasLevelDb) {
        AppendDetail(detail, "LevelDB ditemukan");
    }
    if (target.hasLocalState) {
        AppendDetail(detail, "Local State ditemukan");
    }
    if (target.processRunning) {
        AppendDetail(detail, "proses berjalan");
    }
    if (!target.hasTokenData) {
        target.detailSummary = "Tidak ada data token";
    } else {
        target.detailSummary = detail.str();
    }

    if (target.roamingBasePath.empty() && target.localBasePath.empty()) {
        return;
    }

    if (!target.hasTokenData) {
        return;
    }

    report.targets.push_back(std::move(target));
}

std::vector<std::wstring> GetChromiumUserDataRoots() {
    const std::wstring localAppData = Utils::GetEnvPath(L"LOCALAPPDATA");
    if (localAppData.empty()) {
        return {};
    }

    return {
        localAppData + L"\\Google\\Chrome\\User Data",
        localAppData + L"\\Microsoft\\Edge\\User Data",
        localAppData + L"\\BraveSoftware\\Brave-Browser\\User Data",
    };
}

std::vector<std::wstring> GetBrowserProfileDirs(const std::wstring& userDataRoot) {
    std::vector<std::wstring> profiles;

    if (!FileExists(userDataRoot)) {
        return profiles;
    }

    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(userDataRoot, ec)) {
        if (ec || !entry.is_directory()) {
            continue;
        }

        const std::wstring name = entry.path().filename().wstring();
        if (name == L"Default" || name.rfind(L"Profile ", 0) == 0) {
            profiles.push_back(entry.path().wstring());
        }
    }

    return profiles;
}

std::string ChromiumBrowserLabel(const std::wstring& userDataRoot) {
    const std::wstring lower = Utils::ToLower(userDataRoot);
    if (lower.find(L"\\google\\chrome\\") != std::wstring::npos) return "Chrome";
    if (lower.find(L"\\microsoft\\edge\\") != std::wstring::npos) return "Edge";
    if (lower.find(L"\\bravesoftware\\brave-browser\\") != std::wstring::npos) {
        return "Brave";
    }
    return "Browser";
}

std::wstring ChromiumProcessExe(const std::string& label) {
    if (label == "Chrome") return L"chrome.exe";
    if (label == "Edge") return L"msedge.exe";
    if (label == "Brave") return L"brave.exe";
    return L"";
}

void ScanChromiumProfile(const std::wstring& profilePath,
                         const std::string& browserLabel,
                         const std::wstring& processExe,
                         TokenScanReport& report) {
    DetectedTokenTarget target;
    target.displayName = browserLabel + " — " + Utils::WideToUtf8(profilePath);
    target.profilePath = profilePath;
    target.processExeName = processExe;

    const std::wstring cookiesPath = GetChromiumCookiesPath(profilePath);
    const std::wstring localStoragePath = profilePath + L"\\Local Storage\\leveldb";

    std::string cookieError;
    if (!cookiesPath.empty()) {
        const char* countSql =
            "SELECT COUNT(*) FROM cookies WHERE host_key LIKE '%discord.com%' "
            "OR host_key LIKE '%discordapp.com%';";
        const int count = CountDiscordCookies(cookiesPath, countSql, cookieError);
        if (count > 0) {
            target.discordCookieCount = count;
        }
    }

    if (FileExists(localStoragePath)) {
        target.hasDiscordLocalStorage = LevelDbHasDiscordData(localStoragePath);
    }

    target.hasDiscordIndexedDb = IndexedDbHasDiscordFolders(profilePath);

    target.hasTokenData =
        target.discordCookieCount > 0 ||
        target.hasDiscordLocalStorage ||
        target.hasDiscordIndexedDb;

    if (target.hasTokenData) {
        target.processRunning = ProcessUtils::IsProcessRunning(processExe);
    }

    std::ostringstream detail;
    if (target.discordCookieCount > 0) {
        AppendDetail(detail, std::to_string(target.discordCookieCount) + " cookies Discord");
    }
    if (target.hasDiscordLocalStorage) {
        AppendDetail(detail, "Local Storage Discord");
    }
    if (target.hasDiscordIndexedDb) {
        AppendDetail(detail, "IndexedDB Discord");
    }
    if (target.processRunning) {
        AppendDetail(detail, "proses berjalan");
    }

    if (!target.hasTokenData) {
        return;
    }

    target.detailSummary = detail.str();
    report.targets.push_back(std::move(target));
}

void ScanFirefoxProfiles(TokenScanReport& report) {
    const std::wstring appData = Utils::GetEnvPath(L"APPDATA");
    if (appData.empty()) {
        return;
    }

    const std::wstring profilesRoot = appData + L"\\Mozilla\\Firefox\\Profiles";
    if (!FileExists(profilesRoot)) {
        return;
    }

    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(profilesRoot, ec)) {
        if (ec || !entry.is_directory()) {
            continue;
        }

        const std::wstring profilePath = entry.path().wstring();
        DetectedTokenTarget target;
        target.displayName = "Firefox — " + Utils::WideToUtf8(profilePath);
        target.profilePath = profilePath;
        target.processExeName = L"firefox.exe";
        target.isFirefox = true;

        const std::wstring cookiesPath = profilePath + L"\\cookies.sqlite";
        std::string cookieError;
        if (FileExists(cookiesPath)) {
            const char* countSql =
                "SELECT COUNT(*) FROM moz_cookies WHERE host LIKE '%discord.com%' "
                "OR host LIKE '%discordapp.com%';";
            const int count = CountDiscordCookies(cookiesPath, countSql, cookieError);
            if (count > 0) {
                target.discordCookieCount = count;
            }
        }

        target.hasDiscordLocalStorage = FirefoxStorageHasDiscord(profilePath);

        target.hasTokenData =
            target.discordCookieCount > 0 || target.hasDiscordLocalStorage;

        if (target.hasTokenData) {
            target.processRunning = ProcessUtils::IsProcessRunning(L"firefox.exe");
        }

        std::ostringstream detail;
        if (target.discordCookieCount > 0) {
            AppendDetail(detail, std::to_string(target.discordCookieCount) + " cookies Discord");
        }
        if (target.hasDiscordLocalStorage) {
            AppendDetail(detail, "Storage Discord");
        }
        if (target.processRunning) {
            AppendDetail(detail, "proses berjalan");
        }

        if (!target.hasTokenData) {
            continue;
        }

        target.detailSummary = detail.str();
        report.targets.push_back(std::move(target));
    }
}

bool ClearDiscordDesktopTarget(const DetectedTokenTarget& target,
                               std::vector<ClearTargetResult>& results) {
    if (!target.hasTokenData) {
        return true;
    }

    int totalDeleted = 0;
    const std::vector<std::wstring> bases = {
        target.roamingBasePath,
        target.localBasePath,
    };

    for (const auto& base : bases) {
        if (base.empty()) {
            continue;
        }

        int deletedCount = 0;
        std::string errorOut;

        const std::wstring levelDb = GetDiscordLevelDbPath(base);
        const std::wstring localState = GetDiscordLocalStatePath(base);

        if (FileExists(levelDb)) {
            if (!ClearDirectoryContents(levelDb, deletedCount, errorOut)) {
                RecordClearResult(results, target.displayName, false, errorOut);
                return false;
            }
            totalDeleted += deletedCount;
        }

        if (FileExists(localState)) {
            if (!OverwriteFileWithZeros(localState, errorOut)) {
                RecordClearResult(results, target.displayName, false, errorOut);
                return false;
            }
        }
    }

    RecordClearResult(results, target.displayName, true,
                      "LevelDB dibersihkan (" + std::to_string(totalDeleted) +
                      " item), Local State di-overwrite.");
    return true;
}

bool ClearChromiumTarget(const DetectedTokenTarget& target,
                         std::vector<ClearTargetResult>& results) {
    if (!target.hasTokenData) {
        return true;
    }

    int cookieRows = 0;
    int indexedDbFolders = 0;
    int scrubbedFiles = 0;
    std::string errorOut;

    const std::wstring cookiesPath = GetChromiumCookiesPath(target.profilePath);
    const std::wstring localStoragePath =
        target.profilePath + L"\\Local Storage\\leveldb";

    if (!cookiesPath.empty() && target.discordCookieCount > 0) {
        const char* deleteSql =
            "DELETE FROM cookies WHERE host_key LIKE '%discord.com%' "
            "OR host_key LIKE '%discordapp.com%';";
        if (!DeleteDiscordCookiesSqlite(cookiesPath, deleteSql, cookieRows, errorOut)) {
            RecordClearResult(results, target.displayName, false, errorOut);
            return false;
        }
    }

    DeleteDiscordIndexedDbFolders(target.profilePath, indexedDbFolders);

    if (target.hasDiscordLocalStorage && FileExists(localStoragePath)) {
        if (!ScrubLevelDbDiscordEntries(localStoragePath, scrubbedFiles, errorOut)) {
            RecordClearResult(results, target.displayName, false, errorOut);
            return false;
        }
    }

    const std::string summary =
        "Cookies Discord: " + std::to_string(cookieRows) +
        " baris; IndexedDB: " + std::to_string(indexedDbFolders) +
        " folder; Local Storage: " + std::to_string(scrubbedFiles) + " file dibersihkan.";

    RecordClearResult(results, target.displayName, true, summary);
    return true;
}

bool ClearFirefoxTarget(const DetectedTokenTarget& target,
                        std::vector<ClearTargetResult>& results) {
    if (!target.hasTokenData) {
        return true;
    }

    int cookieRows = 0;
    int storageFolders = 0;
    std::string errorOut;

    const std::wstring cookiesPath = target.profilePath + L"\\cookies.sqlite";
    if (target.discordCookieCount > 0 && FileExists(cookiesPath)) {
        const char* deleteSql =
            "DELETE FROM moz_cookies WHERE host LIKE '%discord.com%' "
            "OR host LIKE '%discordapp.com%';";
        if (!DeleteDiscordCookiesSqlite(cookiesPath, deleteSql, cookieRows, errorOut)) {
            RecordClearResult(results, target.displayName, false, errorOut);
            return false;
        }
    }

    DeleteFirefoxDiscordStorage(target.profilePath, storageFolders);

    const std::string summary =
        "Cookies Discord: " + std::to_string(cookieRows) +
        " baris; Storage: " + std::to_string(storageFolders) + " folder dihapus.";

    RecordClearResult(results, target.displayName, true, summary);
    return true;
}

} // namespace

bool TokenScanReport::HasAnyTokenData() const {
    for (const auto& target : targets) {
        if (target.hasTokenData) {
            return true;
        }
    }
    return false;
}

std::vector<std::wstring> TokenScanReport::GetProcessNamesToKill() const {
    std::vector<std::wstring> names;

    for (const auto& target : targets) {
        if (!target.hasTokenData || target.processExeName.empty()) {
            continue;
        }

        bool exists = false;
        for (const auto& existing : names) {
            if (_wcsicmp(existing.c_str(), target.processExeName.c_str()) == 0) {
                exists = true;
                break;
            }
        }

        if (!exists) {
            names.push_back(target.processExeName);
        }
    }

    return names;
}

int TokenScanReport::CountRunningProcesses() const {
    int count = 0;
    const auto names = GetProcessNamesToKill();

    for (const auto& name : names) {
        if (ProcessUtils::IsProcessRunning(name)) {
            ++count;
        }
    }

    return count;
}

bool TokenClearExecutionReport::AnyClearSuccess() const {
    for (const auto& r : clearResults) {
        if (r.success) return true;
    }
    return false;
}

bool TokenClearExecutionReport::HasClearFailures() const {
    for (const auto& r : clearResults) {
        if (!r.success) return true;
    }
    return false;
}

// Fase 1: deteksi lokasi token tanpa mengubah file.
TokenScanReport ScanDiscordTokenLocations() {
    TokenScanReport report;

    try {
    Logger::Info("Token clear scan: memulai deteksi lokasi token Discord.");

    ScanDiscordDesktopPaths(L"discord", L"Discord",
                            "Discord Desktop (Stable)", L"Discord.exe", report);
    ScanDiscordDesktopPaths(L"discordptb", L"DiscordPTB",
                            "Discord Desktop (PTB)", L"DiscordPTB.exe", report);
    ScanDiscordDesktopPaths(L"discordcanary", L"DiscordCanary",
                            "Discord Desktop (Canary)", L"DiscordCanary.exe", report);

    for (const auto& userDataRoot : GetChromiumUserDataRoots()) {
        const std::string label = ChromiumBrowserLabel(userDataRoot);
        const std::wstring processExe = ChromiumProcessExe(label);
        const auto profiles = GetBrowserProfileDirs(userDataRoot);

        for (const auto& profile : profiles) {
            ScanChromiumProfile(profile, label, processExe, report);
        }
    }

    ScanFirefoxProfiles(report);

    const int detected = static_cast<int>(std::count_if(
        report.targets.begin(), report.targets.end(),
        [](const DetectedTokenTarget& t) { return t.hasTokenData; }));

    Logger::Info("Token clear scan selesai. Target dengan data token: " +
                 std::to_string(detected) + ".");

    } catch (const std::exception& ex) {
        Logger::Error(std::string("Token clear scan exception: ") + ex.what());
    } catch (...) {
        Logger::Error("Token clear scan exception tidak dikenal.");
    }

    return report;
}

// Fase 2: kill proses terkait, lalu bersihkan token yang terdeteksi.
TokenClearExecutionReport ExecuteTokenClear(const TokenScanReport& scan) {
    TokenClearExecutionReport report;

    try {
    Logger::Info("Token clear execute: memulai kill proses terkait.");

    const std::vector<std::wstring> processNames = scan.GetProcessNamesToKill();
    report.killResults = ProcessUtils::KillProcessesByExecutableNames(processNames);

    int totalKilled = 0;
    int totalKillFailed = 0;
    for (const auto& kr : report.killResults) {
        totalKilled += kr.killed;
        totalKillFailed += kr.failed;

        if (kr.failed > 0) {
            Logger::Warning(
                "Token clear kill " + Utils::WideToUtf8(kr.exeName) +
                ": berhasil=" + std::to_string(kr.killed) +
                ", gagal=" + std::to_string(kr.failed));
        } else if (kr.killed > 0) {
            Logger::Info(
                "Token clear kill " + Utils::WideToUtf8(kr.exeName) +
                ": " + std::to_string(kr.killed) + " proses dihentikan.");
        }
    }

    if (totalKilled > 0 || totalKillFailed > 0) {
        Logger::Info("Token clear: menunggu " +
                     std::to_string(kPostKillDelayMs) +
                     "ms agar file tidak terkunci...");
        Sleep(kPostKillDelayMs);
    }

    Logger::Info("Token clear execute: memulai pembersihan file token.");

    for (const auto& target : scan.targets) {
        if (!target.hasTokenData) {
            continue;
        }

        if (target.isFirefox) {
            ClearFirefoxTarget(target, report.clearResults);
        } else if (!target.profilePath.empty()) {
            ClearChromiumTarget(target, report.clearResults);
        } else {
            ClearDiscordDesktopTarget(target, report.clearResults);
        }
    }

    const int successCount = static_cast<int>(std::count_if(
        report.clearResults.begin(), report.clearResults.end(),
        [](const ClearTargetResult& r) { return r.success; }));

    Logger::Info("Token clear execute selesai. Pembersihan berhasil: " +
                 std::to_string(successCount) + "/" +
                 std::to_string(report.clearResults.size()) + ".");

    } catch (const std::exception& ex) {
        Logger::Error(std::string("Token clear execute exception: ") + ex.what());
    } catch (...) {
        Logger::Error("Token clear execute exception tidak dikenal.");
    }

    return report;
}

} // namespace TokenClear