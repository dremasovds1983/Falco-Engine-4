#pragma once
#include "ComponentEditor.h"

namespace GX
{
    class CloudSystem;
    class CloudSystemEditor : public ComponentEditor
    {
    public:
        CloudSystemEditor() { setEditorName("CloudSystemEditor"); }
        void init(std::vector<Component*> comps) override;
    private:
        void setParameter(float CloudSystem::* member, float value, const char* label);
    };
}
