#pragma once

// Player uses the historical <dear-imgui/imgui.h> spelling while Engine D2
// still compiles the legacy ImGui compatibility island. Do not expose the
// entire legacy bgfx/3rdparty directory to the modern configuration.
#include "../../ThirdParty/dear-imgui-1.85-bgfx/imgui.h"
