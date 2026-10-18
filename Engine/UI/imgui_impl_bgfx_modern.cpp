/*
 * Copyright 2014-2015 Daniel Collin. All rights reserved.
 * Adapted from bgfx examples/common/imgui/imgui.cpp at
 * 65551a7db19240b4d105f09e665190d196243d92 (BSD-2-Clause).
 * Falco adaptation 2026: ImGui 1.85 font atlas, Engine texture IDs and API,
 * bounded framebuffer scissors, reset callbacks and device lifetime handling.
 * Full upstream license: ThirdParty/bgfx-production-v1/LICENSE.
 */
#include "imgui_impl_bgfx.h"
#include "imgui.h"
#include <bgfx/bgfx.h>
#include <bgfx/embedded_shader.h>
#include <bx/math.h>
#include <cstdint>
#include <cstring>
#include "../../out/modern-bgfx/src/bgfx/examples/common/imgui/fs_ocornut_imgui.bin.h"
#include "../../out/modern-bgfx/src/bgfx/examples/common/imgui/vs_ocornut_imgui.bin.h"

namespace {
bgfx::ViewId viewId = 1023;
bgfx::TextureHandle font = BGFX_INVALID_HANDLE;
bgfx::ProgramHandle program = BGFX_INVALID_HANDLE;
bgfx::UniformHandle sampler = BGFX_INVALID_HANDLE;
bgfx::VertexLayout layout;
const bgfx::EmbeddedShader shaders[] = {
    BGFX_EMBEDDED_SHADER(vs_ocornut_imgui),
    BGFX_EMBEDDED_SHADER(fs_ocornut_imgui), BGFX_EMBEDDED_SHADER_END()
};
void setupView(const ImDrawData& data, uint16_t width, uint16_t height)
{
    float ortho[16];
    bx::mtxOrtho(ortho, data.DisplayPos.x, data.DisplayPos.x + data.DisplaySize.x,
        data.DisplayPos.y + data.DisplaySize.y, data.DisplayPos.y,
        0.0f, 1000.0f, 0.0f, bgfx::getCaps()->homogeneousDepth);
    bgfx::setViewName(viewId, "ImGui");
    bgfx::setViewMode(viewId, bgfx::ViewMode::Sequential);
    bgfx::setViewTransform(viewId, nullptr, ortho);
    bgfx::setViewRect(viewId, 0, 0, width, height);
}
}

void ImGui_Implbgfx_InvalidateDeviceObjects()
{
    if (bgfx::isValid(font)) bgfx::destroy(font);
    if (bgfx::isValid(program)) bgfx::destroy(program);
    if (bgfx::isValid(sampler)) bgfx::destroy(sampler);
    font = BGFX_INVALID_HANDLE;
    program = BGFX_INVALID_HANDLE;
    sampler = BGFX_INVALID_HANDLE;
    ImGui::GetIO().Fonts->SetTexID(nullptr);
}

bool ImGui_Implbgfx_CreateDeviceObjects()
{
    if (bgfx::isValid(program) && bgfx::isValid(font) && bgfx::isValid(sampler)) return true;
    ImGui_Implbgfx_InvalidateDeviceObjects();
    layout.begin().add(bgfx::Attrib::Position, 2, bgfx::AttribType::Float)
        .add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
        .add(bgfx::Attrib::Color0, 4, bgfx::AttribType::Uint8, true).end();
    const auto renderer = bgfx::getRendererType();
    auto vs = bgfx::createEmbeddedShader(shaders, renderer, "vs_ocornut_imgui");
    auto fs = bgfx::createEmbeddedShader(shaders, renderer, "fs_ocornut_imgui");
    if (!bgfx::isValid(vs) || !bgfx::isValid(fs)) {
        if (bgfx::isValid(vs)) bgfx::destroy(vs);
        if (bgfx::isValid(fs)) bgfx::destroy(fs);
        return false;
    }
    program = bgfx::createProgram(vs, fs, true);
    sampler = bgfx::createUniform("s_tex", bgfx::UniformType::Sampler);
    unsigned char* pixels = nullptr;
    int width = 0, height = 0;
    ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    if (width > 0 && height > 0 && width <= 65535 && height <= 65535)
        font = bgfx::createTexture2D(uint16_t(width), uint16_t(height), false, 1,
            bgfx::TextureFormat::RGBA8, 0, bgfx::copy(pixels, uint32_t(width) * uint32_t(height) * 4));
    if (!bgfx::isValid(program) || !bgfx::isValid(sampler) || !bgfx::isValid(font)) {
        ImGui_Implbgfx_InvalidateDeviceObjects();
        return false;
    }
    ImGui::GetIO().Fonts->SetTexID(reinterpret_cast<ImTextureID>(uintptr_t(font.idx)));
    return true;
}

