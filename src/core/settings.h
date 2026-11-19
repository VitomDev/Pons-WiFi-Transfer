#pragma once

#include <windows.h>

#include <filesystem>

namespace pons {

// Registry location. The installer and the tests rely on this name.
inline constexpr wchar_t kSettingsKey[] = L"Software\\Pons WiFi Transfer";

inline constexpr int kDefaultPort = 8642;
inline constexpr int kDefaultTimeoutMinutes = 15;
inline constexpr int kDefaultBackgroundMinutes = 60;
inline constexpr int kMaxBackgroundMinutes = 24 * 60;

// What closing the window does while devices can still connect.
enum class CloseAction { Ask = 0, Quit = 1, KeepRunning = 2 };

struct Settings {
    int port = kDefaultPort;
    // 0 keeps the server running until Pons is closed.
    int timeoutMinutes = kDefaultTimeoutMinutes;
    CloseAction closeAction = CloseAction::Ask;
    // How long Pons keeps sharing in the background after its window is closed.
    int backgroundMinutes = kDefaultBackgroundMinutes;
    // Empty means the Downloads folder.
    std::filesystem::path receiveFolder;

    // `keyPath` is relative to HKEY_CURRENT_USER; tests use a scratch key.
    static Settings Load(const wchar_t* keyPath = kSettingsKey);
    bool Save(const wchar_t* keyPath = kSettingsKey) const;
};

bool IsValidPort(int port);

}  // namespace pons
