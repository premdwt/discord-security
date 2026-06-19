#pragma once

#include <string>
#include <windows.h>

// Helper warna console — satu tempat untuk aturan pewarnaan output.
namespace ConsoleColor {

// Atribut warna standar (foreground only, background hitam default)
enum class Attr : WORD {
    Default     = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE,
    Info        = FOREGROUND_BLUE | FOREGROUND_GREEN | FOREGROUND_INTENSITY,
    Success     = FOREGROUND_GREEN | FOREGROUND_INTENSITY,
    Warning     = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY,
    Error       = FOREGROUND_RED | FOREGROUND_INTENSITY,
    Alert       = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY,
    Status      = FOREGROUND_INTENSITY,
    Prompt      = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY,
};

void SetColor(WORD attr);
void Reset();

// Tulis satu baris dengan warna otomatis berdasarkan isi/tag log.
void WriteLine(const std::string& line, bool leadingNewline = false);

// Tulis prompt interaktif (warna terang default).
void WritePrompt(const std::string& text);

// Tentukan warna dari isi baris (tag [INFO], [ERROR], [OK], dll).
WORD ResolveColorForLine(const std::string& line);

} // namespace ConsoleColor