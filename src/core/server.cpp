#include "core/server.h"

#include "core/http.h"
#include "core/text.h"
#include "core/web_page.h"

#include <ws2tcpip.h>

#include <algorithm>
#include <charconv>

namespace pons {

namespace {

constexpr size_t kMaxHeadBytes = 16 * 1024;
constexpr size_t kMaxTextBytes = 64 * 1024;
constexpr size_t kChunkBytes = 256 * 1024;
constexpr size_t kMaxConnections = 32;
constexpr size_t kMaxTransfers = 50;
constexpr DWORD kSocketTimeoutMs = 30'000;
constexpr int kPortsToTry = 10;

constexpr char kCommonHeaders[] =
    "Cache-Control: no-store\r\n"
    "X-Content-Type-Options: nosniff\r\n"
    // The token is in the URL; never pass it on to another site.
    "Referrer-Policy: no-referrer\r\n";

bool SendAll(SOCKET socket, const char* data, size_t size) {
    while (size > 0) {
        const int sent = send(socket, data, static_cast<int>(std::min<size_t>(size, 1 << 20)), 0);
        if (sent <= 0) return false;
        data += sent;
        size -= static_cast<size_t>(sent);
    }
    return true;
}

const char* StatusText(int status) {
    switch (status) {
        case 200: return "OK";
        case 400: return "Bad Request";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 411: return "Length Required";
        case 413: return "Payload Too Large";
        case 500: return "Internal Server Error";
        case 507: return "Insufficient Storage";
        default: return "Error";
    }
}

std::string ResponseHead(int status, const char* contentType, uint64_t length, bool keepAlive,
                         const std::string& extraHeaders = {}) {
    std::string head = "HTTP/1.1 " + std::to_string(status) + " " + StatusText(status) + "\r\n";
    head += "Content-Type: ";
    head += contentType;
    head += "\r\nContent-Length: " + std::to_string(length) + "\r\n";
    head += kCommonHeaders;
    head += extraHeaders;
    head += keepAlive ? "Connection: keep-alive\r\n\r\n" : "Connection: close\r\n\r\n";
    return head;
}

bool SendResponse(SOCKET socket, int status, const char* contentType, const std::string& body, bool keepAlive,
                  const std::string& extraHeaders = {}) {
    const std::string response = ResponseHead(status, contentType, body.size(), keepAlive, extraHeaders) + body;
    return SendAll(socket, response.data(), response.size());
}

bool SendJson(SOCKET socket, int status, const std::string& json, bool keepAlive) {
    return SendResponse(socket, status, "application/json; charset=utf-8", json, keepAlive);
}

bool SendError(SOCKET socket, int status, const std::string& message) {
    // Errors close the connection: what is left of the request is unknown.
    SendJson(socket, status, "{\"error\":\"" + JsonEscape(message) + "\"}", false);
    return false;
}

}  // namespace

// A request body: the bytes already read with the head, then the socket,
// never past Content-Length.
struct Server::Body {
    SOCKET socket;
    std::string prefetched;
    uint64_t remaining;

    // Returns the number of bytes read, 0 at the end, -1 on errors.
    int Read(char* buffer, size_t capacity) {
        if (remaining == 0) return 0;
        const size_t wanted = static_cast<size_t>(std::min<uint64_t>(capacity, remaining));
        if (!prefetched.empty()) {
            const size_t count = std::min(wanted, prefetched.size());
            std::copy_n(prefetched.data(), count, buffer);
            prefetched.erase(0, count);
            remaining -= count;
            return static_cast<int>(count);
        }
        const int received = recv(socket, buffer, static_cast<int>(std::min<size_t>(wanted, INT_MAX)), 0);
        if (received <= 0) return -1;
        remaining -= static_cast<uint64_t>(received);
        return received;
    }