void ImGui_Implbgfx_RenderDrawLists(ImDrawData* data)
{
    if (!data || !bgfx::isValid(program)) return;
    const float fw = data->DisplaySize.x * data->FramebufferScale.x;
    const float fh = data->DisplaySize.y * data->FramebufferScale.y;
    if (!(fw >= 1 && fh >= 1) || fw > 65535 || fh > 65535) return;
    const auto width = uint16_t(fw), height = uint16_t(fh);
    setupView(*data, width, height);
    for (int i = 0; i < data->CmdListsCount; ++i) {
        const ImDrawList* list = data->CmdLists[i];
        const auto vertices = uint32_t(list->VtxBuffer.Size);
        const auto indices = uint32_t(list->IdxBuffer.Size);
        if (!vertices || !indices) {
            for (const ImDrawCmd& cmd : list->CmdBuffer) {
                if (cmd.UserCallback == ImDrawCallback_ResetRenderState) setupView(*data, width, height);
                else if (cmd.UserCallback) cmd.UserCallback(list, &cmd);
            }
            continue;
        }
        const bool index32 = sizeof(ImDrawIdx) == 4;
        if (bgfx::getAvailTransientVertexBuffer(vertices, layout) != vertices ||
            bgfx::getAvailTransientIndexBuffer(indices, index32) != indices) break;
        bgfx::TransientVertexBuffer tvb;
        bgfx::TransientIndexBuffer tib;
        bgfx::allocTransientVertexBuffer(&tvb, vertices, layout);
        bgfx::allocTransientIndexBuffer(&tib, indices, index32);
        std::memcpy(tvb.data, list->VtxBuffer.Data, vertices * sizeof(ImDrawVert));
        std::memcpy(tib.data, list->IdxBuffer.Data, indices * sizeof(ImDrawIdx));
        auto* encoder = bgfx::begin();
        for (const ImDrawCmd& cmd : list->CmdBuffer) {
            if (cmd.UserCallback) {
                if (cmd.UserCallback == ImDrawCallback_ResetRenderState) setupView(*data, width, height);
                else cmd.UserCallback(list, &cmd);
                continue;
            }
            if (!cmd.ElemCount || cmd.VtxOffset >= vertices || cmd.IdxOffset > indices ||
                cmd.ElemCount > indices - cmd.IdxOffset) continue;
            const float left = bx::max(0.0f, (cmd.ClipRect.x - data->DisplayPos.x) * data->FramebufferScale.x);
            const float top = bx::max(0.0f, (cmd.ClipRect.y - data->DisplayPos.y) * data->FramebufferScale.y);
            const float right = bx::min(float(width), (cmd.ClipRect.z - data->DisplayPos.x) * data->FramebufferScale.x);
            const float bottom = bx::min(float(height), (cmd.ClipRect.w - data->DisplayPos.y) * data->FramebufferScale.y);
            if (!(right > left && bottom > top)) continue;
            const uint16_t x = uint16_t(left), y = uint16_t(top);
            const bgfx::TextureHandle texture = { uint16_t(uintptr_t(cmd.GetTexID()) & 0xffff) };
            encoder->setScissor(x, y, uint16_t(right) - x, uint16_t(bottom) - y);
            encoder->setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_MSAA |
                BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_SRC_ALPHA, BGFX_STATE_BLEND_INV_SRC_ALPHA));
            encoder->setTexture(0, sampler, texture);
            encoder->setVertexBuffer(0, &tvb, cmd.VtxOffset, vertices - cmd.VtxOffset);
            encoder->setIndexBuffer(&tib, cmd.IdxOffset, cmd.ElemCount);
            encoder->submit(viewId, program);
        }
        bgfx::end(encoder);
    }
}

void ImGui_Implbgfx_Init(int view)
{
    viewId = bgfx::ViewId(view);
    auto& io = ImGui::GetIO();
    io.BackendRendererName = "falco_bgfx_pinned";
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
}
void ImGui_Implbgfx_NewFrame() { ImGui_Implbgfx_CreateDeviceObjects(); }
void ImGui_Implbgfx_Shutdown()
{
    ImGui_Implbgfx_InvalidateDeviceObjects();
    auto& io = ImGui::GetIO();
    io.BackendFlags &= ~ImGuiBackendFlags_RendererHasVtxOffset;
    io.BackendRendererName = nullptr;
}
