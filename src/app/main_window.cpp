#include "app/main_window.h"

#include "app/resource.h"
#include "app/vitool_tray.h"
#include "core/http.h"
#include "core/network.h"
#include "core/paths.h"
#include "core/text.h"

#include <commctrl.h>
#include <uxtheme.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <windowsx.h>

#include <algorithm>
#include <cwchar>
#include <iterator>

namespace pons {

namespace {

constexpr wchar_t kTransfersClass[] = L"PonsTransferList";
constexpr UINT WM_APP_SERVER = WM_APP + 1;
constexpr UINT_PTR kTickTimer = 1;
constexpr ULONGLONG kNetworkCheckMs = 10'000;
// The page asks every two seconds; a little longer than that means gone.
constexpr ULONGLONG kDeviceSeenMs = 6'000;
constexpr int kTimeoutChoices[] = {5, 15, 30, 60, 0};
constexpr UINT WM_APP_TRAY = WM_APP + 2;
// How long Pons can keep sharing after its window is closed.
constexpr int kBackgroundChoices[] = {15, 30, 60, 120, 240};
// Item data in the "When closed" box: Ask, Quit, or keep running for N minutes.
constexpr LRESULT kCloseAsk = -1;
constexpr LRESULT kCloseQuit = 0;

enum ControlId {
    kCopyLink = 101,
    kNewLink,
    kAddFiles,
    kRemoveFiles,
    kChangeFolder,
    kOpenFolder,
    kClearTransfers,
    kSendText,
    kCopyText,
    kFileList = 200,
    kTransferList,
    kTextBox,
    kPortBox,
    kTimeoutBox,
    kCloseBox,
    kBackgroundRadio = 300,
    kKeepRunningButton = 400,
    kQuitButton,
    kMenuOpen = 500,
    kMenuCopyLink,
    kMenuQuit,
};

std::wstring FormatDuration(int minutes) {
    if (minutes % 60 != 0) return std::to_wstring(minutes) + (minutes == 1 ? L" minute" : L" minutes");
    const int hours = minutes / 60;
    return std::to_wstring(hours) + (hours == 1 ? L" hour" : L" hours");
}

// The local clock time `minutes` from now, e.g. "15:30".
std::wstring ClockTimeAfter(int minutes) {
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER ticks;
    ticks.LowPart = now.dwLowDateTime;
    ticks.HighPart = now.dwHighDateTime;
    ticks.QuadPart += static_cast<ULONGLONG>(minutes) * 60 * 10'000'000;
    const FILETIME later{ticks.LowPart, ticks.HighPart};
    SYSTEMTIME utc;
    SYSTEMTIME local;
    wchar_t text[64]{};
    if (FileTimeToSystemTime(&later, &utc) && SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local)) {
        GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, &local, nullptr, text, 64);
    }
    return text;
}

struct ButtonInfo {
    int id;
    const wchar_t* icon;
    const wchar_t* label;
    ButtonStyle style;
};

constexpr ButtonInfo kButtons[] = {
    {kCopyLink, icons::kCopy, L"Copy link", ButtonStyle::Standard},
    {kNewLink, icons::kRefresh, L"New link", ButtonStyle::Standard},
    {kAddFiles, icons::kAdd, L"Add files", ButtonStyle::Primary},
    {kRemoveFiles, icons::kDelete, L"Remove", ButtonStyle::Standard},
    {kChangeFolder, nullptr, L"Change…", ButtonStyle::Standard},
    {kOpenFolder, icons::kFolder, L"Open", ButtonStyle::Standard},
    {kClearTransfers, icons::kClear, L"Clear", ButtonStyle::Subtle},
    {kSendText, icons::kSend, L"Send to devices", ButtonStyle::Primary},
    {kCopyText, icons::kCopy, L"Copy", ButtonStyle::Standard},
};

const ButtonInfo* FindButton(int id) {
    for (const auto& button : kButtons) {
        if (button.id == id) return &button;
    }
    return nullptr;
}

void DrawTextIn(HDC dc, HFONT font, COLORREF color, const std::wstring& text, RECT rect, UINT format) {
    HGDIOBJ previous = SelectObject(dc, font);
    SetTextColor(dc, color);
    SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &rect, format | DT_NOPREFIX);
    SelectObject(dc, previous);
}

std::wstring WindowText(HWND window) {
    std::wstring text(static_cast<size_t>(GetWindowTextLengthW(window)) + 1, L'\0');
    text.resize(static_cast<size_t>(GetWindowTextW(window, text.data(), static_cast<int>(text.size()))));
    return text;
}

void CopyToClipboard(HWND owner, const std::wstring& text) {
    if (!OpenClipboard(owner)) return;
    EmptyClipboard();
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    if (HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
        memcpy(GlobalLock(memory), text.c_str(), bytes);
        GlobalUnlock(memory);
        if (!SetClipboardData(CF_UNICODETEXT, memory)) GlobalFree(memory);
    }
    CloseClipboard();
}

std::vector<std::filesystem::path> PickFiles(HWND owner, bool folders) {
    std::vector<std::filesystem::path> paths;
    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) return paths;
    FILEOPENDIALOGOPTIONS options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_FORCEFILESYSTEM | (folders ? FOS_PICKFOLDERS : FOS_ALLOWMULTISELECT));
    dialog->SetTitle(folders ? L"Where should received files go?" : L"Share files with your devices");
    IShellItemArray* items = nullptr;
    if (SUCCEEDED(dialog->Show(owner)) && SUCCEEDED(dialog->GetResults(&items))) {
        DWORD count = 0;
        items->GetCount(&count);
        for (DWORD i = 0; i < count; ++i) {
            IShellItem* item = nullptr;
            PWSTR path = nullptr;
            if (SUCCEEDED(items->GetItemAt(i, &item)) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                paths.emplace_back(path);
                CoTaskMemFree(path);
            }
            if (item) item->Release();
        }
        items->Release();
    }
    dialog->Release();
    return paths;
}

}  // namespace

MainWindow::MainWindow() = default;

MainWindow::~MainWindow() {
    for (HGDIOBJ object : std::initializer_list<HGDIOBJ>{background_, fieldBrush_, titleFont_, headingFont_, textFont_,
                                                         smallFont_, iconFont_}) {
        if (object) DeleteObject(object);
    }
}

