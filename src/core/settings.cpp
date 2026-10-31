#include "core/settings.h"

#include <string>

namespace pons {

namespace {

bool ReadDword(HKEY key, const wchar_t* name, DWORD& value) {
    DWORD size = sizeof(value);
    return RegGetValueW(key, nullptr, name, RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS;
}

bool WriteDword(HKEY key, const wchar_t* name, DWORD value) {
    return RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value)) == ERROR_SUCCESS;
}

}  // namespace

bool IsValidPort(int port) { return port >= 1024 && port <= 65535; }

Settings Settings::Load(const wchar_t* keyPath) {
    Settings settings;
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, keyPath, 0, KEY_READ, &key) != ERROR_SUCCESS) return settings;

    DWORD value = 0;
    if (ReadDword(key, L"Port", value) && IsValidPort(static_cast<int>(value))) settings.port = static_cast<int>(value);
    if (ReadDword(key, L"TimeoutMinutes", value) && value <= 24 * 60) settings.timeoutMinutes = static_cast<int>(value);

    wchar_t folder[MAX_PATH * 2]{};
    DWORD size = sizeof(folder);
    if (RegGetValueW(key, nullptr, L"ReceiveFolder", RRF_RT_REG_SZ, nullptr, folder, &size) == ERROR_SUCCESS) {
        settings.receiveFolder = folder;
    }
    RegCloseKey(key);
    return settings;
}

bool Settings::Save(const wchar_t* keyPath) const {
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, keyPath, 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
        return false;
    }
    const std::wstring folder = receiveFolder.wstring();
    const bool saved = WriteDword(key, L"Port", static_cast<DWORD>(port)) &&
                       WriteDword(key, L"TimeoutMinutes", static_cast<DWORD>(timeoutMinutes)) &&
                       RegSetValueExW(key, L"ReceiveFolder", 0, REG_SZ, reinterpret_cast<const BYTE*>(folder.c_str()),
                                      static_cast<DWORD>((folder.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
    RegCloseKey(key);
    return saved;
}

}  // namespace pons
