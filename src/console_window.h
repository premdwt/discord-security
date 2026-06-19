#pragma once

#include <functional>
#include <windows.h>

namespace AppConsole {

// Pesan kustom — dikirim ke message window tray saat user klik X di console.
constexpr UINT WM_APP_CONSOLE_HIDE = WM_APP + 100;

using CloseCallback = std::function<void()>;

// Alokasikan console, hubungkan stdin/stdout/stderr, lalu sembunyikan ke tray.
bool InitializeHidden();

// Tampilkan console yang sudah dialokasikan (bukan membuat CMD baru).
void Show();

// Sembunyikan console — proses tetap berjalan di background.
void Hide();

// Lepas console saat aplikasi benar-benar keluar.
void Shutdown();

bool IsReady();
HWND GetWindowHandle();

// HWND yang menerima WM_APP_CONSOLE_HIDE (biasanya tray message window).
void SetNotifyWindow(HWND hwnd);

// Fallback callback jika notify window belum di-set.
void SetOnCloseRequested(CloseCallback callback);

// Pasang handler tombol X console (CTRL_CLOSE_EVENT + WM_CLOSE subclass).
void InstallCloseProtection();

} // namespace AppConsole