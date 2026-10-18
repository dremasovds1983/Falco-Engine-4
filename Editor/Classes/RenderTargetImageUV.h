#pragma once
#include <bgfx/bgfx.h>
#include <imgui.h>
namespace GX
{
    // Final HDR presentation preserves native RT orientation. ImGui uses top-left UVs.
    inline ImVec2 renderTargetImageUV0()
    {
#ifdef FALCO_MODERN_BGFX
        const auto* caps = bgfx::getCaps();
        return ImVec2(0.0f, caps != nullptr && caps->originBottomLeft ? 1.0f : 0.0f);
#else
        return ImVec2(0.0f, 1.0f);
#endif
    }
    inline ImVec2 renderTargetImageUV1()
    {
        return ImVec2(1.0f, 1.0f - renderTargetImageUV0().y);
    }
}
