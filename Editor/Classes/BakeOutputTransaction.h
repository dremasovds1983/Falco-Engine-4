#pragma once
#include <chrono>
#include <filesystem>
#include <stdexcept>
#include <string>

namespace GX
{
    // Only this bake's unique staging directory is removed on cancellation.
    // Keep the old directory until every new JPEG has been written successfully.
    class BakeOutputTransaction
    {
        std::filesystem::path destination, staging, backup;
    public:
        explicit BakeOutputTransaction(const std::filesystem::path& target) : destination(target.lexically_normal())
        {
            if (destination.filename().empty()) destination = destination.parent_path();
            if (destination.filename() != "Lightmaps") throw std::runtime_error("Invalid lightmap output directory");
            auto suffix = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
            staging = destination.parent_path() / ("Lightmaps.baking-" + suffix);
            backup = destination.parent_path() / ("Lightmaps.previous-" + suffix);
            if (std::filesystem::exists(backup) || !std::filesystem::create_directories(staging))
                throw std::runtime_error("Cannot create unique lightmap staging directory");
        }
        ~BakeOutputTransaction() { std::error_code error; std::filesystem::remove_all(staging, error); }
        BakeOutputTransaction(const BakeOutputTransaction&) = delete;
        BakeOutputTransaction& operator=(const BakeOutputTransaction&) = delete;
        const std::filesystem::path& directory() const { return staging; }
        void commit()
        {
            const bool hadPrevious = std::filesystem::exists(destination);
            if (hadPrevious) std::filesystem::rename(destination, backup);
            try { std::filesystem::rename(staging, destination); }
            catch (...)
            {
                if (hadPrevious)
                {
                    std::error_code error;
                    std::filesystem::rename(backup, destination, error);
                    if (error) throw std::runtime_error("Cannot restore previous lightmaps; preserved at " + backup.u8string());
                }
                throw;
            }
            // A locked old file may leave a backup for manual cleanup; never
            // turn a successful publication into a reported bake failure.
            std::error_code error;
            if (hadPrevious) std::filesystem::remove_all(backup, error);
        }
    };
}
