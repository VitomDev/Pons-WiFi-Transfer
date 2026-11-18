#pragma once

#include <string>

namespace pons {

// The page devices open from the QR code or link: one self-contained HTML file
// (styles and script inline), nothing loaded from the internet.
const std::string& PageHtml();

// Shown for a missing or old token.
const std::string& ExpiredPageHtml();

}  // namespace pons
