#pragma once

#include <filesystem>

namespace pons {

std::filesystem::path ExecutablePath();

// The user's Downloads folder, where received files go by default.
std::filesystem::path DownloadsDirectory();

}  // namespace pons
