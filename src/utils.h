#pragma once

#include <string>
#include <vector>
#include <windows.h>

namespace Utils {

// Ambil path environment variable (AppData, LocalAppData, dll.)
std::wstring GetEnvPath(const wchar_t* varName);

// Path folder Discord yang perlu dipantau
std::vector<std::wstring> GetDiscordWatchPaths();

// Konversi wide <-> narrow string
std::string WideToUtf8(const std::wstring& wide);
std::wstring Utf8ToWide(const std::string& utf8);

// String helper: lowercase + substring check (case-insensitive)
std::wstring ToLower(const std::wstring& str);
bool ContainsIgnoreCase(const std::wstring& haystack, const std::wstring& needle);

// Direktori tempat executable berada (untuk menyimpan log)
std::wstring GetExecutableDirectory();

// Aktifkan SeDebugPrivilege agar bisa inspect handle proses lain
bool EnableDebugPrivilege();

// Dapatkan nama proses dari PID
std::wstring GetProcessNameByPid(DWORD pid);

// Cek apakah path berada di dalam direktori parent (untuk validasi handle)
bool IsPathInsideDirectory(const std::wstring& path, const std::wstring& directory);

// Cek apakah proses berjalan dengan hak Administrator
bool IsRunningAsAdministrator();

// Konversi kode error Windows ke pesan yang bisa dibaca
std::string FormatWin32Error(DWORD errorCode);

} // namespace Utils