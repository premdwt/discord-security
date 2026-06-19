#include "console_color.h"

#include <iostream>

namespace ConsoleColor {

namespace {

HANDLE GetStdoutHandle() {
    return GetStdHandle(STD_OUTPUT_HANDLE);
}

bool ContainsTag(const std::string& line, const char* tag) {
    return line.find(tag) != std::string::npos;
}

} // namespace

void SetColor(WORD attr) {
    const HANDLE handle = GetStdoutHandle();
    if (handle != INVALID_HANDLE_VALUE && handle != nullptr) {
        SetConsoleTextAttribute(handle, attr);
    }
}

void Reset() {
    SetColor(static_cast<WORD>(Attr::Default));
}

WORD ResolveColorForLine(const std::string& line) {
    if (ContainsTag(line, "] [ERROR]") || ContainsTag(line, "[ERROR]") ||
        ContainsTag(line, "[GAGAL]")) {
        return static_cast<WORD>(Attr::Error);
    }

    if (ContainsTag(line, "] [ALERT]") || ContainsTag(line, "[ALERT]") ||
        ContainsTag(line, "dihentikan") || ContainsTag(line, "mencurigakan dihentikan")) {
        return static_cast<WORD>(Attr::Alert);
    }

    if (ContainsTag(line, "] [WARNING]") || ContainsTag(line, "[WARNING]") ||
        ContainsTag(line, "[PERINGATAN]")) {
        return static_cast<WORD>(Attr::Warning);
    }

    if (ContainsTag(line, "[OK]") || ContainsTag(line, "=== Hasil")) {
        return static_cast<WORD>(Attr::Success);
    }

    if (ContainsTag(line, "] [INFO]") || ContainsTag(line, "[INFO]")) {
        return static_cast<WORD>(Attr::Info);
    }

    if (ContainsTag(line, "[TIP]") || ContainsTag(line, "[AKTIF]") ||
        ContainsTag(line, "Monitoring berjalan")) {
        return static_cast<WORD>(Attr::Status);
    }

    if (ContainsTag(line, "=== Menu ===")) {
        return static_cast<WORD>(Attr::Default);
    }

    return static_cast<WORD>(Attr::Default);
}

void WriteLine(const std::string& line, bool leadingNewline) {
    const WORD color = ResolveColorForLine(line);
    SetColor(color);

    if (leadingNewline) {
        std::cout << '\n';
    }
    std::cout << line << '\n';
    std::cout.flush();

    Reset();
}

void WritePrompt(const std::string& text) {
    SetColor(static_cast<WORD>(Attr::Prompt));
    std::cout << text;
    std::cout.flush();
    Reset();
}

} // namespace ConsoleColor