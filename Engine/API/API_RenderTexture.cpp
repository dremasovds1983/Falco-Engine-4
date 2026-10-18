#include "API_RenderTexture.h"

#include "../Core/APIManager.h"
#include "../Renderer/RenderTexture.h"
#include "../Classes/StringConverter.h"
#include "../Assets/Texture.h"

namespace GX
{
	void API_RenderTexture::ctor(MonoObject* this_ptr, int w, int h, int type)
	{
		RenderTexture* rt = new RenderTexture(w, h, static_cast<RenderTexture::TextureType>(type));

		rt->setManagedObject(this_ptr);
	}

    bool API_RenderTexture::ctorFormat(MonoObject* this_ptr, int w, int h, int type, int format)
    {
        if (w <= 0 || h <= 0 || type < 0 || type > 1 || format < 0 || format > 1) return false;
        RenderTexture* rt = new RenderTexture(w, h, static_cast<RenderTexture::TextureType>(type),
            format == 1 ? bgfx::TextureFormat::RGBA16F : bgfx::TextureFormat::BGRA8);
        if (!bgfx::isValid(rt->getFrameBufferHandle()))
        {
            delete rt;
            return false;
        }
        rt->setManagedObject(this_ptr);
        return true;
    }

    int API_RenderTexture::getColorFormat(MonoObject* this_ptr)
    {
        RenderTexture* object = nullptr;
        mono_field_get_value(this_ptr, APIManager::getSingleton()->object_ptr_field, reinterpret_cast<void*>(&object));
        return object != nullptr && object->getColorFormat() == bgfx::TextureFormat::RGBA16F ? 1 : 0;
    }

    bool API_RenderTexture::isValid(MonoObject* this_ptr)
    {
        RenderTexture* object = nullptr;
        mono_field_get_value(this_ptr, APIManager::getSingleton()->object_ptr_field, reinterpret_cast<void*>(&object));
        return object != nullptr && bgfx::isValid(object->getFrameBufferHandle());
    }

	MonoObject* API_RenderTexture::getColorTexture(MonoObject* this_ptr)
	{
		RenderTexture* object = nullptr;
		mono_field_get_value(this_ptr, APIManager::getSingleton()->object_ptr_field, reinterpret_cast<void*>(&object));

		if (object != nullptr)
		{
			Texture* tex = object->getColorTexture();
			if (tex != nullptr)
				return tex->getManagedObject();
		}

		return nullptr;
	}

	MonoObject* API_RenderTexture::getDepthTexture(MonoObject* this_ptr)
	{
		RenderTexture* object = nullptr;
		mono_field_get_value(this_ptr, APIManager::getSingleton()->object_ptr_field, reinterpret_cast<void*>(&object));

		if (object != nullptr)
		{
			Texture* tex = object->getDepthTexture();
			if (tex != nullptr)
				return tex->getManagedObject();
		}

		return nullptr;
	}

	int API_RenderTexture::getWidth(MonoObject* this_ptr)
	{
		RenderTexture* object = nullptr;
		mono_field_get_value(this_ptr, APIManager::getSingleton()->object_ptr_field, reinterpret_cast<void*>(&object));

		if (object != nullptr)
			return object->getWidth();

		return 0;
	}

	int API_RenderTexture::getHeight(MonoObject* this_ptr)
	{
		RenderTexture* object = nullptr;
		mono_field_get_value(this_ptr, APIManager::getSingleton()->object_ptr_field, reinterpret_cast<void*>(&object));

		if (object != nullptr)
			return object->getHeight();

		return 0;
	}

	void API_RenderTexture::reset(MonoObject* this_ptr, int w, int h)
	{
		RenderTexture* object = nullptr;
		mono_field_get_value(this_ptr, APIManager::getSingleton()->object_ptr_field, reinterpret_cast<void*>(&object));

		if (object != nullptr)
		{
			object->reset(w, h);
		}
	}

	void API_RenderTexture::dispose(MonoObject* this_ptr)
	{
		RenderTexture* object = nullptr;
		mono_field_get_value(this_ptr, APIManager::getSingleton()->object_ptr_field, reinterpret_cast<void*>(&object));

		if (object != nullptr)
			delete object;
	}
}