#include "StartupScreen.h"

#include <SDL2/SDL_video.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "../resource.h"
#include <algorithm>
#include <atomic>
#include <future>
#include <thread>
#include <vector>
#endif

namespace GX
{
    struct StartupScreen::Impl
    {
        bool maximize = false;
        bool active = false; // Engine thread only, published by the startup future.
#ifdef _WIN32
        std::thread thread;
        std::atomic<bool> stopping{false};
        std::atomic<HWND> window{nullptr};
        std::wstring logPath;
        std::vector<std::wstring> lines;
        HICON logo = nullptr;
        HFONT titleFont = nullptr;
        HFONT logFont = nullptr;
        ULONGLONG started = GetTickCount64();
        WIN32_FILE_ATTRIBUTE_DATA originalLog{};
        bool hadOriginalLog = false;
        bool currentLog = false;

        static std::wstring wide(const std::string& text)
        {
            UINT page = CP_UTF8;
            int length = MultiByteToWideChar(page, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
            if (!length)
            {
                page = CP_ACP;
                length = MultiByteToWideChar(page, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
            }
            std::wstring result(length, L' ');
            if (length)
                MultiByteToWideChar(page, 0, text.data(), static_cast<int>(text.size()), result.data(), length);
            return result;
        }

        void readLog()
        {
            // The logger closes each append. Sharing all access avoids blocking it.
            // Ignore the preceding session until Window::init resets the log.
            WIN32_FILE_ATTRIBUTE_DATA now{};
            if (!GetFileAttributesExW(logPath.c_str(), GetFileExInfoStandard, &now))
                return;
            if (!currentLog)
            {
                currentLog = !hadOriginalLog || now.nFileSizeLow != originalLog.nFileSizeLow ||
                    now.nFileSizeHigh != originalLog.nFileSizeHigh ||
                    CompareFileTime(&now.ftLastWriteTime, &originalLog.ftLastWriteTime) != 0;
                if (!currentLog)
                    return;
            }
            HANDLE file = CreateFileW(logPath.c_str(), GENERIC_READ,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE)
                return;
            LARGE_INTEGER size{};
            char bytes[16384];
            DWORD count = 0;
            bool read = false;
            LONGLONG offset = 0;
            if (GetFileSizeEx(file, &size))
            {
                offset = std::max<LONGLONG>(0, size.QuadPart - static_cast<LONGLONG>(sizeof(bytes)));
                LARGE_INTEGER position{};
                position.QuadPart = offset;
                if (SetFilePointerEx(file, position, nullptr, FILE_BEGIN))
                    read = ReadFile(file, bytes, sizeof(bytes), &count, nullptr) != FALSE;
            }
            CloseHandle(file);
            if (!read || !count)
                return;
            std::string text(bytes, count);
            std::vector<std::wstring> recent;
            size_t begin = 0;
            if (offset) // Drop a partial first line / UTF-8 character.
            {
                const auto end = text.find('\n');
                if (end == std::string::npos)
                    return;
                begin = end + 1;
            }
            for (auto end = text.find('\n', begin); end != std::string::npos; end = text.find('\n', begin))
            {
                auto line = text.substr(begin, end - begin);
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                if (!line.empty() && line.find_first_not_of("- \t") != std::string::npos)
                    recent.push_back(wide(line));
                begin = end + 1;
            }
            if (recent.size() > 5)
                recent.erase(recent.begin(), recent.end() - 5);
            if (!recent.empty())
                lines = std::move(recent);
        }

        void paint(HWND hwnd)
        {
            PAINTSTRUCT ps{};
            HDC target = BeginPaint(hwnd, &ps);
            RECT rect{};
            GetClientRect(hwnd, &rect);
            HDC dc = CreateCompatibleDC(target);
            HBITMAP bitmap = CreateCompatibleBitmap(target, rect.right, rect.bottom);
            if (!dc || !bitmap)
            {
                if (bitmap) DeleteObject(bitmap);
                if (dc) DeleteDC(dc);
                EndPaint(hwnd, &ps);
                return;
            }
            auto oldBitmap = SelectObject(dc, bitmap);
            HBRUSH background = CreateSolidBrush(RGB(15, 23, 35));
            FillRect(dc, &rect, background);
            DeleteObject(background);
            SetBkMode(dc, TRANSPARENT);
            if (logo)
                DrawIconEx(dc, (rect.right - 256) / 2, 24, logo, 256, 256, 0, nullptr, DI_NORMAL);
            auto oldFont = SelectObject(dc, titleFont ? titleFont : GetStockObject(DEFAULT_GUI_FONT));
            SetTextColor(dc, RGB(228, 235, 244));
            RECT title{24, 285, rect.right - 24, 330};
            DrawTextW(dc, L"Falco Engine", -1, &title, DT_CENTER | DT_SINGLELINE | DT_NOPREFIX);
            SelectObject(dc, logFont ? logFont : GetStockObject(DEFAULT_GUI_FONT));
            SetTextColor(dc, RGB(235, 177, 65));
            RECT version{24, 330, rect.right - 24, 352};
            DrawTextW(dc, L"4.0.0.0", -1, &version, DT_CENTER | DT_SINGLELINE);
            const auto elapsed = (GetTickCount64() - started) / 1000;
            std::wstring status = L"Loading" + std::wstring(1 + (GetTickCount64() / 400) % 3, L'.') +
                L"   " + std::to_wstring(elapsed) + L" s";
            RECT loading{24, 366, rect.right - 24, 390};
            DrawTextW(dc, status.c_str(), -1, &loading, DT_RIGHT | DT_SINGLELINE | DT_NOPREFIX);
            int y = rect.bottom - 28 - static_cast<int>(lines.size()) * 23;
            for (size_t i = 0; i < lines.size(); ++i, y += 23)
            {
                SetTextColor(dc, i + 1 == lines.size() ? RGB(228, 235, 244) : RGB(143, 159, 180));
                RECT line{28, y, rect.right - 28, y + 23};
                DrawTextW(dc, lines[i].c_str(), -1, &line, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            }
            SelectObject(dc, oldFont);
            BitBlt(target, 0, 0, rect.right, rect.bottom, dc, 0, 0, SRCCOPY);
            SelectObject(dc, oldBitmap);
            DeleteObject(bitmap);
            DeleteDC(dc);
            EndPaint(hwnd, &ps);
        }

        static LRESULT CALLBACK procedure(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
        {
            auto self = reinterpret_cast<Impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
            if (message == WM_NCCREATE)
            {
                self = static_cast<Impl*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
                SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            }
            if (self)
            {
                switch (message)
                {
                case WM_ERASEBKGND: return 1;
                case WM_PAINT: self->paint(hwnd); return 0;
                case WM_CLOSE: return 0; // Initialization is synchronous and cannot safely be cancelled here.
                case WM_TIMER:
                    if (self->stopping.load())
                        DestroyWindow(hwnd);
                    else
                    {
                        self->readLog();
                        InvalidateRect(hwnd, nullptr, FALSE);
                    }
                    return 0;
                case WM_APP:
                    DestroyWindow(hwnd);
                    return 0;
                case WM_DESTROY:
                    KillTimer(hwnd, 1);
                    self->window.store(nullptr);
                    PostQuitMessage(0);
                    return 0;
                }
            }
            return DefWindowProcW(hwnd, message, wparam, lparam);
        }

        void run(std::promise<bool> ready)
        {
            HINSTANCE instance = GetModuleHandleW(nullptr);
            constexpr auto className = L"Falco.StartupScreen";
            WNDCLASSEXW wc{};
            wc.cbSize = sizeof(wc);
            wc.style = CS_NOCLOSE;
            wc.lpfnWndProc = procedure;
            wc.hInstance = instance;
            wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_ICON1));
            wc.hIconSm = wc.hIcon;
            wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
            wc.lpszClassName = className;
            if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            {
                ready.set_value(false);
                return;
            }
            logo = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(IDI_ICON1), IMAGE_ICON, 256, 256, 0));
            titleFont = CreateFontW(-36, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
            logFont = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
            POINT cursor{};
            GetCursorPos(&cursor);
            MONITORINFO monitor{};
            monitor.cbSize = sizeof(monitor);
            GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY), &monitor);
            const RECT& area = monitor.rcWork;
            HWND hwnd = CreateWindowExW(WS_EX_APPWINDOW, className, L"Falco Engine - Loading", WS_POPUP | WS_BORDER,
                area.left + (area.right - area.left - 760) / 2, area.top + (area.bottom - area.top - 550) / 2,
                760, 550, nullptr, nullptr, instance, this);
            window.store(hwnd);
            bool usable = hwnd && SetTimer(hwnd, 1, 100, nullptr);
            if (usable)
            {
                ShowWindow(hwnd, SW_SHOWNORMAL);
                UpdateWindow(hwnd);
            }
            ready.set_value(usable);
            if (usable)
            {
                MSG message{};
                while (GetMessageW(&message, nullptr, 0, 0) > 0)
                {
                    TranslateMessage(&message);
                    DispatchMessageW(&message);
                }
            }
            if (window.load()) DestroyWindow(hwnd);
            if (logo) DestroyIcon(logo);
            if (titleFont) DeleteObject(titleFont);
            if (logFont) DeleteObject(logFont);
            UnregisterClassW(className, instance);
        }

