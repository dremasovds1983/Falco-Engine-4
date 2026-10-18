// Link with production Engine/serializer objects; no substituted settings methods.
// CPU-only in-memory archives: no user project, renderer or filesystem setup.
#include "Engine/Serialization/Settings/ProjectSettings.h"
#include <sstream>
#include <stdexcept>
#include <iostream>
#include <limits>

static void require(bool ok, const char* message)
{
    if (!ok) throw std::runtime_error(message);
}
struct HistoricalSettings : GX::ProjectSettings
{
    int schema;
    explicit HistoricalSettings(int value) : schema(value) {}
    int getVersion() override { return schema; }
};
int main()
{
    try
    {
        for (int version = 0; version <= 5; ++version)
        {
            HistoricalSettings source(version);
            source.setStartupScene("Scenes/compatibility.scene");
            source.setScreenWidth(1733);
            source.getTags().push_back("compatibility-tag");
            source.setTonemap(2); source.setExposure(2.25f);
            source.setBrightness(0.25f); source.setContrast(2.0f); source.setSaturation(0.5f);
            source.setBloom(true); source.setBloomThreshold(3.0f);
            source.setBloomIntensity(0.5f); source.setBloomRadius(2.0f);
            source.setSSAO(true); source.setSSAORadius(2.0f);
            source.setSSAOIntensity(2.0f); source.setSSAOBias(0.125f);
            source.setSSAOSamples(32); source.setBloomDownsample(4);
            source.setSSR(true); source.setSSRDistance(60.0f); source.setSSRThickness(0.5f);
            std::stringstream bytes(std::ios::in | std::ios::out | std::ios::binary);
            GX::BinarySerializer writer;
            writer.serialize(&bytes, &source, "Project Settings");
            const std::string payload = bytes.str();
            GX::ProjectSettings restored;
            // Dirty destination proves migration resets absent fields, not just constructor defaults.
            restored.setBloom(true); restored.setSSAO(true); restored.setSSR(true);
            restored.setTonemap(0); restored.setSSAOSamples(8);
            GX::BinarySerializer reader;
            reader.deserialize(&bytes, &restored, "Project Settings");
            require(restored.getStartupScene() == source.getStartupScene(), "legacy scene lost");
            require(restored.getScreenWidth() == 1733 && restored.getTags() == source.getTags(), "legacy fields shifted");
            require(restored.getTonemap() == (version >= 1 ? 2 : 1), "tonemap migration");
            require(restored.getExposure() == (version >= 1 ? 2.25f : 1.0f), "exposure round trip");
            require(restored.getBloom() == (version >= 2), "bloom migration");
            require(restored.getBloomThreshold() == (version >= 2 ? 3.0f : 1.0f), "bloom values");
            require(restored.getSSAO() == (version >= 3), "AO migration");
            require(restored.getSSAORadius() == (version >= 3 ? 2.0f : 1.0f), "AO radius");
            require(restored.getSSAOSamples() == (version >= 4 ? 32 : 16), "AO quality migration");
            require(restored.getBloomDownsample() == (version >= 4 ? 4 : 2), "bloom quality migration");
            require(restored.getSSR() == (version >= 5), "SSR migration");
            require(restored.getSSRDistance() == (version >= 5 ? 60.0f : 30.0f), "SSR distance");
            require(bytes.peek() == std::char_traits<char>::eof(), "unconsumed archive bytes");
            bool rejected = false;
            try
            {
                std::istringstream truncated(payload.substr(0, payload.size() - 1), std::ios::binary);
                truncated.exceptions(std::ios::failbit | std::ios::badbit);
                GX::ProjectSettings incomplete;
                GX::BinarySerializer decoder;
                decoder.deserialize(&truncated, &incomplete, "Project Settings");
            }
            catch (const std::exception&) { rejected = true; }
            require(rejected, "truncated settings accepted");
        }
        GX::ProjectSettings sanitized;
        sanitized.setExposure(std::numeric_limits<float>::quiet_NaN());
        sanitized.setSSAORadius(-1.0f); sanitized.setSSRThickness(100.0f);
        sanitized.setTonemap(999); sanitized.setSSAOSamples(15);
        require(sanitized.getExposure() == 1.0f && sanitized.getTonemap() == 1, "invalid presentation values");
        require(sanitized.getSSAORadius() == 0.01f && sanitized.getSSRThickness() == 5.0f, "unsafe sampling ranges");
        require(sanitized.getSSAOSamples() == 16, "invalid AO sample count");
        std::cout << "PASS: graphics settings schemas 0..5, truncation and invalid values\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