bool MainWindow::Create(HINSTANCE instance, int show, const std::vector<std::filesystem::path>& files) {
    instance_ = instance;
    settings_ = Settings::Load();

    WNDCLASSEXW windowClass{sizeof(windowClass)};
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = instance;
    windowClass.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_PONS));
    // The title bar uses the 16 px image, not a shrunken 32 px one.
    windowClass.hIconSm = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(IDI_PONS), IMAGE_ICON,
                                                        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.lpszClassName = kMainWindowClass;
    if (!RegisterClassExW(&windowClass)) return false;

    WNDCLASSEXW listClass{sizeof(listClass)};
    listClass.lpfnWndProc = TransfersProc;
    listClass.hInstance = instance;
    listClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    listClass.lpszClassName = kTransfersClass;
    if (!RegisterClassExW(&listClass)) return false;

    dpi_ = GetDpiForSystem();
    RECT frame{0, 0, Scale(840), Scale(640)};
    AdjustWindowRectExForDpi(&frame, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi_);
    window_ = CreateWindowExW(WS_EX_ACCEPTFILES, kMainWindowClass, L"Pons — WiFi Transfer", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                              CW_USEDEFAULT, CW_USEDEFAULT, frame.right - frame.left, frame.bottom - frame.top, nullptr,
                              nullptr, instance, this);
    if (!window_) return false;

    server_.SetNotifyWindow(window_, WM_APP_SERVER);
    taskbarCreatedMessage_ = RegisterWindowMessageW(L"TaskbarCreated");
    idleSince_ = GetTickCount64();
    CreateControls();
    RefreshStyle();
    Layout();
    StartServer();
    AddFiles(files);
    SetTimer(window_, kTickTimer, 1000, nullptr);
    ShowWindow(window_, show);
    UpdateWindow(window_);
    return true;
}

bool MainWindow::PreTranslate(MSG& message) {
    if (message.hwnd == port_ && message.message == WM_KEYDOWN && message.wParam == VK_RETURN) {
        ApplyPort();
        return true;
    }
    return IsDialogMessageW(window_, &message) != FALSE;
}

// --- Window -------------------------------------------------------------------

LRESULT CALLBACK MainWindow::WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCCREATE) {
        auto* self = static_cast<MainWindow*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        self->window_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    auto* self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    return self ? self->HandleMessage(message, wParam, lParam) : DefWindowProcW(window, message, wParam, lParam);
}

