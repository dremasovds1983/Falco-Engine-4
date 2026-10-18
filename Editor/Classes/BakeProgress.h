#pragma once
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>

namespace GX
{
    struct BakeCancelled : std::runtime_error { BakeCancelled() : std::runtime_error("Lighting bake cancelled") {} };

    // Engine-thread writer; an independent native UI thread reads snapshots only.
    class BakeProgress
    {
    public:
        explicit BakeProgress(void* owner);
        ~BakeProgress();
        BakeProgress(const BakeProgress&) = delete;
        BakeProgress& operator=(const BakeProgress&) = delete;
        void stage(const std::string& name, const std::string& detail = {}, uint64_t total = 0, const std::string& unit = {});
        void advance(uint64_t count = 1);
        void sample();
        void ray();
        void allocation(int size);
        void checkpoint(bool force = false);
        void beginCommit();
        void note(const std::string& text);
        void finish(const std::string& result);
        std::string reportPath() const;
    private:
        struct Impl;
        std::unique_ptr<Impl> impl;
    };
}
