#pragma once

#include <string>

// Informasi versi aplikasi — satu sumber untuk banner, log, dan UI.
namespace AppVersion {

inline constexpr const char* kApplicationName = "Discord Sentinel";
inline constexpr const char* kVersionNumber  = "1.0";
inline constexpr const char* kVersionLabel     = "Discord Sentinel v1.0";
inline constexpr wchar_t kConsoleTitle[]       = L"Discord Sentinel v1.0 — Console";

inline std::string DisplayString() {
    return std::string(kApplicationName) + " v" + kVersionNumber;
}

} // namespace AppVersion