#pragma once

// Small, dependency-free pieces of the HTTP server, kept apart so they can be
// tested without sockets.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace pons {

struct RequestHead {
    std::string method;
    std::string path;   // decoded, without the query
    std::string query;  // raw, without the '?'
    uint64_t contentLength = 0;
    bool hasContentLength = false;
    bool keepAlive = true;
};

// Parses the request line and headers (everything before the blank line).
std::optional<RequestHead> ParseRequestHead(std::string_view head);

// Percent-decoding; '+' becomes a space when `plusIsSpace` (query strings).
std::string UrlDecode(std::string_view text, bool plusIsSpace = true);
std::string UrlEncode(std::string_view text);

// The decoded value of `name` in a query string, if present.
std::optional<std::string> QueryParam(std::string_view query, std::string_view name);

std::string JsonEscape(std::string_view text);

// A safe file name for anything the phone sends: no folders, reserved
// characters or device names, never empty.
std::wstring SanitizeFileName(std::wstring_view name);

// `folder\name`, or "name (2).ext" and so on when that file already exists.
std::filesystem::path UniquePath(const std::filesystem::path& folder, const std::wstring& name);

// The Content-Disposition value that makes browsers save the file with its
// real name, accents and all.
std::string AttachmentHeader(std::wstring_view fileName);

// 32 random hex characters from the system's cryptographic generator.
std::string GenerateToken();
// Compares in constant time, so the token cannot be guessed byte by byte.
bool TokensEqual(std::string_view a, std::string_view b);

// IPv4 address in host byte order.
bool IsPrivateIPv4(uint32_t address);
std::wstring FormatIPv4(uint32_t address);

// "3.4 MB" style sizes.
std::wstring FormatSize(uint64_t bytes);

}  // namespace pons
