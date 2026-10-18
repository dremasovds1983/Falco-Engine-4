#include "ProjectSettings.h"
#include <cmath>
#include <algorithm>

#include "Classes/IO.h"
#include "Classes/md5.h"

#include <boost/serialization/export.hpp>
#include <boost/serialization/serialization.hpp>

#include "../../Core/Engine.h"
#include "../../Core/PhysicsManager.h"
#include "../../Core/Debug.h"
#include "../../Renderer/Renderer.h"
#include "../../Components/Light.h"
#include "../../Assets/Asset.h"
#include "../../Assets/Texture.h"

#include <SDL2/SDL.h>

namespace GX
{
    void updateLights();
	namespace
	{
		float presentationValue(float value, float minimum, float maximum, float fallback)
		{
			return std::isfinite(value) ? std::max(minimum, std::min(maximum, value)) : fallback;
		}
	}
	void ProjectSettings::setTonemap(int value) { tonemap = value >= 0 && value <= 2 ? value : 1; }
	void ProjectSettings::setExposure(float value) { exposure = presentationValue(value, 0.01f, 16.0f, 1.0f); }
	void ProjectSettings::setBrightness(float value) { brightness = presentationValue(value, -1.0f, 1.0f, 0.0f); }
	void ProjectSettings::setContrast(float value) { contrast = presentationValue(value, 0.0f, 4.0f, 1.0f); }
	void ProjectSettings::setSaturation(float value) { saturation = presentationValue(value, 0.0f, 4.0f, 1.0f); }

    void ProjectSettings::setBloomThreshold(float value) { bloomThreshold = presentationValue(value, 0.0f, 32.0f, 1.0f); }
    void ProjectSettings::setBloomIntensity(float value) { bloomIntensity = presentationValue(value, 0.0f, 4.0f, 0.1f); }
    void ProjectSettings::setBloomRadius(float value) { bloomRadius = presentationValue(value, 0.5f, 4.0f, 1.0f); }

    void ProjectSettings::setSSAORadius(float value) { ssaoRadius = presentationValue(value, 0.01f, 20.0f, 1.0f); }
    void ProjectSettings::setSSAOIntensity(float value) { ssaoIntensity = presentationValue(value, 0.0f, 4.0f, 1.0f); }
    void ProjectSettings::setSSAOBias(float value) { ssaoBias = presentationValue(value, 0.0f, 0.5f, 0.02f); }

    int ProjectSettings::getGraphicsPreset() const
    {
        for (int p = 1; p <= 3; ++p)
        {
            if (fxaa && shadowsEnabled && directionalShadowResolution == (p == 1 ? 1024 : p == 2 ? 2048 : 4096)
                && spotShadowResolution == (p == 1 ? 512 : p == 2 ? 1024 : 2048)
                && pointShadowResolution == (p == 1 ? 512 : p == 2 ? 1024 : 2048)
                && shadowCascadesCount == (p == 1 ? 2 : 4) && shadowSamplesCount == (p == 3 ? 4 : 2)
                && shadowCascadesBlending == (p != 1) && ssao == (p != 1) && bloom == (p != 1)
                && ssaoSamples == (p == 1 ? 8 : p == 2 ? 16 : 32)
                && bloomDownsample == (p == 1 ? 4 : p == 2 ? 2 : 1) && ssr == (p == 3)) return p;
        }
        return 0;
    }
    void ProjectSettings::applyGraphicsPreset(int p)
    {
        if (p < 1 || p > 3) return;
        // Batch changes: recreate shadow resources once, not once per field.
        fxaa = true; shadowsEnabled = true;
        directionalShadowResolution = p == 1 ? 1024 : p == 2 ? 2048 : 4096;
        spotShadowResolution = pointShadowResolution = p == 1 ? 512 : p == 2 ? 1024 : 2048;
        shadowCascadesCount = p == 1 ? 2 : 4; shadowSamplesCount = p == 3 ? 4 : 2;
        shadowCascadesBlending = p != 1; ssao = bloom = p != 1; ssr = p == 3;
        ssaoSamples = p == 1 ? 8 : p == 2 ? 16 : 32;
        bloomDownsample = p == 1 ? 4 : p == 2 ? 2 : 1;
        updateLights();
    }

    void ProjectSettings::setSSRDistance(float value) { ssrDistance = presentationValue(value, 0.1f, 500.0f, 30.0f); }
    void ProjectSettings::setSSRThickness(float value) { ssrThickness = presentationValue(value, 0.001f, 5.0f, 0.2f); }

	ProjectSettings::ProjectSettings()
	{
		for (int i = 0; i < 32; ++i)
		{
			for (int j = 0; j < 32; ++j)
			{
				collisionMatrix[i][j] = true;
			}
		}
	}

	void ProjectSettings::save()
	{
		std::string path = Engine::getSingleton()->getSettingsPath() + "Engine.settings";

		std::ofstream ofs(path, std::ios::binary);
		BinarySerializer s;
		s.serialize(&ofs, this, "Project Settings");
		ofs.close();
	}

