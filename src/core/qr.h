#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace pons {

// The modules of an encoded QR code, without its quiet zone.
struct QrMatrix {
    int size = 0;
    std::vector<uint8_t> dark;  // size * size, 1 for dark modules

    bool empty() const { return size <= 0; }
    bool IsDark(int x, int y) const { return dark[static_cast<size_t>(y) * size + x] != 0; }
};

// Error correction M (15%), the usual choice. Returns nothing when the text
// is empty or too long for any QR version.
std::optional<QrMatrix> EncodeQrCode(const std::wstring& text);

inline constexpr int kQuietZoneModules = 4;

}  // namespace pons
