#pragma once
#include <bgfx/bgfx.h>
#include <initializer_list>
#include "../Core/Debug.h"
namespace GX
{
    inline bool supportsSceneTargets(int width, int height)
    {
        const auto* caps = bgfx::getCaps();
        const bool supported = caps && width >= 8 && height >= 8 && width <= 65535 && height <= 65535
            && uint32_t(width) <= caps->limits.maxTextureSize && uint32_t(height) <= caps->limits.maxTextureSize
            && caps->limits.maxFBAttachments >= 5
            && bgfx::isTextureValid(0, false, 1, bgfx::TextureFormat::RGBA16F, BGFX_TEXTURE_RT)
            && bgfx::isTextureValid(0, false, 1, bgfx::TextureFormat::BGRA8, BGFX_TEXTURE_RT)
            && bgfx::isTextureValid(0, false, 1, bgfx::TextureFormat::D24S8, BGFX_TEXTURE_RT);
        if (!supported) Debug::logError("HDR scene targets: unsupported dimensions, formats or attachment count");
        return supported;
    }
    inline bgfx::FrameBufferHandle createOwnedTarget(std::initializer_list<bgfx::TextureHandle> textures)
    {
        bgfx::Attachment attachments[5];
        uint8_t count = 0;
        for (auto texture : textures)
        {
            if (!bgfx::isValid(texture) || count >= 5) return BGFX_INVALID_HANDLE;
            attachments[count++].init(texture, bgfx::Access::Write, 0, 1, 0, BGFX_RESOLVE_NONE);
        }
        return bgfx::createFrameBuffer(count, attachments, true);
    }
    inline void releaseOwnedTarget(bgfx::FrameBufferHandle target, std::initializer_list<bgfx::TextureHandle> textures)
    {
        if (bgfx::isValid(target)) bgfx::destroy(target);
        else for (auto texture : textures) if (bgfx::isValid(texture)) bgfx::destroy(texture);
    }
}
