# Discord Sentinel

**Discord Sentinel v1.0** — aplikasi keamanan Windows native (C++ / Win32) untuk melindungi akun Discord dari **token stealer** dan proses mencurigakan.

Aplikasi berjalan di **system tray** dengan monitoring background yang tetap aktif meskipun console disembunyikan.

---

## Fitur Utama

| Fitur | Deskripsi |
|-------|-----------|
| **Background Monitoring** | Pemantauan folder Discord (`ReadDirectoryChangesW`) + scan proses berkala di thread terpisah |
| **Process Scanner** | Deteksi proses mencurigakan berdasarkan keyword, lokasi file, dan verifikasi tanda tangan digital |
| **Folder Watcher** | Alert saat ada perubahan file di direktori Discord (Roaming/Local AppData) |
| **Manual Scan** | Scan manual semua proses yang berjalan dan hentikan yang terdeteksi mencurigakan |
| **Token Clear** | Scan & bersihkan data token Discord dari Discord Desktop, browser Chromium, dan Firefox |
| **System Tray** | Minimize ke tray — monitoring tidak berhenti saat console di-hide |
| **Logging** | Log ke file + mirror ke console dengan level (Info, Warning, Error, Alert) |

---

## Persyaratan

- **OS:** Windows 10/11 (x64)
- **Compiler:** Visual Studio 2022/2026 dengan workload *Desktop development with C++*
- **Toolset:** MSVC v145 (atau kompatibel)
- **CMake:** 3.16+ (opsional, untuk build alternatif)
- **Hak akses:** Administrator (disarankan — diperlukan untuk `SeDebugPrivilege` dan kill proses)

---

## Cara Build

### Visual Studio (disarankan)

1. Clone repository:
   ```bash
   git clone https://github.com/premdwt/discord-security.git
   cd discord-security
   ```
2. Buka `DiscordSentinel.sln` di Visual Studio.
3. Pilih konfigurasi **Release | x64**.
4. Build → **Build Solution** (`Ctrl+Shift+B`).
5. Output executable: `bin/Release/DiscordSentinel.exe`

### CMake

```bash
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

Executable akan berada di `build/bin/DiscordSentinel.exe`.

---

## Cara Pakai

1. Jalankan `DiscordSentinel.exe` **sebagai Administrator**.
2. Aplikasi langsung masuk ke **system tray** — monitoring background otomatis aktif.
3. **Klik kiri** ikon tray untuk membuka console dashboard.
4. Tutup window console (X) = **hide ke tray**, bukan exit. Monitoring tetap berjalan.
5. **Exit penuh** hanya dari menu kanan tray → *Exit*.

### Menu Console

| # | Menu | Fungsi |
|---|------|--------|
| 1 | Mulai Monitoring | Aktifkan background monitoring |
| 2 | Hentikan Monitoring | Stop monitoring |
| 3 | Scan Proses Mencurigakan | Scan manual & kill proses suspicious |
| 4 | Info Log & Folder Discord | Tampilkan path log dan folder yang dipantau |
| 5 | Sembunyikan ke Tray | Hide console |
| 6 | Clear Discord Tokens | Scan & hapus data token Discord |

---

## Struktur Project

```
DiscordSentinel/
├── src/
│   ├── main.cpp           # Entry point (WinMain)
│   ├── monitoring.cpp/h   # Folder watcher + process scanner
│   ├── process_utils.cpp/h  # Enumerasi, signature check, kill proses
│   ├── token_clear.cpp/h  # Scan & clear token Discord
│   ├── dashboard.cpp/h    # Console menu interaktif
│   ├── tray.cpp/h         # System tray icon & menu
│   ├── console_window.cpp/h
│   ├── logger.cpp/h       # File + console logging
│   └── utils.cpp/h        # Helper Windows API
├── DiscordSentinel.sln
├── DiscordSentinel.vcxproj
├── CMakeLists.txt
└── app.manifest           # requireAdministrator
```

---

## Catatan Keamanan

- Aplikasi ini **bukan antivirus** — fokus pada proteksi token Discord dan deteksi pola proses mencurigakan.
- Fitur **Clear Discord Tokens** akan menghentikan proses terkait dan menghapus data autentikasi — Anda perlu **login ulang** ke Discord setelahnya.
- Whitelist proses Windows built-in dilindungi agar tidak dihentikan secara tidak sengaja.
- Disarankan menjalankan sebagai Administrator agar semua fitur berfungsi optimal.

---

## Teknologi

- C++17
- Win32 API (tanpa framework UI eksternal)
- MSVC / MSBuild
- CMake (build alternatif)

---

## Author

**premdwt** — [github.com/premdwt](https://github.com/premdwt)

---

## Lisensi

Project ini bersifat pribadi/edukatif. Hubungi author untuk pertanyaan penggunaan atau distribusi.