        explicit Impl(const std::wstring& initialStatus) : lines{initialStatus}
        {
            wchar_t path[32768]{};
            const DWORD length = GetModuleFileNameW(nullptr, path, 32768);
            logPath.assign(path, length);
            logPath.resize(logPath.find_last_of(L"\\/") + 1);
            logPath += L"editor.log";
            hadOriginalLog = GetFileAttributesExW(logPath.c_str(), GetFileExInfoStandard, &originalLog) != FALSE;
            std::promise<bool> ready;
            auto result = ready.get_future();
            try
            {
                thread = std::thread([this, ready = std::move(ready)]() mutable { run(std::move(ready)); });
                active = result.get();
            }
            catch (const std::system_error&)
            {
                // If a native UI thread cannot start, preserve the ordinary visible SDL window.
                active = false;
            }
        }

        void stop()
        {
            stopping.store(true);
            if (auto hwnd = window.load()) PostMessageW(hwnd, WM_APP, 0, 0);
            if (thread.joinable())
            {
                // Destroying a native window can synchronously message the SDL
                // thread. Dispatch sent messages while joining; leave queued
                // SDL input and WM_QUIT for its normal event loop.
                HANDLE handle = thread.native_handle();
                while (MsgWaitForMultipleObjects(1, &handle, FALSE, INFINITE, QS_SENDMESSAGE) == WAIT_OBJECT_0 + 1)
                {
                    MSG message{};
                    PeekMessageW(&message, nullptr, 0, 0, PM_NOREMOVE | PM_QS_SENDMESSAGE);
                }
                thread.join();
            }
            active = false;
        }
#else
        explicit Impl(const std::wstring&) {}
        void stop() {}
#endif
        ~Impl() { stop(); }
    };

    StartupScreen::StartupScreen(const std::wstring& initialStatus) : impl(std::make_unique<Impl>(initialStatus)) {}
    StartupScreen::~StartupScreen() = default;

    uint32_t StartupScreen::windowFlags(uint32_t flags)
    {
        // SDL 2.0.12 maximizes with ShowWindow even when HIDDEN is requested.
        impl->maximize = (flags & SDL_WINDOW_MAXIMIZED) != 0;
        return impl->active ? (flags & ~(SDL_WINDOW_SHOWN | SDL_WINDOW_MAXIMIZED)) | SDL_WINDOW_HIDDEN : flags;
    }

    void StartupScreen::reveal(void* sdlWindow)
    {
        if (!impl->active)
            return;
        if (impl->maximize)
            SDL_MaximizeWindow(static_cast<SDL_Window*>(sdlWindow));
        SDL_ShowWindow(static_cast<SDL_Window*>(sdlWindow));
        SDL_RaiseWindow(static_cast<SDL_Window*>(sdlWindow));
        impl->stop();
    }
}
