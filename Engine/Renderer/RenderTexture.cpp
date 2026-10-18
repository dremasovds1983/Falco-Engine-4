#include "RenderTexture.h"
#include "../Core/Debug.h"

#include "../Core/Engine.h"
#include "../Core/APIManager.h"
#include "../Assets/Texture.h"
#include "../Classes/GUIDGenerator.h"

namespace GX
{
	RenderTexture::RenderTexture(int w, int h, TextureType type, bgfx::TextureFormat::Enum format) : Object(APIManager::getSingleton()->rendertexture_class)
	{
		textureType = type;
		colorFormat = format;

		colorTexture = new Texture();

		if (type == TextureType::ColorWithDepth)
			depthTexture = new Texture();

		reset(w, h);
	}

	RenderTexture::~RenderTexture()
	{
		release();

		if (colorTexture != nullptr)
			delete colorTexture;

		if (depthTexture != nullptr)
			delete depthTexture;

		colorTexture = nullptr;
		depthTexture = nullptr;
	}

	void RenderTexture::release()
	{
		if (bgfx::isValid(frameBufferHandle))
		{
			// Every RenderTexture framebuffer is created with destroyTextures=true,
			// so destroying it also releases both attachment handles.
			bgfx::destroy(frameBufferHandle);
		}
		else
		{
			// A failed framebuffer allocation does not own the textures that were
			// created immediately before it. Release those explicitly.
			if (bgfx::isValid(colorTextureHandle))
				bgfx::destroy(colorTextureHandle);
			if (bgfx::isValid(depthTextureHandle))
				bgfx::destroy(depthTextureHandle);
		}

		frameBufferHandle = BGFX_INVALID_HANDLE;
		colorTextureHandle = BGFX_INVALID_HANDLE;
		depthTextureHandle = BGFX_INVALID_HANDLE;

		if (colorTexture != nullptr)
			colorTexture->textureHandle = BGFX_INVALID_HANDLE;
		if (depthTexture != nullptr)
			depthTexture->textureHandle = BGFX_INVALID_HANDLE;
	}

	void RenderTexture::reset(int w, int h)
	{
		release();

		width = w;
		height = h;

		if (width < 8) width = 8;
		if (height < 8) height = 8;

        const bgfx::Caps* caps = bgfx::getCaps();
        if (caps == nullptr || width > 65535 || height > 65535
            || uint32_t(width) > caps->limits.maxTextureSize || uint32_t(height) > caps->limits.maxTextureSize
            || !bgfx::isTextureValid(0, false, 1, colorFormat, BGFX_TEXTURE_RT)
            || (textureType == TextureType::ColorWithDepth
                && !bgfx::isTextureValid(0, false, 1, bgfx::TextureFormat::D24S8, BGFX_TEXTURE_RT)))
        {
            Debug::logError("RenderTexture: unsupported target dimensions or attachment format");
            return;
        }

		colorTextureHandle = bgfx::createTexture2D(uint16_t(width), uint16_t(height), false, 1, colorFormat,
			BGFX_TEXTURE_RT
			| BGFX_SAMPLER_MIN_ANISOTROPIC
			| BGFX_SAMPLER_MAG_ANISOTROPIC
			//| BGFX_SAMPLER_MIP_POINT
			| BGFX_SAMPLER_U_CLAMP
			| BGFX_SAMPLER_V_CLAMP);

        if (!bgfx::isValid(colorTextureHandle))
        {
            Debug::logError("RenderTexture: color allocation failed");
            return;
        }

		colorTexture->textureHandle = colorTextureHandle;
		colorTexture->width = width;
		colorTexture->originalWidth = width;
		colorTexture->height = height;
		colorTexture->originalHeight = height;
		colorTexture->format = colorFormat;

		if (textureType == TextureType::ColorWithDepth)
		{
			depthTextureHandle = bgfx::createTexture2D(uint16_t(width), uint16_t(height), false, 1, bgfx::TextureFormat::D24S8,
				BGFX_TEXTURE_RT
				| BGFX_SAMPLER_MIN_POINT
				| BGFX_SAMPLER_MAG_POINT
				| BGFX_SAMPLER_MIP_POINT
				| BGFX_SAMPLER_U_CLAMP
				| BGFX_SAMPLER_V_CLAMP);

            if (!bgfx::isValid(depthTextureHandle))
            {
                Debug::logError("RenderTexture: depth allocation failed");
                release();
                return;
            }

			bgfx::Attachment at[2];
			at[0].init(colorTextureHandle, bgfx::Access::Write, 0, 1, 0, BGFX_RESOLVE_NONE);
			at[1].init(depthTextureHandle, bgfx::Access::Write, 0, 1, 0, BGFX_RESOLVE_NONE);

			frameBufferHandle = bgfx::createFrameBuffer(2, at, true);

			depthTexture->textureHandle = depthTextureHandle;
			depthTexture->width = width;
			depthTexture->originalWidth = width;
			depthTexture->height = height;
			depthTexture->originalHeight = height;
			depthTexture->format = bgfx::TextureFormat::D24S8;
		}
		else
		{
			bgfx::Attachment at[1];
			at[0].init(colorTextureHandle, bgfx::Access::Write, 0, 1, 0, BGFX_RESOLVE_NONE);

			frameBufferHandle = bgfx::createFrameBuffer(1, at, true);
		}
        if (!bgfx::isValid(frameBufferHandle))
        {
            Debug::logError("RenderTexture: framebuffer creation failed");
            release();
        }

	}
}
