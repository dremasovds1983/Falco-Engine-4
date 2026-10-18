#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace GX
{
    // Owns a native UI thread; it never calls SDL, bgfx or engine singletons.
    class StartupScreen
    {
    public:
        explicit StartupScreen(const std::wstring& initialStatus);
        ~StartupScreen();
        StartupScreen(const StartupScreen&) = delete;
        StartupScreen& operator=(const StartupScreen&) = delete;

        uint32_t windowFlags(uint32_t flags);
        // Call on the engine thread after submitting the first usable frames.
        void reveal(void* sdlWindow);

    private:
        struct Impl;
        std::unique_ptr<Impl> impl;
    };
}
