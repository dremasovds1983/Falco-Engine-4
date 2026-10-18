#pragma once
#include <string>
namespace GX
{
    // Draft-only prefab editing. No GameObjects are added to the running scene.
    class PrefabEditorWindow
    {
    public:
        static void open(const std::string& path);
        static void update();
        static bool ownsFocus();
        static bool save();
        static void undo();
        static void redo();
        static bool confirmClose();
        static void invalidatePreview();
        static void shutdown();
    };
}
