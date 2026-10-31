#pragma once

#include <windows.h>

#include <filesystem>

namespace pons {

// Registry location. The installer and the tests rely on this name.
inline constexpr wchar_t kSettingsKey[] = L"Software\\Pons WiFi Transfer";

inline constexpr int kDefaultPort = 8642;
inline constexpr int kDefaultTimeoutMinutes = 15;

struct Settings {
    int port = kDefaultPort;
    // 0 keeps the server running until Pons is closed.
    int timeoutMinutes = kDefaultTimeoutMinutes;
    // Empty means the Downloads folder.
    std::filesystem::path receiveFolder;

    // `keyPath` is relative to HKEY_CURRENT_USER; tests use a scratch key.
    static Settings Load(const wchar_t* keyPath = kSettingsKey);
    bool Save(const wchar_t* keyPath = kSettingsKey) const;
};

bool IsValidPort(int port);

}  // namespace pons
