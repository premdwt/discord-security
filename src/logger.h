#pragma once

// logger.h — Logging ke file + mirror opsional ke console (thread-safe).
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <windows.h>

namespace Logger {

enum class Level {
    Debug,
    Info,
    Warning,
    Error,
    Alert
};

enum class LogFlags : std::uint8_t {
    None           = 0,
    ForceConsole   = 1 << 0,
    FromBackground = 1 << 1,
};

inline LogFlags operator|(LogFlags a, LogFlags b) {
    return static_cast<LogFlags>(
        static_cast<std::uint8_t>(a) | static_cast<std::uint8_t>(b));
}

inline bool HasFlag(LogFlags flags, LogFlags flag) {
    return (static_cast<std::uint8_t>(flags) &
            static_cast<std::uint8_t>(flag)) != 0;
}

struct Config {
    bool debugEnabled = false;
    bool mirrorInfoToConsole = false;
};

bool Initialize(const Config& config = {});
void Shutdown();

void SetDebugEnabled(bool enabled);
bool IsDebugEnabled();

void SetConsoleSink(std::function<void(const std::string& line)> sink);
void SetBackgroundConsoleSink(std::function<void(const std::string& line)> sink);

std::string FormatLine(Level level, const std::string& message);
std::string LevelTag(Level level);

void Log(Level level, const std::string& message, LogFlags flags = LogFlags::None);

void Debug(const std::string& message);
void Info(const std::string& message, LogFlags flags = LogFlags::None);
void Warning(const std::string& message, LogFlags flags = LogFlags::None);
void Error(const std::string& message, LogFlags flags = LogFlags::None);
void Alert(const std::string& message, LogFlags flags = LogFlags::None);

void LogWin32Error(const std::string& context, DWORD errorCode = 0);

void LogProcessKilled(const std::string& processName,
                      DWORD pid,
                      const std::string& reason,
                      const std::string& source = "Monitoring");

void LogManualScanKill(const std::string& processName,
                       DWORD pid,
                       const std::string& reason = "");

void LogSkippedTrustedProcess(const std::string& processName,
                              DWORD pid,
                              const std::string& reason);

std::string GetLogFilePath();

} // namespace Logger