	void ProjectSettings::load()
	{
		std::string path = Engine::getSingleton()->getSettingsPath() + "Engine.settings";

		if (IO::FileExists(path))
		{
			//Deserialize
			try
			{
                // Never publish partially read settings (especially HDR/shadow
                // values) when an old or interrupted file cannot be decoded.
                ProjectSettings loaded;
                std::ifstream input(path, std::ios::binary);
                input.exceptions(std::ios::failbit | std::ios::badbit);
                BinarySerializer serializer;
                serializer.deserialize(&input, &loaded, "Project Settings");
                input.close();
                *this = loaded;
			}
			catch(const std::exception& e)
			{
				Debug::logError(std::string("Project settings load error: ") + e.what());
				std::cerr << "Project settings load error: " << e.what() << std::endl;
			}
		}
	}

	void updateLights()
	{
		std::vector<Light*>& lights = Renderer::getSingleton()->getLights();
		for (auto it = lights.begin(); it != lights.end(); ++it)
		{
			(*it)->updateShadowMaps();
		}
	}

	void ProjectSettings::setUseDynamicResolution(bool value)
	{
		useDynamicResolution = value;
		Renderer::getSingleton()->resetFrameBuffers();
	}

	void ProjectSettings::setAutoDynamicResolution(bool value)
	{
		autoDynamicResolution = value;
		Renderer::getSingleton()->resetFrameBuffers();
	}

	void ProjectSettings::setDynamicResolution(float value)
	{
		dynamicResolution = value;
		Renderer::getSingleton()->resetFrameBuffers();
	}

	void ProjectSettings::setDirectionalShadowResolution(int value)
	{
		directionalShadowResolution = value;
		updateLights();
	}

	void ProjectSettings::setSpotShadowResolution(int value)
	{
		spotShadowResolution = value;
		updateLights();
	}

	void ProjectSettings::setPointShadowResolution(int value)
	{
		pointShadowResolution = value;
		updateLights();
	}

	void ProjectSettings::setShadowCascadesCount(int value)
	{
		shadowCascadesCount = value;
		updateLights();
	}

	void ProjectSettings::setShadowSamplesCount(int value)
	{
		shadowSamplesCount = value;
		updateLights();
	}

	void ProjectSettings::setShadowCascadesBlending(bool value)
	{
		shadowCascadesBlending = value;
		updateLights();
	}

	void ProjectSettings::setVSync(bool value)
	{
		vsync = value;
		
	#ifndef FALCO_MODERN_BGFX
		if (vsync)
			SDL_GL_SetSwapInterval(1);
		else
			SDL_GL_SetSwapInterval(0);
	#endif

		int w = Renderer::getSingleton()->getWidth();
		int h = Renderer::getSingleton()->getHeight();

		Renderer::getSingleton()->setSize(w, h);
	}

	void reloadTextures(std::function<void(std::string status, int progress)> callback)
	{
		std::vector<std::string> texFiles;
		auto& exts = Engine::getImageFileFormats();

		IO::listFiles(Engine::getSingleton()->getAssetsPath(), true, nullptr, [=, &texFiles](std::string dir, std::string name) -> bool
			{
				std::string filePath = dir + name;
				std::string ext = IO::GetFileExtension(filePath);

				if (std::find(exts.begin(), exts.end(), ext) != exts.end())
					texFiles.push_back(filePath);

				return true;
			}
		);

		int count = texFiles.size();

		int i = 0;
		for (auto& texFile : texFiles)
		{
			int progress = 100.0f / count * i;
			std::string texName = IO::GetFileNameWithExt(texFile);
			std::string str = texName + " (" + std::to_string(i) + "/" + std::to_string(count) + ")";

			if (callback != nullptr)
				callback(str, progress);

			///

			Texture* texture = nullptr;
			for (auto& asset : Asset::getLoadedInstances())
			{
				if (asset.second->getAssetType() == Texture::ASSET_TYPE && asset.second->isLoaded())
				{
					if (asset.first == texFile)
					{
						texture = (Texture*)asset.second;
						break;
					}
				}
			}

			if (texture != nullptr)
			{
				std::string texCache = texture->getCachedFileName();
				if (IO::FileExists(texCache))
					IO::FileDelete(texCache);
				texture->reload();
			}
			else
			{
				std::string location = Engine::getSingleton()->getAssetsPath();
				std::string name = IO::RemovePart(texFile, location);

				std::string libLocation = Engine::getSingleton()->getLibraryPath();
				std::string texDir = IO::GetFilePath(name);
				std::string texName = libLocation + texDir + md5(name) + ".texture";

				if (IO::FileExists(texName))
				{
					texture = Texture::load(location, name);
					if (texture != nullptr)
					{
						IO::FileDelete(texName);
						texture->reload();
						delete texture;
					}
				}
			}

			++i;
		}
	}

	void ProjectSettings::setTextureCompression(int value, std::function<void(std::string status, int progress)> callback)
	{
		textureCompression = value;
		reloadTextures(callback);
	}

	void ProjectSettings::setTextureCompressionQuality(int value, std::function<void(std::string status, int progress)> callback)
	{
		textureCompressionQuality = value;
		reloadTextures(callback);
	}

	void ProjectSettings::setTextureMaxResolution(int value, std::function<void(std::string status, int progress)> callback)
	{
		textureMaxResolution = value;
		reloadTextures(callback);
	}

	void ProjectSettings::setCollisionMask(int i, int j, bool value)
	{
		collisionMatrix[i][j] = value;
		PhysicsManager::getSingleton()->updateCollisionMatrix();
	}
}
