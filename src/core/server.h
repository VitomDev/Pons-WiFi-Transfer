#pragma once

#include "core/http.h"

#include <winsock2.h>
#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace pons {

struct SharedFile {
    uint32_t id = 0;
    std::filesystem::path path;
    std::wstring name;
    uint64_t size = 0;
};

enum class Direction { ToPhone, FromPhone };
enum class TransferState { Active, Done, Failed };

struct Transfer {
    uint32_t id = 0;
    Direction direction = Direction::FromPhone;
    std::wstring name;
    std::filesystem::path path;  // where an upload was saved, once done
    uint64_t done = 0;
    uint64_t total = 0;
    TransferState state = TransferState::Active;
};

// Posted to the notify window as wParam of its message, from server threads.
enum class ServerEvent : WPARAM { TransfersChanged = 1, TextFromPhone = 2 };

struct ServerOptions {
    uint32_t address = 0;  // host byte order; the server listens on this address only
    int port = 0;          // the first of ten ports tried
    std::filesystem::path receiveFolder;
    bool allowLoopback = false;  // tests talk to 127.0.0.1
};

// The small HTTP server the phone talks to. Every request must carry the
// session token, which changes on each Start; only peers on a private
// network are accepted. Windows sockets only, no HTTP library.
class Server {
public:
    Server(HWND notifyWindow = nullptr, UINT notifyMessage = 0);
    ~Server();
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    bool Start(const ServerOptions& options);
    // Closes every connection and waits for transfers in progress to end.
    void Stop();

    void SetNotifyWindow(HWND window, UINT message) {
        notifyWindow_ = window;
        notifyMessage_ = message;
    }

    bool IsRunning() const { return running_; }
    int Port() const { return port_; }
    std::string Token() const;
    // http://192.168.1.20:8642/?t=<token>, what the QR code holds.
    std::wstring Url() const;

    void SetReceiveFolder(const std::filesystem::path& folder);

    // Files offered to the phone; they stay on disk where they are.
    std::vector<uint32_t> AddFiles(const std::vector<std::filesystem::path>& paths);
    void RemoveFile(uint32_t id);
    std::vector<SharedFile> Files() const;

    void SetText(const std::wstring& text);
    std::wstring Text() const;

    std::vector<Transfer> Transfers() const;
    void ClearFinishedTransfers();
    bool HasActiveTransfers() const;

    // GetTickCount64 of the last request or transferred chunk from the phone.
    ULONGLONG LastActivity() const { return lastActivity_; }

private:
    struct Body;

    void AcceptLoop();
    void Serve(SOCKET socket);
    bool Handle(SOCKET socket, const RequestHead& request, Body& body);
    bool HandleUpload(SOCKET socket, const RequestHead& request, Body& body);
    bool HandleDownload(SOCKET socket, const RequestHead& request, uint32_t id);
    std::string StateJson() const;

    uint32_t BeginTransfer(Direction direction, const std::wstring& name, uint64_t total);
    void UpdateTransfer(uint32_t id, uint64_t done);
    void FinishTransfer(uint32_t id, bool succeeded, const std::filesystem::path& path = {});
    void Notify(ServerEvent event, bool throttle = false);

    HWND notifyWindow_;
    UINT notifyMessage_;

    std::atomic<bool> running_{false};
    SOCKET listener_ = INVALID_SOCKET;
    std::thread acceptThread_;
    ServerOptions options_;
    int port_ = 0;

    mutable std::mutex mutex_;
    std::condition_variable idle_;
    std::set<SOCKET> connections_;
    std::string token_;
    std::filesystem::path receiveFolder_;
    std::vector<SharedFile> files_;
    std::vector<Transfer> transfers_;
    std::wstring text_;
    uint32_t textRevision_ = 0;
    uint32_t nextId_ = 1;

    std::atomic<ULONGLONG> lastActivity_{0};
    std::atomic<ULONGLONG> lastNotify_{0};
};

}  // namespace pons
