#pragma once

#include "app/theme.h"
#include "core/qr.h"
#include "core/server.h"
#include "core/settings.h"

#include <windows.h>

#include <filesystem>
#include <string>
#include <vector>

namespace pons {

inline constexpr wchar_t kMainWindowClass[] = L"PonsWiFiTransferWindow";

// Pons' only window: the QR code to scan, the files offered to the phone,
// the transfers and the shared text. Closing it stops the server.
class MainWindow {
public:
    MainWindow();
    ~MainWindow();
    MainWindow(const MainWindow&) = delete;
    MainWindow& operator=(const MainWindow&) = delete;

    bool Create(HINSTANCE instance, int show, const std::vector<std::filesystem::path>& files);
    bool PreTranslate(MSG& message);

private:
    enum class ServerState { Running, Idle, NoNetwork, PortBusy };

    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK TransfersProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);

    void CreateControls();
    void RefreshStyle();
    void Layout();
    void Paint(HDC dc);
    void PaintTransfers(HWND window, HDC dc);
    void DrawItem(const DRAWITEMSTRUCT& item);
    void OnCommand(int id, int code);

    void StartServer();
    void UpdateQr();
    void CheckIdleAndNetwork();
    void UpdateStatus();

    void AddFiles(const std::vector<std::filesystem::path>& paths);
    void ChooseFiles();
    void RemoveSelectedFiles();
    void RefreshFileList();
    void ChooseFolder();
    std::filesystem::path ReceiveFolder() const;
    void OnTransfersChanged();
    void OpenTransfer(int row);
    void ShareText();
    void ApplyPort();

    int Scale(int value) const { return MulDiv(value, static_cast<int>(dpi_), 96); }

    HINSTANCE instance_ = nullptr;
    HWND window_ = nullptr;
    HWND files_ = nullptr;
    HWND transfers_ = nullptr;
    HWND text_ = nullptr;
    HWND port_ = nullptr;
    HWND timeout_ = nullptr;
    std::vector<HWND> buttons_;

    Server server_;
    Settings settings_;
    ServerState state_ = ServerState::NoNetwork;
    uint32_t address_ = 0;
    ULONGLONG startedAt_ = 0;
    ULONGLONG lastNetworkCheck_ = 0;
    QrMatrix qr_;
    std::wstring status_;
    std::wstring detail_;
    std::vector<Transfer> transferRows_;

    UINT dpi_ = 96;
    Theme theme_{};
    HBRUSH background_ = nullptr;
    HBRUSH fieldBrush_ = nullptr;
    HFONT titleFont_ = nullptr;
    HFONT headingFont_ = nullptr;
    HFONT textFont_ = nullptr;
    HFONT smallFont_ = nullptr;
    HFONT iconFont_ = nullptr;

    // Layout in client pixels, computed by Layout and used by Paint.
    RECT qrRect_{};
    RECT statusRect_{};
    RECT filesHeading_{};
    RECT folderLabel_{};
    RECT transfersHeading_{};
    RECT textHeading_{};
    RECT portLabel_{};
    RECT timeoutLabel_{};
};

}  // namespace pons