LRESULT MainWindow::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
    if (vitool_tray::HandleMessage(message, wParam)) return 0;
    if (message == taskbarCreatedMessage_ && taskbarCreatedMessage_ != 0) {
        // Explorer restarted: the tray icon has to be added again.
        if (trayIconAdded_) AddTrayIcon();
        return 0;
    }
    switch (message) {
        case WM_PAINT: {
            PAINTSTRUCT paint;
            HDC dc = BeginPaint(window_, &paint);
            RECT client;
            GetClientRect(window_, &client);
            // Double-buffered against flicker while transfers repaint.
            HDC memory = CreateCompatibleDC(dc);
            HBITMAP bitmap = CreateCompatibleBitmap(dc, client.right, client.bottom);
            HGDIOBJ previous = SelectObject(memory, bitmap);
            Paint(memory);
            BitBlt(dc, 0, 0, client.right, client.bottom, memory, 0, 0, SRCCOPY);
            SelectObject(memory, previous);
            DeleteObject(bitmap);
            DeleteDC(memory);
            EndPaint(window_, &paint);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_SIZE:
            Layout();
            return 0;
        case WM_GETMINMAXINFO: {
            auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
            RECT frame{0, 0, Scale(720), Scale(600)};
            AdjustWindowRectExForDpi(&frame, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi_);
            info->ptMinTrackSize = {frame.right - frame.left, frame.bottom - frame.top};
            return 0;
        }
        case WM_DPICHANGED: {
            dpi_ = HIWORD(wParam);
            const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
            RefreshStyle();
            SetWindowPos(window_, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                         suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
            Layout();
            return 0;
        }
        case WM_SETTINGCHANGE:
            if (lParam && wcscmp(reinterpret_cast<const wchar_t*>(lParam), L"ImmersiveColorSet") == 0) {
                RefreshStyle();
                InvalidateRect(window_, nullptr, TRUE);
            }
            break;
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX: {
            HDC dc = reinterpret_cast<HDC>(wParam);
            SetTextColor(dc, theme_.text);
            SetBkColor(dc, theme_.dark ? RGB(32, 32, 32) : RGB(255, 255, 255));
            return reinterpret_cast<LRESULT>(fieldBrush_);
        }
        case WM_CTLCOLORSTATIC: {
            HDC dc = reinterpret_cast<HDC>(wParam);
            SetTextColor(dc, theme_.text);
            SetBkColor(dc, theme_.background);
            return reinterpret_cast<LRESULT>(background_);
        }
        case WM_DRAWITEM:
            DrawItem(*reinterpret_cast<const DRAWITEMSTRUCT*>(lParam));
            return TRUE;
        case WM_COMMAND:
            OnCommand(LOWORD(wParam), HIWORD(wParam));
            return 0;
        case WM_NOTIFY: {
            const auto* header = reinterpret_cast<const NMHDR*>(lParam);
            if (header->idFrom == kFileList && header->code == LVN_GETEMPTYMARKUP) {
                auto* markup = reinterpret_cast<NMLVEMPTYMARKUP*>(lParam);
                markup->dwFlags = EMF_CENTERED;
                wcscpy_s(markup->szMarkup, L"Drag files here to share them with your devices");
                return TRUE;
            }
            if (header->idFrom == kFileList && header->code == LVN_KEYDOWN &&
                reinterpret_cast<const NMLVKEYDOWN*>(lParam)->wVKey == VK_DELETE) {
                RemoveSelectedFiles();
            }
            if (header->idFrom == kFileList && header->code == LVN_ITEMCHANGED) {
                EnableWindow(GetDlgItem(window_, kRemoveFiles), ListView_GetSelectedCount(files_) > 0);
            }
            break;
        }
        case WM_DROPFILES: {
            HDROP drop = reinterpret_cast<HDROP>(wParam);
            std::vector<std::filesystem::path> paths;
            const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
            for (UINT i = 0; i < count; ++i) {
                std::wstring path(DragQueryFileW(drop, i, nullptr, 0) + 1, L'\0');
                path.resize(DragQueryFileW(drop, i, path.data(), static_cast<UINT>(path.size())));
                paths.emplace_back(path);
            }
            DragFinish(drop);
            AddFiles(paths);
            return 0;
        }
        case WM_COPYDATA: {
            // Files given to a second launch (e.g. dropped on the shortcut).
            const auto* data = reinterpret_cast<const COPYDATASTRUCT*>(lParam);
            std::vector<std::filesystem::path> paths;
            const std::wstring_view list(static_cast<const wchar_t*>(data->lpData), data->cbData / sizeof(wchar_t));
            size_t start = 0;
            while (start < list.size()) {
                const size_t end = std::min(list.find(L'\n', start), list.size());
                if (end > start) paths.emplace_back(list.substr(start, end - start));
                start = end + 1;
            }
            AddFiles(paths);
            if (inBackground_) LeaveBackground();
            if (IsIconic(window_)) ShowWindow(window_, SW_RESTORE);
            SetForegroundWindow(window_);
            return TRUE;
        }
        case WM_APP_TRAY:
            switch (LOWORD(lParam)) {
                case NIN_SELECT:
                case NIN_KEYSELECT:
                case NIN_BALLOONUSERCLICK:
                    LeaveBackground();
                    break;
                case WM_CONTEXTMENU:
                    ShowTrayMenu({GET_X_LPARAM(wParam), GET_Y_LPARAM(wParam)});
                    break;
            }
            return 0;
        case WM_APP_SERVER:
            if (static_cast<ServerEvent>(wParam) == ServerEvent::TextFromDevice) {
                SetWindowTextW(text_, ToWindowsLineBreaks(server_.Text()).c_str());
                FLASHWINFO flash{sizeof(flash), window_, FLASHW_TRAY | FLASHW_TIMERNOFG, 3, 0};
                if (GetForegroundWindow() != window_) FlashWindowEx(&flash);
            } else {
                OnTransfersChanged();
            }
            UpdateStatus();
            return 0;
        case WM_TIMER:
            if (wParam == kTickTimer) {
                // Time is up, but a download still running gets to finish.
                if (inBackground_ && GetTickCount64() >= backgroundUntil_ && !server_.HasActiveTransfers()) {
                    DestroyWindow(window_);
                    return 0;
                }
                CheckIdleAndNetwork();
                UpdateStatus();
            }
            return 0;
        case WM_SYSCOMMAND:
            // The user closing the window (title bar, Alt+F4, taskbar).
            if ((wParam & 0xFFF0) == SC_CLOSE) {
                OnCloseRequested();
                return 0;
            }
            break;
        case WM_CLOSE:
            // Sent by the installer before upgrading or uninstalling: quit without asking.
            DestroyWindow(window_);
            return 0;
        case WM_DESTROY:
            KillTimer(window_, kTickTimer);
            RemoveTrayIcon();
            server_.Stop();
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(window_, message, wParam, lParam);
}

void MainWindow::CreateControls() {
    for (const auto& button : kButtons) {
        HWND control = CreateWindowExW(0, WC_BUTTONW, button.label, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 0,
                                       0, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(button.id)), instance_, nullptr);
        EnableHover(control);
        buttons_.push_back(control);
    }
    EnableWindow(GetDlgItem(window_, kRemoveFiles), FALSE);

    files_ = CreateWindowExW(0, WC_LISTVIEWW, nullptr,
                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_NOCOLUMNHEADER | LVS_SHOWSELALWAYS, 0, 0,
                             0, 0, window_, reinterpret_cast<HMENU>(kFileList), instance_, nullptr);
    ListView_SetExtendedListViewStyle(files_, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    LVCOLUMNW column{LVCF_WIDTH | LVCF_FMT};
    column.fmt = LVCFMT_LEFT;
    ListView_InsertColumn(files_, 0, &column);
    column.fmt = LVCFMT_RIGHT;
    ListView_InsertColumn(files_, 1, &column);

    transfers_ = CreateWindowExW(0, kTransfersClass, nullptr, WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_,
                                 reinterpret_cast<HMENU>(kTransferList), instance_, nullptr);
    SetWindowLongPtrW(transfers_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));

    text_ = CreateWindowExW(0, WC_EDITW, nullptr,
                            WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN,
                            0, 0, 0, 0, window_, reinterpret_cast<HMENU>(kTextBox), instance_, nullptr);
    SendMessageW(text_, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"Text or a link to send to your devices"));

    port_ = CreateWindowExW(0, WC_EDITW, std::to_wstring(settings_.port).c_str(),
                            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER | ES_CENTER, 0, 0, 0, 0, window_,
                            reinterpret_cast<HMENU>(kPortBox), instance_, nullptr);
    SendMessageW(port_, EM_LIMITTEXT, 5, 0);

    timeout_ = CreateWindowExW(0, WC_COMBOBOXW, nullptr, WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, 0,
                               0, 0, 0, window_, reinterpret_cast<HMENU>(kTimeoutBox), instance_, nullptr);
    for (const int minutes : kTimeoutChoices) {
        const std::wstring label = minutes ? std::to_wstring(minutes) + L" minutes" : L"Never";
        const auto index = SendMessageW(timeout_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
        SendMessageW(timeout_, CB_SETITEMDATA, index, minutes);
        if (minutes == settings_.timeoutMinutes) SendMessageW(timeout_, CB_SETCURSEL, index, 0);
    }
    if (SendMessageW(timeout_, CB_GETCURSEL, 0, 0) == CB_ERR) SendMessageW(timeout_, CB_SETCURSEL, 1, 0);

    closeChoice_ = CreateWindowExW(0, WC_COMBOBOXW, nullptr, WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                                   0, 0, 0, 0, window_, reinterpret_cast<HMENU>(kCloseBox), instance_, nullptr);
    const auto addChoice = [&](const std::wstring& label, LRESULT data) {
        const auto index = SendMessageW(closeChoice_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
        SendMessageW(closeChoice_, CB_SETITEMDATA, index, data);
    };
    addChoice(L"Ask first", kCloseAsk);
    addChoice(L"Quit", kCloseQuit);
    for (const int minutes : kBackgroundChoices) addChoice(L"Keep " + FormatDuration(minutes), minutes);
    SelectCloseChoice();
}

void MainWindow::SelectCloseChoice() {
    const LRESULT wanted = settings_.closeAction == CloseAction::Ask    ? kCloseAsk
                           : settings_.closeAction == CloseAction::Quit ? kCloseQuit
                                                                        : settings_.backgroundMinutes;
    const auto count = SendMessageW(closeChoice_, CB_GETCOUNT, 0, 0);
    for (LRESULT index = 0; index < count; ++index) {
        if (SendMessageW(closeChoice_, CB_GETITEMDATA, index, 0) == wanted) {
            SendMessageW(closeChoice_, CB_SETCURSEL, index, 0);
            return;
        }
    }
    // A duration set outside the list (e.g. by hand in the registry).
    const std::wstring label = L"Keep " + FormatDuration(static_cast<int>(wanted));
    const auto index = SendMessageW(closeChoice_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
    SendMessageW(closeChoice_, CB_SETITEMDATA, index, wanted);
    SendMessageW(closeChoice_, CB_SETCURSEL, index, 0);
}

void MainWindow::RefreshStyle() {
    theme_ = Theme::Current();
    for (HGDIOBJ object : std::initializer_list<HGDIOBJ>{background_, fieldBrush_, titleFont_, headingFont_, textFont_,
                                                         smallFont_, iconFont_}) {
        if (object) DeleteObject(object);
    }
    background_ = CreateSolidBrush(theme_.background);
    fieldBrush_ = CreateSolidBrush(theme_.dark ? RGB(32, 32, 32) : RGB(255, 255, 255));
    titleFont_ = CreateUiFont(dpi_, 18, FW_SEMIBOLD);
    headingFont_ = CreateUiFont(dpi_, 11, FW_SEMIBOLD);
    textFont_ = CreateUiFont(dpi_, 10);
    smallFont_ = CreateUiFont(dpi_, 9);
    iconFont_ = CreateIconFont(dpi_, 10);

    ApplyWindowStyle(window_, theme_);
    for (HWND control : {files_, text_, port_, timeout_, closeChoice_}) {
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(textFont_), TRUE);
    }
    SetWindowTheme(timeout_, theme_.dark ? L"DarkMode_CFD" : L"CFD", nullptr);
    SetWindowTheme(closeChoice_, theme_.dark ? L"DarkMode_CFD" : L"CFD", nullptr);
    SetWindowTheme(port_, theme_.dark ? L"DarkMode_CFD" : L"CFD", nullptr);
    const COLORREF field = theme_.dark ? RGB(32, 32, 32) : RGB(255, 255, 255);
    ListView_SetBkColor(files_, field);
    ListView_SetTextBkColor(files_, field);
    ListView_SetTextColor(files_, theme_.text);
    InvalidateRect(window_, nullptr, TRUE);
}

void MainWindow::Layout() {
    RECT client;
    GetClientRect(window_, &client);
    const int margin = Scale(20);
    const int buttonHeight = Scale(32);
    const int gap = Scale(8);
    const ButtonFonts fonts{textFont_, iconFont_};
    const auto width = [&](int id) {
        const ButtonInfo* info = FindButton(id);
        return ButtonWidth(window_, fonts, info->icon, info->label, dpi_);
    };
    HDWP batch = BeginDeferWindowPos(16);
    const auto place = [&](int id, int x, int y, int w, int h) {
        batch = DeferWindowPos(batch, GetDlgItem(window_, id), nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
    };

    // Left: the QR code and the connection.
    const int left = margin;
    const int leftWidth = Scale(260);
    qrRect_ = {left, margin + Scale(48), left + leftWidth, margin + Scale(48) + leftWidth};
    statusRect_ = {left, qrRect_.bottom + Scale(12), left + leftWidth, qrRect_.bottom + Scale(56)};
    const int half = (leftWidth - gap) / 2;
    place(kCopyLink, left, statusRect_.bottom + gap, half, buttonHeight);
    place(kNewLink, left + half + gap, statusRect_.bottom + gap, half, buttonHeight);
    const int fieldHeight = Scale(30);
    const int optionsTop = statusRect_.bottom + gap + buttonHeight + Scale(20);
    portLabel_ = {left, optionsTop, left + Scale(130), optionsTop + fieldHeight};
    place(kPortBox, left + leftWidth - Scale(120), optionsTop, Scale(120), fieldHeight);
    timeoutLabel_ = {left, optionsTop + fieldHeight + gap, left + Scale(130), optionsTop + 2 * fieldHeight + gap};
    place(kTimeoutBox, left + leftWidth - Scale(120), timeoutLabel_.top, Scale(120), Scale(200));
    closeLabel_ = {left, timeoutLabel_.bottom + gap, left + Scale(130), timeoutLabel_.bottom + gap + fieldHeight};
    place(kCloseBox, left + leftWidth - Scale(120), closeLabel_.top, Scale(120), Scale(240));

    // Right: shared files, transfers and text.
    const int right = client.right - margin;
    const int x = left + leftWidth + Scale(28);
    int y = margin;
    filesHeading_ = {x, y, right, y + buttonHeight};
    int buttonX = right - width(kAddFiles);
    place(kAddFiles, buttonX, y, width(kAddFiles), buttonHeight);
    buttonX -= gap + width(kRemoveFiles);
    place(kRemoveFiles, buttonX, y, width(kRemoveFiles), buttonHeight);
    y += buttonHeight + gap;
    const int listHeight = Scale(150);
    place(kFileList, x, y, right - x, listHeight);
    ListView_SetColumnWidth(files_, 1, Scale(90));
    ListView_SetColumnWidth(files_, 0, right - x - Scale(90) - GetSystemMetricsForDpi(SM_CXVSCROLL, dpi_) - Scale(4));
    y += listHeight + gap;

    folderLabel_ = {x, y, right, y + buttonHeight};
    buttonX = right - width(kOpenFolder);
    place(kOpenFolder, buttonX, y, width(kOpenFolder), buttonHeight);
    buttonX -= gap + width(kChangeFolder);
    place(kChangeFolder, buttonX, y, width(kChangeFolder), buttonHeight);
    folderLabel_.right = buttonX - gap;
    y += buttonHeight + Scale(16);

    const int textHeight = Scale(80);
    textHeading_ = {x, client.bottom - margin - textHeight - gap - Scale(24), right, client.bottom - margin - textHeight - gap};
    const int sideWidth = std::max(width(kSendText), width(kCopyText));
    place(kTextBox, x, textHeading_.bottom + gap, right - x - sideWidth - gap, textHeight);
    place(kSendText, right - sideWidth, textHeading_.bottom + gap, sideWidth, buttonHeight);
    place(kCopyText, right - sideWidth, textHeading_.bottom + gap + buttonHeight + gap, sideWidth, buttonHeight);

    transfersHeading_ = {x, y, right, y + buttonHeight};
    place(kClearTransfers, right - width(kClearTransfers), y, width(kClearTransfers), buttonHeight);
    y += buttonHeight + Scale(4);
    place(kTransferList, x, y, right - x, std::max(0, static_cast<int>(textHeading_.top) - Scale(12) - y));

    EndDeferWindowPos(batch);
    InvalidateRect(window_, nullptr, FALSE);
}

void MainWindow::Paint(HDC dc) {
    RECT client;
    GetClientRect(window_, &client);
    FillRect(dc, &client, background_);
    const int margin = Scale(20);

    DrawTextIn(dc, titleFont_, theme_.text, L"Pons", {margin, margin - Scale(4), qrRect_.right, margin + Scale(28)},
               DT_LEFT | DT_SINGLELINE);
    DrawTextIn(dc, smallFont_, theme_.secondaryText, L"WiFi Transfer · same network, nothing to install",
               {margin, margin + Scale(24), qrRect_.right, margin + Scale(44)}, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);

    // The QR code on a white card, so phone cameras read it in dark mode too.
    const bool showQr = state_ == ServerState::Running && !qr_.empty();
    FillRounded(dc, qrRect_, showQr ? RGB(255, 255, 255) : theme_.hover, Scale(8));
    if (showQr) {
        const int modules = qr_.size + 2 * kQuietZoneModules;
        const int side = qrRect_.right - qrRect_.left;
        const int module = std::max(1, side / modules);
        const int offset = (side - module * modules) / 2 + module * kQuietZoneModules;
        HBRUSH black = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        for (int row = 0; row < qr_.size; ++row) {
            for (int col = 0; col < qr_.size; ++col) {
                if (!qr_.IsDark(col, row)) continue;
                RECT cell{qrRect_.left + offset + col * module, qrRect_.top + offset + row * module, 0, 0};
                cell.right = cell.left + module;
                cell.bottom = cell.top + module;
                FillRect(dc, &cell, black);
            }
        }
    } else {
        RECT inner = qrRect_;
        InflateRect(&inner, -Scale(24), 0);
        const wchar_t* message = state_ == ServerState::Idle ? L"Stopped to keep your PC safe.\nPress Start for a new code."
                                 : state_ == ServerState::PortBusy ? L"The port is in use.\nChoose another port below."
                                                                   : L"Connect this PC to the same\nnetwork as your other devices.";
        RECT measure = inner;
        DrawTextIn(dc, textFont_, theme_.secondaryText, message, measure, DT_CENTER | DT_WORDBREAK | DT_CALCRECT);
        const int height = measure.bottom - measure.top;
        inner.top = (qrRect_.top + qrRect_.bottom - height) / 2;
        inner.bottom = inner.top + height;
        DrawTextIn(dc, textFont_, theme_.secondaryText, message, inner, DT_CENTER | DT_WORDBREAK);
    }

    // Status: a colored dot, a short line and the details.
    const bool deviceSeen = state_ == ServerState::Running && server_.LastActivity() > startedAt_ &&
                           GetTickCount64() - server_.LastActivity() < kDeviceSeenMs;
    const COLORREF dotColor = state_ != ServerState::Running ? RGB(200, 70, 60) : deviceSeen ? RGB(40, 170, 90) : theme_.accent;
    const int dot = Scale(8);
    const int lineHeight = Scale(22);
    RECT dotRect{statusRect_.left, statusRect_.top + (lineHeight - dot) / 2, statusRect_.left + dot,
                 statusRect_.top + (lineHeight - dot) / 2 + dot};
    FillRounded(dc, dotRect, dotColor, dot / 2);
    DrawTextIn(dc, headingFont_, theme_.text, status_,
               {statusRect_.left + dot + Scale(8), statusRect_.top, statusRect_.right, statusRect_.top + lineHeight},
               DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    DrawTextIn(dc, smallFont_, theme_.secondaryText, detail_,
               {statusRect_.left, statusRect_.top + lineHeight, statusRect_.right, statusRect_.bottom},
               DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);

    DrawTextIn(dc, textFont_, theme_.text, L"Port", portLabel_, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    DrawTextIn(dc, textFont_, theme_.text, L"Stop when idle for", timeoutLabel_, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    DrawTextIn(dc, textFont_, theme_.text, L"When closed", closeLabel_, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    DrawTextIn(dc, headingFont_, theme_.text, L"Shared with your devices", filesHeading_, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    RECT folderCaption = folderLabel_;
    folderCaption.bottom = folderCaption.top + (folderLabel_.bottom - folderLabel_.top) / 2;
    DrawTextIn(dc, smallFont_, theme_.secondaryText, L"Files from your devices go to", folderCaption,
               DT_LEFT | DT_SINGLELINE | DT_BOTTOM);
    RECT folderPath = folderLabel_;
    folderPath.top = folderCaption.bottom;
    DrawTextIn(dc, textFont_, theme_.text, ReceiveFolder().wstring(), folderPath, DT_LEFT | DT_SINGLELINE | DT_PATH_ELLIPSIS);
    DrawTextIn(dc, headingFont_, theme_.text, L"Transfers", transfersHeading_, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    DrawTextIn(dc, headingFont_, theme_.text, L"Shared text", textHeading_, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
}

void MainWindow::DrawItem(const DRAWITEMSTRUCT& item) {
    const ButtonInfo* info = FindButton(static_cast<int>(item.CtlID));
    if (!info) return;
    const wchar_t* label = info->label;
    const wchar_t* icon = info->icon;
    if (info->id == kNewLink && state_ != ServerState::Running) {
        label = L"Start";
        icon = icons::kPlay;
    }
    DrawButton(item, theme_, {textFont_, iconFont_}, info->style, icon, label, Scale(4));
}

void MainWindow::OnCommand(int id, int code) {
    switch (id) {
        case kCopyLink:
            if (state_ == ServerState::Running) CopyToClipboard(window_, server_.Url());
            break;
        case kNewLink:
            StartServer();
            break;
        case kAddFiles:
            ChooseFiles();
            break;
        case kRemoveFiles:
            RemoveSelectedFiles();
            break;
        case kChangeFolder:
            ChooseFolder();
            break;
        case kOpenFolder: {
            std::error_code error;
            std::filesystem::create_directories(ReceiveFolder(), error);
            ShellExecuteW(window_, L"open", ReceiveFolder().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            break;
        }
        case kClearTransfers:
            server_.ClearFinishedTransfers();
            OnTransfersChanged();
            break;
        case kSendText:
            ShareText();
            break;
        case kCopyText:
            CopyToClipboard(window_, WindowText(text_));
            break;
        case kPortBox:
            if (code == EN_KILLFOCUS) ApplyPort();
            break;
        case kTimeoutBox:
            if (code == CBN_SELCHANGE) {
                const auto index = SendMessageW(timeout_, CB_GETCURSEL, 0, 0);
                settings_.timeoutMinutes = static_cast<int>(SendMessageW(timeout_, CB_GETITEMDATA, index, 0));
                settings_.Save();
            }
            break;
        case kCloseBox:
            if (code == CBN_SELCHANGE) {
                const auto index = SendMessageW(closeChoice_, CB_GETCURSEL, 0, 0);
                const LRESULT data = SendMessageW(closeChoice_, CB_GETITEMDATA, index, 0);
                if (data == kCloseAsk) {
                    settings_.closeAction = CloseAction::Ask;
                } else if (data == kCloseQuit) {
                    settings_.closeAction = CloseAction::Quit;
                } else {
                    settings_.closeAction = CloseAction::KeepRunning;
                    settings_.backgroundMinutes = static_cast<int>(data);
                }
                settings_.Save();
            }
            break;
    }
}

// --- Server -------------------------------------------------------------------

void MainWindow::StartServer() {
    // Shared files and text survive a new link; only the token changes.
    server_.Stop();
    const auto address = FindLocalAddress();
    lastNetworkCheck_ = GetTickCount64();
    if (!address) {
        state_ = ServerState::NoNetwork;
    } else {
        address_ = *address;
        ServerOptions options;
        options.address = address_;
        options.port = settings_.port;
        options.receiveFolder = ReceiveFolder();
        state_ = server_.Start(options) ? ServerState::Running : ServerState::PortBusy;
        startedAt_ = server_.LastActivity();
    }
    UpdateQr();
    UpdateStatus();
    InvalidateRect(window_, nullptr, FALSE);
    for (HWND button : buttons_) InvalidateRect(button, nullptr, FALSE);
}

void MainWindow::UpdateQr() {
    qr_ = {};
    if (state_ == ServerState::Running) {
        if (auto matrix = EncodeQrCode(server_.Url())) qr_ = std::move(*matrix);
    }
    InvalidateRect(window_, &qrRect_, FALSE);
}

void MainWindow::CheckIdleAndNetwork() {
    const ULONGLONG now = GetTickCount64();
    // In the background the chosen time replaces the idle timeout.
    if (state_ == ServerState::Running && !inBackground_ && settings_.timeoutMinutes > 0 && !server_.HasActiveTransfers() &&
        now - std::max(server_.LastActivity(), idleSince_) > static_cast<ULONGLONG>(settings_.timeoutMinutes) * 60'000) {
        server_.Stop();
        state_ = ServerState::Idle;
        UpdateQr();
        for (HWND button : buttons_) InvalidateRect(button, nullptr, FALSE);
        return;
    }
    if (now - lastNetworkCheck_ < kNetworkCheckMs) return;
    lastNetworkCheck_ = now;
    const auto address = FindLocalAddress();
    // Joined a network, or moved to another one: a new code for the new address.
    if ((state_ == ServerState::NoNetwork && address) ||
        (state_ == ServerState::Running && (!address || *address != address_) && !server_.HasActiveTransfers())) {
        StartServer();
    }
}

void MainWindow::UpdateStatus() {
    std::wstring status;
    std::wstring detail;
    switch (state_) {
        case ServerState::Running: {
            const bool deviceSeen =
                server_.LastActivity() > startedAt_ && GetTickCount64() - server_.LastActivity() < kDeviceSeenMs;
            status = deviceSeen ? L"Device connected" : L"Scan the code or open the link";
            detail = L"http://" + FormatIPv4(address_) + L":" + std::to_wstring(server_.Port());
            break;
        }
        case ServerState::Idle:
            status = L"Stopped";
            detail = L"No activity for " + std::to_wstring(settings_.timeoutMinutes) + L" minutes";
            break;
        case ServerState::NoNetwork:
            status = L"No local network";
            detail = L"Pons starts as soon as the PC is connected";
            break;
        case ServerState::PortBusy:
            status = L"Could not start";
            detail = L"Ports " + std::to_wstring(settings_.port) + L"–" + std::to_wstring(settings_.port + 9) + L" are busy";
            break;
    }
    if (status == status_ && detail == detail_) return;
    status_ = std::move(status);
    detail_ = std::move(detail);
    InvalidateRect(window_, &statusRect_, FALSE);
}

void MainWindow::ApplyPort() {
    const int port = _wtoi(WindowText(port_).c_str());
    if (!IsValidPort(port)) {
        SetWindowTextW(port_, std::to_wstring(settings_.port).c_str());
        MessageBeep(MB_ICONWARNING);
        return;
    }
    if (port == settings_.port) return;
    settings_.port = port;
    settings_.Save();
    StartServer();
}

// --- Closing and the background -----------------------------------------------

void MainWindow::OnCloseRequested() {
    // Nothing to share while the server is stopped: just quit.
    if (state_ != ServerState::Running) {
        DestroyWindow(window_);
        return;
    }
    CloseAction action = settings_.closeAction;
    int minutes = settings_.backgroundMinutes;
    const bool asked = action == CloseAction::Ask;
    if (asked && !AskHowToClose(action, minutes)) return;
    if (action == CloseAction::KeepRunning) {
        EnterBackground(minutes);
        return;
    }
    // The question already warned about a running transfer.
    if (!asked && server_.HasActiveTransfers() &&
        MessageBoxW(window_, L"A transfer is still in progress. Close Pons and stop it?", L"Pons",
                    MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) {
        return;
    }
    DestroyWindow(window_);
}

bool MainWindow::AskHowToClose(CloseAction& action, int& minutes) {
    std::wstring content =
        L"Your devices can still open the link to download what you shared or send you files. "
        L"Pons waits in the notification area and quits when the time is up.";
    if (server_.HasActiveTransfers()) content += L"\n\nA transfer is in progress. Closing Pons now stops it.";

    // Our own Cancel: the common one follows the Windows language, and Pons is English only.
    const TASKDIALOG_BUTTON buttons[] = {
        {kKeepRunningButton, L"Keep running"}, {kQuitButton, L"Close Pons"}, {IDCANCEL, L"Cancel"}};
    std::wstring labels[std::size(kBackgroundChoices)];
    TASKDIALOG_BUTTON radios[std::size(kBackgroundChoices)];
    int defaultRadio = kBackgroundRadio + 2;  // 1 hour
    for (size_t i = 0; i < std::size(kBackgroundChoices); ++i) {
        labels[i] = L"For " + FormatDuration(kBackgroundChoices[i]);
        radios[i] = {kBackgroundRadio + static_cast<int>(i), labels[i].c_str()};
        if (kBackgroundChoices[i] == minutes) defaultRadio = radios[i].nButtonID;
    }

    TASKDIALOGCONFIG config{sizeof(config)};
    config.hwndParent = window_;
    config.hInstance = instance_;
    config.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
    config.pszWindowTitle = L"Pons";
    config.pszMainInstruction = L"Keep sharing after you close Pons?";
    config.pszContent = content.c_str();
    config.cButtons = static_cast<UINT>(std::size(buttons));
    config.pButtons = buttons;
    config.nDefaultButton = kKeepRunningButton;
    config.cRadioButtons = static_cast<UINT>(std::size(radios));
    config.pRadioButtons = radios;
    config.nDefaultRadioButton = defaultRadio;
    config.pszVerificationText = L"Remember my choice";
    config.pszFooter = L"You can change this later under “When closed”.";

    int button = 0;
    int radio = 0;
    BOOL remember = FALSE;
    if (FAILED(TaskDialogIndirect(&config, &button, &radio, &remember))) {
        action = CloseAction::Quit;
        return true;
    }
    if (button != kKeepRunningButton && button != kQuitButton) return false;  // Cancelled.

    action = button == kKeepRunningButton ? CloseAction::KeepRunning : CloseAction::Quit;
    const int choice = radio - kBackgroundRadio;
    if (choice >= 0 && choice < static_cast<int>(std::size(kBackgroundChoices))) {
        minutes = kBackgroundChoices[choice];
    }
    // The duration is offered first next time, remembered or not.
    if (action == CloseAction::KeepRunning) settings_.backgroundMinutes = minutes;
    if (remember) settings_.closeAction = action;
    settings_.Save();
    SelectCloseChoice();
    return true;
}

void MainWindow::EnterBackground(int minutes) {
    inBackground_ = true;
    backgroundUntil_ = GetTickCount64() + static_cast<ULONGLONG>(minutes) * 60'000;
    const std::wstring until = ClockTimeAfter(minutes);
    ShowWindow(window_, SW_HIDE);
    AddTrayIcon();
    swprintf_s(trayIcon_.szTip, L"Pons — sharing until %s", until.c_str());
    trayIcon_.uFlags = NIF_TIP | NIF_SHOWTIP;
    vitool_tray::NotifyIcon(NIM_MODIFY, &trayIcon_);

    NOTIFYICONDATAW balloon = trayIcon_;
    balloon.uFlags = NIF_INFO;
    balloon.dwInfoFlags = NIIF_INFO | NIIF_RESPECT_QUIET_TIME;
    wcscpy_s(balloon.szInfoTitle, L"Pons is still sharing");
    swprintf_s(balloon.szInfo, L"Until %s. Click the Pons icon to open it again.", until.c_str());
    vitool_tray::NotifyIcon(NIM_MODIFY, &balloon);
}

void MainWindow::LeaveBackground() {
    if (!inBackground_) return;
    inBackground_ = false;
    RemoveTrayIcon();
    // The idle timeout starts over now that the window is back.
    idleSince_ = GetTickCount64();
    ShowWindow(window_, SW_SHOW);
    if (IsIconic(window_)) ShowWindow(window_, SW_RESTORE);
    SetForegroundWindow(window_);
}

void MainWindow::AddTrayIcon() {
    const std::wstring tip = trayIcon_.szTip;
    if (trayIcon_.hIcon) DestroyIcon(trayIcon_.hIcon);
    trayIcon_ = {sizeof(trayIcon_)};
    trayIcon_.hWnd = window_;
    trayIcon_.uID = 1;
    trayIcon_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    trayIcon_.uCallbackMessage = WM_APP_TRAY;
    LoadIconMetric(instance_, MAKEINTRESOURCEW(IDI_PONS), LIM_SMALL, &trayIcon_.hIcon);
    wcscpy_s(trayIcon_.szTip, tip.empty() ? L"Pons — WiFi Transfer" : tip.c_str());
    vitool_tray::NotifyIcon(NIM_ADD, &trayIcon_);
    trayIcon_.uVersion = NOTIFYICON_VERSION_4;
    vitool_tray::NotifyIcon(NIM_SETVERSION, &trayIcon_);
    trayIconAdded_ = true;
}

void MainWindow::RemoveTrayIcon() {
    if (!trayIconAdded_) return;
    vitool_tray::NotifyIcon(NIM_DELETE, &trayIcon_);
    trayIconAdded_ = false;
    if (trayIcon_.hIcon) DestroyIcon(trayIcon_.hIcon);
    trayIcon_ = {};
}

void MainWindow::ShowTrayMenu(POINT point) {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, kMenuOpen, L"Open Pons");
    AppendMenuW(menu, MF_STRING | (state_ == ServerState::Running ? 0 : MF_GRAYED), kMenuCopyLink, L"Copy link");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuQuit, L"Quit Pons");
    SetMenuDefaultItem(menu, kMenuOpen, FALSE);
    // Required so the menu closes when clicking elsewhere.
    SetForegroundWindow(window_);
    const UINT command = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, point.x, point.y, window_, nullptr);
    DestroyMenu(menu);
    switch (command) {
        case kMenuOpen:
            LeaveBackground();
            break;
        case kMenuCopyLink:
            if (state_ == ServerState::Running) CopyToClipboard(window_, server_.Url());
            break;
        case kMenuQuit:
            if (server_.HasActiveTransfers() &&
                MessageBoxW(nullptr, L"A transfer is still in progress. Quit Pons and stop it?", L"Pons",
                            MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2 | MB_SETFOREGROUND) != IDYES) {
                break;
            }
            DestroyWindow(window_);
            break;
    }
}

// --- Files --------------------------------------------------------------------

void MainWindow::AddFiles(const std::vector<std::filesystem::path>& paths) {
    if (paths.empty()) return;
    std::vector<std::filesystem::path> files;
    for (const auto& path : paths) {
        std::error_code error;
        if (std::filesystem::is_directory(path, error)) {
            // A dropped folder shares the files directly inside it.
            for (const auto& entry : std::filesystem::directory_iterator(path, error)) {
                if (entry.is_regular_file(error)) files.push_back(entry.path());
            }
        } else {
            files.push_back(path);
        }
    }
    server_.AddFiles(files);
    RefreshFileList();
}

void MainWindow::ChooseFiles() { AddFiles(PickFiles(window_, false)); }

void MainWindow::RemoveSelectedFiles() {
    for (int index = -1; (index = ListView_GetNextItem(files_, index, LVNI_SELECTED)) >= 0;) {
        LVITEMW item{LVIF_PARAM};
        item.iItem = index;
        ListView_GetItem(files_, &item);
        server_.RemoveFile(static_cast<uint32_t>(item.lParam));
    }
    RefreshFileList();
}

void MainWindow::RefreshFileList() {
    SendMessageW(files_, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(files_);
    int row = 0;
    for (const auto& file : server_.Files()) {
        LVITEMW item{LVIF_TEXT | LVIF_PARAM};
        item.iItem = row;
        item.pszText = const_cast<wchar_t*>(file.name.c_str());
        item.lParam = static_cast<LPARAM>(file.id);
        ListView_InsertItem(files_, &item);
        std::wstring size = FormatSize(file.size);
        ListView_SetItemText(files_, row, 1, size.data());
        ++row;
    }
    SendMessageW(files_, WM_SETREDRAW, TRUE, 0);
    EnableWindow(GetDlgItem(window_, kRemoveFiles), FALSE);
}

std::filesystem::path MainWindow::ReceiveFolder() const {
    return settings_.receiveFolder.empty() ? DownloadsDirectory() : settings_.receiveFolder;
}

void MainWindow::ChooseFolder() {
    const auto folders = PickFiles(window_, true);
    if (folders.empty()) return;
    settings_.receiveFolder = folders.front();
    settings_.Save();
    server_.SetReceiveFolder(folders.front());
    InvalidateRect(window_, &folderLabel_, FALSE);
}

void MainWindow::ShareText() {
    server_.SetText(ToUnixLineBreaks(WindowText(text_)));
}

// --- Transfers ----------------------------------------------------------------

void MainWindow::OnTransfersChanged() {
    transferRows_ = server_.Transfers();
    std::reverse(transferRows_.begin(), transferRows_.end());
    InvalidateRect(transfers_, nullptr, FALSE);
}

void MainWindow::OpenTransfer(int row) {
    if (row < 0 || row >= static_cast<int>(transferRows_.size())) return;
    const Transfer& transfer = transferRows_[static_cast<size_t>(row)];
    if (transfer.state != TransferState::Done || transfer.path.empty()) return;
    // Show the received file selected in its folder.
    if (PIDLIST_ABSOLUTE item = ILCreateFromPathW(transfer.path.c_str())) {
        SHOpenFolderAndSelectItems(item, 0, nullptr, 0);
        ILFree(item);
    }
}

LRESULT CALLBACK MainWindow::TransfersProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (!self) return DefWindowProcW(window, message, wParam, lParam);
    switch (message) {
        case WM_PAINT: {
            PAINTSTRUCT paint;
            HDC dc = BeginPaint(window, &paint);
            RECT client;
            GetClientRect(window, &client);
            HDC memory = CreateCompatibleDC(dc);
            HBITMAP bitmap = CreateCompatibleBitmap(dc, client.right, client.bottom);
            HGDIOBJ previous = SelectObject(memory, bitmap);
            self->PaintTransfers(window, memory);
            BitBlt(dc, 0, 0, client.right, client.bottom, memory, 0, 0, SRCCOPY);
            SelectObject(memory, previous);
            DeleteObject(bitmap);
            DeleteDC(memory);
            EndPaint(window, &paint);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_LBUTTONUP:
            self->OpenTransfer(GET_Y_LPARAM(lParam) / self->Scale(46));
            return 0;
        case WM_SETCURSOR: {
            POINT point;
            GetCursorPos(&point);
            ScreenToClient(window, &point);
            const int row = point.y / self->Scale(46);
            const bool link = row >= 0 && row < static_cast<int>(self->transferRows_.size()) &&
                              self->transferRows_[static_cast<size_t>(row)].state == TransferState::Done &&
                              !self->transferRows_[static_cast<size_t>(row)].path.empty();
            SetCursor(LoadCursorW(nullptr, link ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

void MainWindow::PaintTransfers(HWND window, HDC dc) {
    RECT client;
    GetClientRect(window, &client);
    FillRect(dc, &client, background_);
    if (transferRows_.empty()) {
        DrawTextIn(dc, textFont_, theme_.secondaryText, L"Nothing transferred yet", client,
                   DT_LEFT | DT_TOP | DT_SINGLELINE);
        return;
    }
    const int rowHeight = Scale(46);
    int y = 0;
    for (const Transfer& transfer : transferRows_) {
        if (y + rowHeight > client.bottom + rowHeight / 2) break;
        RECT row{0, y, client.right, y + rowHeight};
        FillRounded(dc, {row.left, row.top, row.right, row.bottom - Scale(4)}, theme_.pressed, Scale(6));

        const bool fromDevice = transfer.direction == Direction::FromDevice;
        // Arrow down: into the PC; arrow up: out to the device.
        DrawTextIn(dc, iconFont_, fromDevice ? RGB(40, 170, 90) : theme_.accent, fromDevice ? L"" : L"",
                   {Scale(10), y, Scale(30), y + rowHeight - Scale(4)}, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        const int percent = transfer.total ? static_cast<int>(transfer.done * 100 / transfer.total) : 100;
        std::wstring meta;
        COLORREF metaColor = theme_.secondaryText;
        switch (transfer.state) {
            case TransferState::Active:
                meta = std::to_wstring(percent) + L"% · " + FormatSize(transfer.done) + L" of " + FormatSize(transfer.total);
                break;
            case TransferState::Done:
                meta = (fromDevice ? L"Saved · " : L"Sent · ") + FormatSize(transfer.total);
                break;
            case TransferState::Failed:
                meta = L"Interrupted";
                metaColor = RGB(200, 70, 60);
                break;
        }
        const int textLeft = Scale(38);
        const int metaWidth = Scale(170);
        DrawTextIn(dc, textFont_, theme_.text, transfer.name,
                   {textLeft, y + Scale(6), client.right - metaWidth - Scale(12), y + Scale(26)},
                   DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
        DrawTextIn(dc, smallFont_, metaColor, meta, {client.right - metaWidth - Scale(10), y + Scale(6), client.right - Scale(10), y + Scale(26)},
                   DT_RIGHT | DT_SINGLELINE);

        // Progress bar under the name.
        RECT track{textLeft, y + Scale(30), client.right - Scale(10), y + Scale(34)};
        FillRounded(dc, track, theme_.divider, Scale(2));
        if (transfer.state != TransferState::Failed && percent > 0) {
            RECT fill = track;
            fill.right = fill.left + (track.right - track.left) * percent / 100;
            FillRounded(dc, fill, fromDevice ? RGB(40, 170, 90) : theme_.accent, Scale(2));
        }
        y += rowHeight;
    }
}

}  // namespace pons
