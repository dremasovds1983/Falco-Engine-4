#pragma once

// Falco's generated ImGui shader headers predate bgfx's DXBC suffix and do
// not contain DXIL or WGSL variants. The modern configuration targets D3D11,
// so expose the existing D3D11 bytecode as DXBC and omit unsupported blobs.
#define BGFX_PLATFORM_SUPPORTS_DXIL 0
#define BGFX_PLATFORM_SUPPORTS_WGSL 0

#define vs_ocornut_imgui_dxbc vs_ocornut_imgui_dx11
#define fs_ocornut_imgui_dxbc fs_ocornut_imgui_dx11
