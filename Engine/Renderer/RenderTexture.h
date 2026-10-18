#pragma once

#include <vector>
#include <bgfx/bgfx.h>

#include "../Core/Object.h"

namespace GX
{
	class Texture;

	class RenderTexture : public Object
	{
	public:
		enum class TextureType
		{
			ColorWithDepth,
			ColorOnly
		};

	private:
		bgfx::FrameBufferHandle frameBufferHandle = { bgfx::kInvalidHandle };
		bgfx::TextureHandle colorTextureHandle = { bgfx::kInvalidHandle };
		bgfx::TextureHandle depthTextureHandle = { bgfx::kInvalidHandle };

		int width = 0;
		int height = 0;

		Texture* colorTexture = nullptr;
		Texture* depthTexture = nullptr;

		TextureType textureType = TextureType::ColorWithDepth;
		bgfx::TextureFormat::Enum colorFormat = bgfx::TextureFormat::BGRA8;

	public:
		RenderTexture(int w, int h, TextureType type = TextureType::ColorWithDepth,
			bgfx::TextureFormat::Enum format = bgfx::TextureFormat::BGRA8);
		virtual ~RenderTexture();

		// Release the bgfx framebuffer and its owned attachments without deleting
		// the RenderTexture wrapper. This is used by editor resize paths that must
		// let bgfx retire old handles before allocating the replacement target.
		void release();
		void reset(int w, int h);

		int getWidth() { return width; }
		int getHeight() { return height; }
		bgfx::TextureFormat::Enum getColorFormat() const { return colorFormat; }

		bgfx::TextureHandle getColorTextureHandle() { return colorTextureHandle; }
		bgfx::TextureHandle getDepthTextureHandle() { return depthTextureHandle; }
		bgfx::FrameBufferHandle getFrameBufferHandle() { return frameBufferHandle; }

		Texture* getColorTexture() { return colorTexture; }
		Texture* getDepthTexture() { return depthTexture; }
	};
}
