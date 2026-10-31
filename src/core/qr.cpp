#include "core/qr.h"

#include <BitMatrix.h>
#include <qrcode/QRErrorCorrectionLevel.h>
#include <qrcode/QRWriter.h>

#include <algorithm>

namespace pons {

std::optional<QrMatrix> EncodeQrCode(const std::wstring& text) {
    if (text.empty()) return std::nullopt;
    try {
        ZXing::QRCode::Writer writer;
        writer.setMargin(0).setErrorCorrectionLevel(ZXing::QRCode::ErrorCorrectionLevel::Medium);
        const bool latin1 = std::all_of(text.begin(), text.end(), [](wchar_t c) { return c < 0x100; });
        if (!latin1) writer.setEncoding(ZXing::CharacterSet::UTF8);
        // Size 0 asks for one pixel per module.
        const ZXing::BitMatrix bits = writer.encode(text, 0, 0);
        QrMatrix matrix;
        matrix.size = bits.width();
        matrix.dark.resize(static_cast<size_t>(matrix.size) * matrix.size);
        for (int y = 0; y < matrix.size; ++y) {
            for (int x = 0; x < matrix.size; ++x) matrix.dark[static_cast<size_t>(y) * matrix.size + x] = bits.get(x, y) ? 1 : 0;
        }
        return matrix;
    } catch (const std::exception&) {
        // Too much data for the largest QR version.
        return std::nullopt;
    }
}

}  // namespace pons