    // Bytes that arrived after this body: the start of the next request.
    std::string Leftover() {
        if (remaining > 0) return {};
        return std::move(prefetched);
    }
};

Server::Server(HWND notifyWindow, UINT notifyMessage) : notifyWindow_(notifyWindow), notifyMessage_(notifyMessage) {
    WSADATA data;
    WSAStartup(MAKEWORD(2, 2), &data);
}

Server::~Server() {
    Stop();
    WSACleanup();
}

bool Server::Start(const ServerOptions& options) {
    Stop();
    options_ = options;
    {
        std::lock_guard lock(mutex_);
        token_ = GenerateToken();
        receiveFolder_ = options.receiveFolder;
    }

    for (int offset = 0; offset < kPortsToTry; ++offset) {
        const int port = options.port + offset;
        if (port > 65535) break;
        SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listener == INVALID_SOCKET) return false;
        // No other program may take over the port while Pons uses it.
        BOOL exclusive = TRUE;
        setsockopt(listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(static_cast<u_short>(port));
        address.sin_addr.s_addr = htonl(options.address);
        if (bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0 &&
            listen(listener, SOMAXCONN) == 0) {
            listener_ = listener;
            port_ = port;
            running_ = true;
            lastActivity_ = GetTickCount64();
            acceptThread_ = std::thread(&Server::AcceptLoop, this);
            return true;
        }
        closesocket(listener);
    }
    return false;
}

void Server::Stop() {
    if (!running_.exchange(false)) return;
    closesocket(listener_);
    listener_ = INVALID_SOCKET;
    if (acceptThread_.joinable()) acceptThread_.join();

    std::unique_lock lock(mutex_);
    for (const SOCKET connection : connections_) shutdown(connection, SD_BOTH);
    idle_.wait(lock, [this] { return connections_.empty(); });
    // A new session gets a new token: old links stop working.
    token_.clear();
}

std::string Server::Token() const {
    std::lock_guard lock(mutex_);
    return token_;
}

std::wstring Server::Url() const {
    return L"http://" + FormatIPv4(options_.address) + L":" + std::to_wstring(port_) + L"/?t=" + FromUtf8(Token());
}

void Server::SetReceiveFolder(const std::filesystem::path& folder) {
    std::lock_guard lock(mutex_);
    receiveFolder_ = folder;
}

std::vector<uint32_t> Server::AddFiles(const std::vector<std::filesystem::path>& paths) {
    std::vector<uint32_t> ids;
    std::lock_guard lock(mutex_);
    for (const auto& path : paths) {
        std::error_code error;
        if (!std::filesystem::is_regular_file(path, error)) continue;
        const bool alreadyShared =
            std::any_of(files_.begin(), files_.end(), [&](const SharedFile& file) { return file.path == path; });
        if (alreadyShared) continue;
        SharedFile file;
        file.id = nextId_++;
        file.path = path;
        file.name = path.filename().wstring();
        file.size = std::filesystem::file_size(path, error);
        files_.push_back(file);
        ids.push_back(file.id);
    }
    return ids;
}

void Server::RemoveFile(uint32_t id) {
    std::lock_guard lock(mutex_);
    std::erase_if(files_, [id](const SharedFile& file) { return file.id == id; });
}

std::vector<SharedFile> Server::Files() const {
    std::lock_guard lock(mutex_);
    return files_;
}

void Server::SetText(const std::wstring& text) {
    std::lock_guard lock(mutex_);
    text_ = text;
    ++textRevision_;
}

std::wstring Server::Text() const {
    std::lock_guard lock(mutex_);
    return text_;
}

std::vector<Transfer> Server::Transfers() const {
    std::lock_guard lock(mutex_);
    return transfers_;
}

void Server::ClearFinishedTransfers() {
    std::lock_guard lock(mutex_);
    std::erase_if(transfers_, [](const Transfer& transfer) { return transfer.state != TransferState::Active; });
}

bool Server::HasActiveTransfers() const {
    std::lock_guard lock(mutex_);
    return std::any_of(transfers_.begin(), transfers_.end(),
                       [](const Transfer& transfer) { return transfer.state == TransferState::Active; });
}

void Server::AcceptLoop() {
    for (;;) {
        sockaddr_in peer{};
        int peerSize = sizeof(peer);
        const SOCKET connection = accept(listener_, reinterpret_cast<sockaddr*>(&peer), &peerSize);
        if (connection == INVALID_SOCKET) {
            if (!running_) return;
            continue;
        }
        const uint32_t peerAddress = ntohl(peer.sin_addr.s_addr);
        const bool loopback = (peerAddress >> 24) == 127;
        if (!(IsPrivateIPv4(peerAddress) || (loopback && options_.allowLoopback))) {
            closesocket(connection);
            continue;
        }
        {
            std::lock_guard lock(mutex_);
            if (!running_ || connections_.size() >= kMaxConnections) {
                closesocket(connection);
                continue;
            }
            connections_.insert(connection);
        }
        // A stalled phone cannot hold a connection forever.
        const DWORD timeout = kSocketTimeoutMs;
        setsockopt(connection, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
        setsockopt(connection, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
        std::thread([this, connection] {
            Serve(connection);
            std::lock_guard lock(mutex_);
            connections_.erase(connection);
            closesocket(connection);
            idle_.notify_all();
        }).detach();
    }
}

void Server::Serve(SOCKET socket) {
    std::string buffer;
    while (running_) {
        size_t headEnd;
        while ((headEnd = buffer.find("\r\n\r\n")) == std::string::npos) {
            if (buffer.size() > kMaxHeadBytes) return;
            char chunk[4096];
            const int received = recv(socket, chunk, sizeof(chunk), 0);
            if (received <= 0) return;
            buffer.append(chunk, static_cast<size_t>(received));
        }
        const auto request = ParseRequestHead(std::string_view(buffer).substr(0, headEnd));
        if (!request) {
            SendError(socket, 400, "Bad request");
            return;
        }
        Body body{socket, buffer.substr(headEnd + 4), request->contentLength};
        if (!Handle(socket, *request, body) || !request->keepAlive || body.remaining > 0) return;
        buffer = body.Leftover();
    }
}

bool Server::Handle(SOCKET socket, const RequestHead& request, Body& body) {
    if (request.path == "/favicon.ico") return SendResponse(socket, 404, "text/plain", "", request.keepAlive);

    const std::string token = QueryParam(request.query, "t").value_or("");
    if (!TokensEqual(token, Token())) {
        // Old QR codes and guessed addresses end here.
        if (request.path == "/") {
            return SendResponse(socket, 403, "text/html; charset=utf-8", ExpiredPageHtml(), false);
        }
        return SendError(socket, 403, "This link has expired. Scan the QR code on the PC again.");
    }
    lastActivity_ = GetTickCount64();

    if (request.method == "GET" && request.path == "/") {
        return SendResponse(socket, 200, "text/html; charset=utf-8", PageHtml(), request.keepAlive,
                            "Content-Security-Policy: default-src 'none'; script-src 'unsafe-inline'; "
                            "style-src 'unsafe-inline'; connect-src 'self'; img-src data:; form-action 'none'; "
                            "frame-ancestors 'none'\r\n");
    }
    if (request.method == "GET" && request.path == "/api/state") {
        return SendJson(socket, 200, StateJson(), request.keepAlive);
    }
    if (request.method == "GET" && request.path.starts_with("/f/")) {
        uint32_t id = 0;
        const std::string_view digits = std::string_view(request.path).substr(3);
        const auto result = std::from_chars(digits.data(), digits.data() + digits.size(), id);
        if (result.ec != std::errc() || result.ptr != digits.data() + digits.size()) return SendError(socket, 404, "Not found");
        return HandleDownload(socket, request, id);
    }
    if (request.method == "POST" && request.path == "/api/upload") return HandleUpload(socket, request, body);
    if (request.method == "POST" && request.path == "/api/text") {
        if (request.contentLength > kMaxTextBytes) return SendError(socket, 413, "The text is too long.");
        std::string text(static_cast<size_t>(request.contentLength), '\0');
        size_t read = 0;
        while (read < text.size()) {
            const int count = body.Read(text.data() + read, text.size() - read);
            if (count <= 0) return false;
            read += static_cast<size_t>(count);
        }
        {
            std::lock_guard lock(mutex_);
            text_ = FromUtf8(text);
            ++textRevision_;
        }
        Notify(ServerEvent::TextFromPhone);
        return SendJson(socket, 200, StateJson(), request.keepAlive);
    }
    if (request.path == "/" || request.path.starts_with("/api/") || request.path.starts_with("/f/")) {
        return SendError(socket, 405, "Method not allowed");
    }
    return SendError(socket, 404, "Not found");
}

bool Server::HandleUpload(SOCKET socket, const RequestHead& request, Body& body) {
    if (!request.hasContentLength) return SendError(socket, 411, "Length required");
    const std::wstring name = SanitizeFileName(FromUtf8(QueryParam(request.query, "name").value_or("")));

    std::filesystem::path folder;
    {
        std::lock_guard lock(mutex_);
        folder = receiveFolder_;
    }
    std::error_code error;
    std::filesystem::create_directories(folder, error);
    ULARGE_INTEGER freeBytes{};
    if (GetDiskFreeSpaceExW(folder.c_str(), &freeBytes, nullptr, nullptr) && freeBytes.QuadPart < request.contentLength) {
        return SendError(socket, 507, "There is not enough free space on the PC.");
    }

    // Written under a temporary name first, so a half-received file never
    // looks complete.
    const std::filesystem::path partial = UniquePath(folder, name + L".pons-part");
    HANDLE file = CreateFileW(partial.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return SendError(socket, 500, "The file could not be saved on the PC.");

    const uint32_t id = BeginTransfer(Direction::FromPhone, name, request.contentLength);
    std::vector<char> chunk(kChunkBytes);
    uint64_t written = 0;
    bool ok = true;
    while (written < request.contentLength) {
        const int count = body.Read(chunk.data(), chunk.size());
        DWORD wrote = 0;
        if (count <= 0 || !running_ || !WriteFile(file, chunk.data(), static_cast<DWORD>(count), &wrote, nullptr) ||
            wrote != static_cast<DWORD>(count)) {
            ok = false;
            break;
        }
        written += static_cast<uint64_t>(count);
        lastActivity_ = GetTickCount64();
        UpdateTransfer(id, written);
    }
    CloseHandle(file);

    std::filesystem::path saved;
    if (ok) {
        saved = UniquePath(folder, name);
        ok = MoveFileExW(partial.c_str(), saved.c_str(), 0) != FALSE;
    }
    if (!ok) DeleteFileW(partial.c_str());
    FinishTransfer(id, ok, saved);
    if (!ok) return SendError(socket, 500, "The file could not be saved on the PC.");
    return SendJson(socket, 200, "{\"name\":\"" + JsonEscape(ToUtf8(saved.filename().wstring())) + "\"}", request.keepAlive);
}

bool Server::HandleDownload(SOCKET socket, const RequestHead& request, uint32_t id) {
    SharedFile shared;
    {
        std::lock_guard lock(mutex_);
        const auto found = std::find_if(files_.begin(), files_.end(), [id](const SharedFile& file) { return file.id == id; });
        if (found == files_.end()) return SendError(socket, 404, "This file is no longer shared.");
        shared = *found;
    }
    HANDLE file = CreateFileW(shared.path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                              FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) return SendError(socket, 404, "The file could not be opened on the PC.");
    LARGE_INTEGER size{};
    GetFileSizeEx(file, &size);
    const uint64_t total = static_cast<uint64_t>(size.QuadPart);

    const std::string head = ResponseHead(200, "application/octet-stream", total, request.keepAlive,
                                          "Content-Disposition: " + AttachmentHeader(shared.name) + "\r\n");
    const uint32_t transfer = BeginTransfer(Direction::ToPhone, shared.name, total);
    bool ok = SendAll(socket, head.data(), head.size());
    std::vector<char> chunk(kChunkBytes);
    uint64_t sent = 0;
    while (ok && sent < total) {
        DWORD read = 0;
        const DWORD wanted = static_cast<DWORD>(std::min<uint64_t>(chunk.size(), total - sent));
        if (!running_ || !ReadFile(file, chunk.data(), wanted, &read, nullptr) || read == 0 ||
            !SendAll(socket, chunk.data(), read)) {
            ok = false;
            break;
        }
        sent += read;
        lastActivity_ = GetTickCount64();
        UpdateTransfer(transfer, sent);
    }
    CloseHandle(file);
    FinishTransfer(transfer, ok);
    return ok;
}

std::string Server::StateJson() const {
    std::lock_guard lock(mutex_);
    std::string json = "{\"files\":[";
    for (size_t i = 0; i < files_.size(); ++i) {
        if (i) json += ',';
        json += "{\"id\":" + std::to_string(files_[i].id) + ",\"name\":\"" + JsonEscape(ToUtf8(files_[i].name)) +
                "\",\"size\":" + std::to_string(files_[i].size) + "}";
    }
    json += "],\"text\":\"" + JsonEscape(ToUtf8(text_)) + "\",\"textRevision\":" + std::to_string(textRevision_) +
            ",\"folder\":\"" + JsonEscape(ToUtf8(receiveFolder_.filename().wstring())) + "\"}";
    return json;
}

uint32_t Server::BeginTransfer(Direction direction, const std::wstring& name, uint64_t total) {
    uint32_t id;
    {
        std::lock_guard lock(mutex_);
        id = nextId_++;
        Transfer transfer;
        transfer.id = id;
        transfer.direction = direction;
        transfer.name = name;
        transfer.total = total;
        transfers_.push_back(transfer);
        // Forget the oldest finished transfers.
        while (transfers_.size() > kMaxTransfers) {
            const auto finished = std::find_if(transfers_.begin(), transfers_.end(),
                                               [](const Transfer& t) { return t.state != TransferState::Active; });
            if (finished == transfers_.end()) break;
            transfers_.erase(finished);
        }
    }
    Notify(ServerEvent::TransfersChanged);
    return id;
}

void Server::UpdateTransfer(uint32_t id, uint64_t done) {
    {
        std::lock_guard lock(mutex_);
        for (auto& transfer : transfers_) {
            if (transfer.id == id) transfer.done = done;
        }
    }
    Notify(ServerEvent::TransfersChanged, true);
}

void Server::FinishTransfer(uint32_t id, bool succeeded, const std::filesystem::path& path) {
    {
        std::lock_guard lock(mutex_);
        for (auto& transfer : transfers_) {
            if (transfer.id != id) continue;
            transfer.state = succeeded ? TransferState::Done : TransferState::Failed;
            transfer.path = path;
        }
    }
    Notify(ServerEvent::TransfersChanged);
}

void Server::Notify(ServerEvent event, bool throttle) {
    if (!notifyWindow_) return;
    const ULONGLONG now = GetTickCount64();
    // Progress is repainted ten times a second at most.
    if (throttle && now - lastNotify_ < 100) return;
    lastNotify_ = now;
    PostMessageW(notifyWindow_, notifyMessage_, static_cast<WPARAM>(event), 0);
}

}  // namespace pons
