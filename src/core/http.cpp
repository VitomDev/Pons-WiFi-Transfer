#include "core/http.h"

#include "core/text.h"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdio>
#include <cwctype>

namespace pons {

namespace {

bool EqualsNoCase(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
           });
}

std::string_view TrimSpaces(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);
    return text;
}

int HexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

}  // namespace

std::optional<RequestHead> ParseRequestHead(std::string_view head) {
    RequestHead request;
    size_t lineEnd = head.find("\r\n");
    const std::string_view requestLine = head.substr(0, lineEnd);

    const size_t firstSpace = requestLine.find(' ');
    if (firstSpace == std::string_view::npos) return std::nullopt;
    const size_t secondSpace = requestLine.find(' ', firstSpace + 1);
    if (secondSpace == std::string_view::npos) return std::nullopt;
    request.method = requestLine.substr(0, firstSpace);
    const std::string_view target = requestLine.substr(firstSpace + 1, secondSpace - firstSpace - 1);
    const std::string_view version = requestLine.substr(secondSpace + 1);
    if (target.empty() || target.front() != '/' || !version.starts_with("HTTP/1.")) return std::nullopt;
    request.keepAlive = version != "HTTP/1.0";

    const size_t question = target.find('?');
    request.path = UrlDecode(target.substr(0, question), false);
    if (question != std::string_view::npos) request.query = target.substr(question + 1);

    while (lineEnd != std::string_view::npos) {
        const size_t start = lineEnd + 2;
        lineEnd = head.find("\r\n", start);
        const std::string_view line = head.substr(start, lineEnd == std::string_view::npos ? std::string_view::npos : lineEnd - start);
        const size_t colon = line.find(':');
        if (colon == std::string_view::npos) continue;
        const std::string_view name = TrimSpaces(line.substr(0, colon));
        const std::string_view value = TrimSpaces(line.substr(colon + 1));
        if (EqualsNoCase(name, "Content-Length")) {
            uint64_t length = 0;
            const auto result = std::from_chars(value.data(), value.data() + value.size(), length);
            if (result.ec != std::errc() || result.ptr != value.data() + value.size()) return std::nullopt;
            request.contentLength = length;
            request.hasContentLength = true;
        } else if (EqualsNoCase(name, "Transfer-Encoding")) {
            // The page never sends chunked bodies; refuse rather than misread one.
            return std::nullopt;
        } else if (EqualsNoCase(name, "Connection")) {
            if (EqualsNoCase(value, "close")) request.keepAlive = false;
            if (EqualsNoCase(value, "keep-alive")) request.keepAlive = true;
        }
    }
    return request;
}

std::string UrlDecode(std::string_view text, bool plusIsSpace) {
    std::string result;
    result.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '%' && i + 2 < text.size()) {
            const int high = HexValue(text[i + 1]);
            const int low = HexValue(text[i + 2]);
            if (high >= 0 && low >= 0) {
                result += static_cast<char>(high * 16 + low);
                i += 2;
                continue;
            }
        }
        result += plusIsSpace && c == '+' ? ' ' : c;
    }
    return result;
}

std::string UrlEncode(std::string_view text) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string result;
    for (const char c : text) {
        const auto byte = static_cast<unsigned char>(c);
        if (std::isalnum(byte) || c == '-' || c == '.' || c == '_' || c == '~') {
            result += c;
        } else {
            result += '%';
            result += kHex[byte >> 4];
            result += kHex[byte & 15];
        }
    }
    return result;
}

std::optional<std::string> QueryParam(std::string_view query, std::string_view name) {
    while (!query.empty()) {
        const size_t amp = query.find('&');
        const std::string_view pair = query.substr(0, amp);
        const size_t equals = pair.find('=');
        if (UrlDecode(pair.substr(0, equals)) == name) {
            return equals == std::string_view::npos ? std::string() : UrlDecode(pair.substr(equals + 1));
        }
        if (amp == std::string_view::npos) break;
        query.remove_prefix(amp + 1);
    }
    return std::nullopt;
}

std::string JsonEscape(std::string_view text) {
    std::string result;
    result.reserve(text.size() + 8);
    for (const char c : text) {
        switch (c) {
            case '"': result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            case '<': result += "\\u003c"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char escaped[8];
                    snprintf(escaped, sizeof(escaped), "\\u%04x", static_cast<unsigned>(c));
                    result += escaped;
                } else {
                    result += c;
                }
        }
    }
    return result;
}

