#pragma once

#include <filesystem>

namespace pons {

std::filesystem::path ExecutablePath();

// The user's Downloads folder, where files from the phone go by default.
std::filesystem::path DownloadsDirectory();

}  // namespace pons
