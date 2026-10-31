#include "app/main_window.h"

#include <commctrl.h>
#include <objbase.h>
#include <shellapi.h>

#include <filesystem>
#include <string>
#include <vector>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    // Files passed on the command line (e.g. dropped on the shortcut) are shared.
    std::vector<std::filesystem::path> files;
    int count = 0;
    if (wchar_t** arguments = CommandLineToArgvW(GetCommandLineW(), &count)) {
        for (int i = 1; i < count; ++i) files.emplace_back(arguments[i]);
        LocalFree(arguments);
    }

    // One instance per user session. A second launch brings the running one
    // forward and hands it its files.
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\PonsWiFiTransfer.SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND running = FindWindowW(pons::kMainWindowClass, nullptr)) {
            std::wstring list;
            for (const auto& file : files) list += file.wstring() + L"\n";
            COPYDATASTRUCT data{1, static_cast<DWORD>(list.size() * sizeof(wchar_t)), list.data()};
            AllowSetForegroundWindow(ASFW_ANY);
            SendMessageW(running, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&data));
        }
        return 0;
    }

    // The file and folder pickers are COM.
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))) return 1;
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);
    pons::EnableDarkMenus();

    int exitCode = 0;
    {
        pons::MainWindow window;
        if (window.Create(instance, show, files)) {
            MSG message;
            while (GetMessageW(&message, nullptr, 0, 0) > 0) {
                if (window.PreTranslate(message)) continue;
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        } else {
            exitCode = 1;
        }
    }
    CoUninitialize();
    ReleaseMutex(mutex);
    CloseHandle(mutex);
    return exitCode;
}
