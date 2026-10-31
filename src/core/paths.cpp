#include "core/paths.h"

#include <windows.h>
#include <shlobj.h>

namespace pons {

std::filesystem::path ExecutablePath() {
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (length < path.size()) {
            path.resize(length);
            return path;
        }
        path.resize(path.size() * 2);
    }
}

std::filesystem::path DownloadsDirectory() {
    PWSTR folder = nullptr;
    std::filesystem::path path;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Downloads, 0, nullptr, &folder))) path = folder;
    CoTaskMemFree(folder);
    return path;
}

}  // namespace pons