std::wstring SanitizeFileName(std::wstring_view name) {
    // Phones may send a path; only the last part is the name.
    const size_t slash = name.find_last_of(L"/\\");
    if (slash != std::wstring_view::npos) name.remove_prefix(slash + 1);

    std::wstring result;
    for (const wchar_t c : name) result += c < 0x20 || wcschr(L"<>:\"|?*", c) ? L'_' : c;
    while (!result.empty() && (result.back() == L'.' || result.back() == L' ')) result.pop_back();
    while (!result.empty() && result.front() == L' ') result.erase(0, 1);
    if (result.size() > 200) {
        // Keep the extension when shortening.
        const size_t dot = result.find_last_of(L'.');
        const std::wstring extension = dot != std::wstring::npos && result.size() - dot <= 16 ? result.substr(dot) : L"";
        result = result.substr(0, 200 - extension.size()) + extension;
    }
    if (result.empty() || result == L".") result = L"file";

    // CON, PRN, AUX, NUL, COM1-9 and LPT1-9 cannot be file names, with any extension.
    std::wstring stem = result.substr(0, result.find(L'.'));
    std::transform(stem.begin(), stem.end(), stem.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towupper(c)); });
    const bool reserved = stem == L"CON" || stem == L"PRN" || stem == L"AUX" || stem == L"NUL" ||
                          (stem.size() == 4 && (stem.starts_with(L"COM") || stem.starts_with(L"LPT")) && iswdigit(stem[3]));
    if (reserved) result = L"_" + result;
    return result;
}

std::filesystem::path UniquePath(const std::filesystem::path& folder, const std::wstring& name) {
    std::filesystem::path candidate = folder / name;
    std::error_code error;
    if (!std::filesystem::exists(candidate, error)) return candidate;
    const std::filesystem::path original(name);
    const std::wstring stem = original.stem().wstring();
    const std::wstring extension = original.extension().wstring();
    for (int i = 2;; ++i) {
        candidate = folder / (stem + L" (" + std::to_wstring(i) + L")" + extension);
        if (!std::filesystem::exists(candidate, error)) return candidate;
    }
}

std::string AttachmentHeader(std::wstring_view fileName) {
    // An ASCII fallback for old browsers, then the exact UTF-8 name (RFC 6266).
    std::string fallback;
    for (const wchar_t c : fileName) fallback += c >= 0x20 && c < 0x7F && c != L'"' && c != L'\\' ? static_cast<char>(c) : '_';
    return "attachment; filename=\"" + fallback + "\"; filename*=UTF-8''" + UrlEncode(ToUtf8(fileName));
}

std::string GenerateToken() {
    uint8_t bytes[16];
    BCryptGenRandom(nullptr, bytes, sizeof(bytes), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    static constexpr char kHex[] = "0123456789abcdef";
    std::string token;
    for (const uint8_t byte : bytes) {
        token += kHex[byte >> 4];
        token += kHex[byte & 15];
    }
    SecureZeroMemory(bytes, sizeof(bytes));
    return token;
}

bool TokensEqual(std::string_view a, std::string_view b) {
    if (a.size() != b.size() || a.empty()) return false;
    unsigned char difference = 0;
    for (size_t i = 0; i < a.size(); ++i) difference |= static_cast<unsigned char>(a[i] ^ b[i]);
    return difference == 0;
}

bool IsPrivateIPv4(uint32_t address) {
    const uint32_t a = address >> 24;
    const uint32_t b = (address >> 16) & 0xFF;
    return a == 10 || (a == 172 && b >= 16 && b <= 31) || (a == 192 && b == 168) ||
           // Link-local: direct links without DHCP.
           (a == 169 && b == 254);
}

std::wstring FormatIPv4(uint32_t address) {
    return std::to_wstring(address >> 24) + L"." + std::to_wstring((address >> 16) & 0xFF) + L"." +
           std::to_wstring((address >> 8) & 0xFF) + L"." + std::to_wstring(address & 0xFF);
}

std::wstring FormatSize(uint64_t bytes) {
    static constexpr const wchar_t* kUnits[] = {L"B", L"KB", L"MB", L"GB", L"TB"};
    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1024 && unit < 4) {
        value /= 1024;
        ++unit;
    }
    wchar_t text[32];
    if (unit == 0) {
        swprintf_s(text, L"%llu B", static_cast<unsigned long long>(bytes));
    } else {
        swprintf_s(text, value < 10 ? L"%.1f %s" : L"%.0f %s", value, kUnits[unit]);
    }
    return text;
}

}  // namespace pons
