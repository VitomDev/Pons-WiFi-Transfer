#pragma once

#include <string>

namespace pons {

// The page the phone opens from the QR code: one self-contained HTML file
// (styles and script inline), nothing loaded from the internet.
const std::string& PageHtml();

// Shown for a missing or old token.
const std::string& ExpiredPageHtml();

}  // namespace pons
