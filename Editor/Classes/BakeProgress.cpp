#include "BakeProgress.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <iomanip>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>
#include <SDL2/SDL_events.h>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commctrl.h>
#include <psapi.h>
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "psapi.lib")
#endif

namespace GX
{
    struct BakeProgress::Impl
    {
        using Clock = std::chrono::steady_clock;
        struct Snapshot
        {
            std::string stage = "Preparing", detail, unit;
            uint64_t done = 0, total = 0, samples = 0, rays = 0, maps = 0, bytes = 0;
            Clock::time_point lastWork = Clock::now();
        } work, published;
        Clock::time_point started = Clock::now(), stageStart = started, lastPublish = started, lastLog = started;
        std::mutex mutex;
        std::atomic<bool> cancelled{false}, committing{false}, stopping{false};
        std::map<std::string, double> times;
        std::ofstream report;
        std::filesystem::path path;
        bool finished = false;
#ifdef _WIN32
        HWND owner = nullptr, label = nullptr, bar = nullptr, cancel = nullptr;
        bool ownerWasEnabled = false;
        std::atomic<HWND> window{nullptr};
        std::thread ui;
#endif
        static double seconds(Clock::duration d) { return std::chrono::duration<double>(d).count(); }
        void account()
        {
            auto now = Clock::now();
            times[work.stage] += seconds(now - stageStart);
            stageStart = now;
        }
        void publish(bool force)
        {
            auto now = Clock::now();
            if (!force && now - lastPublish < std::chrono::milliseconds(100)) return;
            { std::lock_guard<std::mutex> lock(mutex); published = work; }
            lastPublish = now;
            // Let SDL service its native window messages, but do not run editor
            // callbacks or mutate scene state reentrantly during the bake.
            SDL_PumpEvents();
            if (report && (force || now - lastLog >= std::chrono::seconds(1)))
            {
                lastLog = now;
                report << "checkpoint\t" << seconds(now - started) << '\t' << work.stage << '\t'
                       << work.done << '/' << work.total << '\t' << work.unit
                       << "\tsamples=" << work.samples << "\trays=" << work.rays << '\n';
                report.flush();
            }
        }
#ifdef _WIN32
        static std::wstring wide(const std::string& value)
        {
            int n = MultiByteToWideChar(CP_UTF8, 0, value.data(), (int)value.size(), nullptr, 0);
            std::wstring result(n, L' ');
            if (n) MultiByteToWideChar(CP_UTF8, 0, value.data(), (int)value.size(), result.data(), n);
            return result;
        }
        void draw()
        {
            Snapshot s;
            { std::lock_guard<std::mutex> lock(mutex); s = published; }
            auto now = Clock::now();
            PROCESS_MEMORY_COUNTERS_EX memory{};
            memory.cb = sizeof(memory);
            const bool hasMemory = GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&memory, sizeof(memory)) != FALSE;
            std::ostringstream text;
            text << s.stage << "\r\n" << s.detail << "\r\n\r\n";
            if (s.total) text << s.done << " / " << s.total << ' ' << s.unit << " (current operation)\r\n";
            else text << "Work total unknown for this operation\r\n";
            text << "Shaded texels: " << s.samples << "    Ray queries: " << s.rays
                 << "\r\nLightmaps: " << s.maps << "    Map buffers: " << s.bytes / (1024 * 1024) << " MiB";
            if (hasMemory) text << "\r\nProcess private memory: " << memory.PrivateUsage / (1024 * 1024) << " MiB";
            text << std::fixed << std::setprecision(1) << "\r\nElapsed: " << seconds(now - started)
                 << " s    Last work checkpoint: " << seconds(now - s.lastWork) << " s ago";
            if (now - s.lastWork > std::chrono::seconds(5))
                text << "\r\nNo new work checkpoint: operation may be busy or stalled.";
            if (cancelled && !committing) text << "\r\nCancellation requested; waiting for a safe checkpoint...";
            if (committing) text << "\r\nPublishing results; cancellation is disabled.";
            SetWindowTextW(label, wide(text.str()).c_str());
            LONG_PTR style = GetWindowLongPtrW(bar, GWL_STYLE);
            SetWindowLongPtrW(bar, GWL_STYLE, s.total ? (style & ~PBS_MARQUEE) : (style | PBS_MARQUEE));
            SendMessageW(bar, PBM_SETMARQUEE, s.total == 0, 70);
            if (s.total) SendMessageW(bar, PBM_SETPOS, (WPARAM)(1000.0 * std::min(s.done, s.total) / s.total), 0);
            EnableWindow(cancel, !committing && !cancelled);
        }
        static LRESULT CALLBACK procedure(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
        {
            auto* self = (Impl*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
            if (msg == WM_NCCREATE)
            {
                self = (Impl*)((CREATESTRUCTW*)lp)->lpCreateParams;
                SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)self);
            }
            if (self)
            {
                if (msg == WM_TIMER) { self->draw(); if (self->stopping) DestroyWindow(hwnd); return 0; }
                if (msg == WM_CLOSE || (msg == WM_COMMAND && LOWORD(wp) == 1))
                { if (!self->committing) self->cancelled = true; return 0; }
                if (msg == WM_APP + 1) { DestroyWindow(hwnd); return 0; }
                if (msg == WM_DESTROY) { PostQuitMessage(0); return 0; }
            }
            return DefWindowProcW(hwnd, msg, wp, lp);
        }
        void startUI()
        {
            std::promise<bool> ready;
            auto result = ready.get_future();
            ui = std::thread([this, signal = std::move(ready)]() mutable {
                HINSTANCE instance = GetModuleHandleW(nullptr);
                WNDCLASSW wc{};
                wc.lpfnWndProc = procedure; wc.hInstance = instance;
                wc.lpszClassName = L"FalcoBakeProgress";
                wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
                wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
                RegisterClassW(&wc);
                INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_PROGRESS_CLASS};
                InitCommonControlsEx(&controls);
                // Deliberately no cross-thread owner: creation/activation must not
                // wait for the engine thread while it waits for this ready future.
                HWND hwnd = CreateWindowExW(WS_EX_APPWINDOW, wc.lpszClassName, L"Falco Engine - Baking lighting",
                    WS_CAPTION | WS_SYSMENU, CW_USEDEFAULT, CW_USEDEFAULT, 680, 340,
                    nullptr, nullptr, instance, this);
                window = hwnd;
                if (!hwnd) { signal.set_value(false); return; }
                label = CreateWindowExW(0, L"STATIC", L"Preparing...", WS_CHILD | WS_VISIBLE,
                    16, 16, 640, 215, hwnd, nullptr, instance, nullptr);
                bar = CreateWindowExW(0, PROGRESS_CLASSW, nullptr, WS_CHILD | WS_VISIBLE | PBS_MARQUEE,
                    16, 238, 525, 22, hwnd, nullptr, instance, nullptr);
                cancel = CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                    555, 236, 100, 28, hwnd, (HMENU)1, instance, nullptr);
                if (!label || !bar || !cancel) { DestroyWindow(hwnd); signal.set_value(false); return; }
                SendMessageW(label, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
                SendMessageW(cancel, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
                SendMessageW(bar, PBM_SETRANGE32, 0, 1000);
                SetTimer(hwnd, 1, 250, nullptr);
                signal.set_value(true);
                ShowWindow(hwnd, SW_SHOWNORMAL);
                MSG message{};
                while (GetMessageW(&message, nullptr, 0, 0) > 0) { TranslateMessage(&message); DispatchMessageW(&message); }
                window = nullptr;
            });
            if (!result.get()) { stop(); throw std::runtime_error("Cannot create lighting progress window"); }
        }
        void stop()
        {
            stopping = true;
            if (HWND hwnd = window.load()) PostMessageW(hwnd, WM_APP + 1, 0, 0);
            if (ui.joinable())
            {
                HANDLE handle = ui.native_handle();
                while (MsgWaitForMultipleObjects(1, &handle, FALSE, INFINITE, QS_SENDMESSAGE) == WAIT_OBJECT_0 + 1)
                { MSG message{}; PeekMessageW(&message, nullptr, 0, 0, PM_NOREMOVE | PM_QS_SENDMESSAGE); }
                ui.join();
            }
            if (owner && ownerWasEnabled) EnableWindow(owner, TRUE);
        }
#else
        void stop() {}
#endif
        ~Impl() { stop(); }
    };

    BakeProgress::BakeProgress(void* owner) : impl(std::make_unique<Impl>())
    {
#ifdef _WIN32
        wchar_t exe[32768]{};
        GetModuleFileNameW(nullptr, exe, 32768);
        auto folder = std::filesystem::path(exe).parent_path() / "BakeReports";
        std::error_code error;
        std::filesystem::create_directories(folder, error);
        if (!error)
        {
            auto ticks = std::chrono::system_clock::now().time_since_epoch().count();
            impl->path = folder / ("bake-" + std::to_string(ticks) + ".log");
            impl->report.open(impl->path);
            if (!impl->report) impl->path.clear();
        }
        impl->owner = (HWND)owner;
        impl->startUI();
        impl->ownerWasEnabled = owner && IsWindowEnabled((HWND)owner);
        if (impl->ownerWasEnabled) EnableWindow((HWND)owner, FALSE);
#endif
    }
    BakeProgress::~BakeProgress() = default;
    void BakeProgress::stage(const std::string& name, const std::string& detail, uint64_t total, const std::string& unit)
    {
        impl->account();
        impl->work.stage = name; impl->work.detail = detail;
        impl->work.done = 0; impl->work.total = total; impl->work.unit = unit;
        impl->work.lastWork = Impl::Clock::now();
        note("stage\t" + name + "\t" + detail);
        checkpoint(true);
    }
    void BakeProgress::advance(uint64_t count) { impl->work.done += count; impl->work.lastWork = Impl::Clock::now(); checkpoint(); }
    void BakeProgress::sample() { ++impl->work.samples; if ((impl->work.samples & 63) == 0) { impl->work.lastWork = Impl::Clock::now(); checkpoint(); } }
    void BakeProgress::ray() { ++impl->work.rays; impl->work.lastWork = Impl::Clock::now(); checkpoint(); }
    void BakeProgress::allocation(int size)
    {
        ++impl->work.maps; impl->work.bytes += uint64_t(size) * size * 32;
        impl->work.lastWork = Impl::Clock::now();
        note("allocated\t" + std::to_string(size) + "x" + std::to_string(size) + "\tbuffers_bytes=" + std::to_string(impl->work.bytes));
        checkpoint(true);
    }
    void BakeProgress::checkpoint(bool force)
    {
        if (impl->cancelled && !impl->committing) throw BakeCancelled();
        impl->publish(force);
    }
    void BakeProgress::beginCommit() { checkpoint(true); impl->committing = true; }
    void BakeProgress::note(const std::string& text) { if (impl->report) { impl->report << text << '\n'; impl->report.flush(); } }
    void BakeProgress::finish(const std::string& result)
    {
        if (impl->finished) return;
        impl->finished = true;
        impl->account();
        note("result\t" + result);
        note("elapsed_seconds\t" + std::to_string(Impl::seconds(Impl::Clock::now() - impl->started)));
        for (const auto& phase : impl->times) note("phase_seconds\t" + phase.first + "\t" + std::to_string(phase.second));
        note("shaded_texels\t" + std::to_string(impl->work.samples) + "\nray_queries\t" + std::to_string(impl->work.rays));
#ifdef _WIN32
        PROCESS_MEMORY_COUNTERS_EX memory{}; memory.cb = sizeof(memory);
        if (GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&memory, sizeof(memory)))
            note("process_peak_working_set_bytes\t" + std::to_string(memory.PeakWorkingSetSize));
#endif
    }
    std::string BakeProgress::reportPath() const { return impl->path.u8string(); }
}
