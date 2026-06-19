// logger.cpp — Implementasi file log + mirror console (non-blocking untuk background).
#include "logger.h"
#include "app_version.h"
#include "console_color.h"
#include "utils.h"

#include <chrono>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace Logger {

namespace {

std::mutex g_mutex;
std::ofstream g_logFile;
std::string g_logPath;
bool g_initialized = false;
Config g_config{};

std::function<void(const std::string&)> g_consoleSink;
std::function<void(const std::string&)> g_backgroundConsoleSink;

const char* LevelToString(Level level) {
    switch (level) {
        case Level::Debug:   return "DEBUG";
        case Level::Info:    return "INFO";
        case Level::Warning: return "WARNING";
        case Level::Error:   return "ERROR";
        case Level::Alert:   return "ALERT";
    }
    return "UNKNOWN";
}

std::string CurrentTimestamp() {
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);

    std::tm localTime{};
    localtime_s(&localTime, &time);

    std::ostringstream oss;
    oss << std::put_time(&localTime, "%Y-%m-%d %H:%M:%S");
    return oss.str();
}

bool ShouldWriteToFile(Level level) {
    if (level == Level::Debug && !g_config.debugEnabled) {
        return false;
    }
    return true;
}

bool ShouldMirrorToConsole(Level level, LogFlags flags) {
    if (HasFlag(flags, LogFlags::ForceConsole)) {
        return true;
    }

    switch (level) {
        case Level::Warning:
        case Level::Error:
        case Level::Alert:
            return true;
        case Level::Info:
            return g_config.mirrorInfoToConsole;
        case Level::Debug:
        default:
            return false;
    }
}

void MirrorToConsoleUnlocked(Level level,
                             const std::string& line,
                             LogFlags flags) {
    if (!ShouldMirrorToConsole(level, flags)) {
        return;
    }

    const bool fromBackground = HasFlag(flags, LogFlags::FromBackground);

    if (fromBackground) {
        if (g_backgroundConsoleSink) {
            g_backgroundConsoleSink(line);
        }
        return;
    }

    if (g_consoleSink) {
        g_consoleSink(line);
        return;
    }

    ConsoleColor::WriteLine(line, false);
}

void WriteUnlocked(Level level,
                   const std::string& message,
                   LogFlags flags = LogFlags::None) {
    const std::string line = FormatLine(level, message);

    if (g_logFile.is_open() && ShouldWriteToFile(level)) {
        g_logFile << line << '\n';
        g_logFile.flush();
    }

    MirrorToConsoleUnlocked(level, line, flags);
}

} // namespace

std::string LevelTag(Level level) {
    return std::string("[") + LevelToString(level) + "]";
}

std::string FormatLine(Level level, const std::string& message) {
    return "[" + CurrentTimestamp() + "] " + LevelTag(level) + " " + message;
}

bool Initialize(const Config& config) {
    std::lock_guard<std::mutex> lock(g_mutex);

    if (g_initialized) {
        return true;
    }

    g_config = config;

    const std::wstring exeDir = Utils::GetExecutableDirectory();
    g_logPath = Utils::WideToUtf8(exeDir) + "\\discord_sentinel.log";

    g_logFile.open(g_logPath, std::ios::app);
    if (!g_logFile.is_open()) {
        return false;
    }

    g_initialized = true;
    WriteUnlocked(Level::Info, AppVersion::DisplayString() + " logger initialized.");
    return true;
}

void Shutdown() {
    std::lock_guard<std::mutex> lock(g_mutex);

    if (g_initialized) {
        WriteUnlocked(Level::Info, AppVersion::DisplayString() + " shutting down.");
        g_logFile.close();
        g_initialized = false;
    }
}

void SetDebugEnabled(bool enabled) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_config.debugEnabled = enabled;
}

bool IsDebugEnabled() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_config.debugEnabled;
}

void SetConsoleSink(std::function<void(const std::string& line)> sink) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_consoleSink = std::move(sink);
}

void SetBackgroundConsoleSink(std::function<void(const std::string& line)> sink) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_backgroundConsoleSink = std::move(sink);
}

void Log(Level level, const std::string& message, LogFlags flags) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_initialized && level != Level::Debug) {
        return;
    }
    WriteUnlocked(level, message, flags);
}

void Debug(const std::string& message) {
    Log(Level::Debug, message);
}

void Info(const std::string& message, LogFlags flags) {
    Log(Level::Info, message, flags);
}

void Warning(const std::string& message, LogFlags flags) {
    Log(Level::Warning, message, flags);
}

void Error(const std::string& message, LogFlags flags) {
    Log(Level::Error, message, flags);
}

void Alert(const std::string& message, LogFlags flags) {
    Log(Level::Alert, message, flags);
}

void LogWin32Error(const std::string& context, DWORD errorCode) {
    const DWORD code = (errorCode != 0) ? errorCode : GetLastError();
    Error(context + " — Win32 error " + std::to_string(code) +
          ": " + Utils::FormatWin32Error(code));
}

void LogProcessKilled(const std::string& processName,
                      DWORD pid,
                      const std::string& reason,
                      const std::string& source) {
    std::string message = "Proses mencurigakan dihentikan: " + processName +
                          " (PID: " + std::to_string(pid) + ")";
    if (!reason.empty()) {
        message += " - Reason: " + reason;
    }
    if (!source.empty()) {
        message = source + " — " + message;
    }
    Alert(message, LogFlags::FromBackground);
}

void LogManualScanKill(const std::string& processName,
                       DWORD pid,
                       const std::string& reason) {
    std::string message = "Manual Scan — Proses dihentikan: " + processName +
                          " (PID: " + std::to_string(pid) + ")";
    if (!reason.empty()) {
        message += " - Reason: " + reason;
    }
    Alert(message, Logger::LogFlags::ForceConsole);
}

void LogSkippedTrustedProcess(const std::string& processName,
                              DWORD pid,
                              const std::string& reason) {
    Debug("Skipped trusted process: " + processName +
          " (PID: " + std::to_string(pid) + ") — " + reason);
}

std::string GetLogFilePath() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_logPath;
}

} // namespace Logger