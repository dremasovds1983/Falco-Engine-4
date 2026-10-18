// CPU-only regression test; links the existing serializer and MD5 objects.
#include "Editor/Serialization/EditorSettings.h"
#include <cassert>
#include <sstream>
#include <iostream>

// No filesystem/UI setup: exercise the production inline archive schema.
namespace GX { EditorSettings::EditorSettings() = default; EditorSettings::~EditorSettings() = default; }
struct LegacySettings : GX::EditorSettings
{
    int archiveVersion;
    explicit LegacySettings(int v) : archiveVersion(v) {}
    int getVersion() override { return archiveVersion; }
};
static void roundTrip(GX::EditorSettings& source, bool expectedEmpty)
{
    source.lastOpenedScene = "Level/Level.scene";
    source.cameraSpeed = 17.0f;
    source.selectionOutlineThickness = 7.0f;
    std::stringstream bytes(std::ios::in | std::ios::out | std::ios::binary);
    GX::BinarySerializer writer;
    writer.serialize(&bytes, &source, "Editor Settings");
    GX::EditorSettings restored;
    GX::BinarySerializer reader;
    reader.deserialize(&bytes, &restored, "Editor Settings");
    assert(restored.openWithEmptyScene == expectedEmpty);
    assert(restored.lastOpenedScene == source.lastOpenedScene);
    assert(restored.cameraSpeed == source.cameraSpeed);
    if (source.getVersion() >= 3)
        assert(restored.selectionOutlineThickness == source.selectionOutlineThickness);
    assert(bytes.peek() == std::char_traits<char>::eof());
}
int main()
{
    for (int version = 0; version <= 3; ++version)
    {
        LegacySettings old(version);
        old.openWithEmptyScene = false; // Not present in legacy archives.
        roundTrip(old, true);
    }
    for (bool value : {false, true})
    {
        GX::EditorSettings current;
        current.openWithEmptyScene = value;
        roundTrip(current, value);
    }
    std::cout << "PASS: v0-v3 default ON; v4 ON/OFF round-trip; existing fields preserved\n";
}
