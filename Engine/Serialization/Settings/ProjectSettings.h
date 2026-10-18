#pragma once

#include <string>
#include <functional>
#include <stdexcept>

#include "../Serializers/BinarySerializer.h"

namespace GX
{
	class ProjectSettings : public Archive
	{
	private:
		std::string startupScene;

		int screenWidth = 1280;
		int screenHeight = 800;
		bool useNativeResolution = true;
		bool fullscreen = true;
		bool exclusiveMode = false;
		bool vsync = true;
		bool fxaa = false;
		// Presentation settings; exposure is a linear radiance multiplier.
		int tonemap = 1; // 0 Reinhard, 1 ACES, 2 Uchimura
		float exposure = 1.0f;
		float brightness = 0.0f;
		float contrast = 1.0f;
		float saturation = 1.0f;
        bool bloom = false;
        bool ssao = false;
        bool ssr = false;
        float ssrDistance = 30.0f, ssrThickness = 0.2f;
        int ssaoSamples = 16, bloomDownsample = 2;
        float ssaoRadius = 1.0f, ssaoIntensity = 1.0f, ssaoBias = 0.02f;
        float bloomThreshold = 1.0f, bloomIntensity = 0.1f, bloomRadius = 1.0f;
		bool useDynamicResolution = false;
		bool autoDynamicResolution = false;
		int autoDynamicResolutionTargetFPS = 60;
		float dynamicResolution = 1.0f;

		int directionalShadowResolution = 2048;
		int spotShadowResolution = 1024;
		int pointShadowResolution = 2048;
		int shadowCascadesCount = 4;
		int shadowSamplesCount = 2;
		bool shadowCascadesBlending = true;
		float shadowDistance = 100.0f;
		bool shadowsEnabled = true;

		int textureCompression = 2;
		int textureCompressionQuality = 1;
		int textureMaxResolution = 4096;

		bool enableSteamAPI = false;
		int steamAppId = 0;

		bool collisionMatrix[32][32];

		std::vector<std::string> tags;
		std::vector<std::string> layers;
		std::vector<std::string> scenes;

	public:
		ProjectSettings();
		~ProjectSettings() = default;
		int getVersion() override { return 5; }

		virtual void serialize(Serializer* s)
		{
			Archive::serialize(s);
            if (s->getOperation() == Serializer::Operation::Deserialize && (version < 0 || version > getVersion()))
                throw std::invalid_argument("Unsupported project settings version");
			data(startupScene);
			data(screenWidth);
			data(screenHeight);
			data(useNativeResolution);
			data(fullscreen);
			data(exclusiveMode);
			data(vsync);
			data(fxaa);
			data(useDynamicResolution);
			data(autoDynamicResolution);
			data(autoDynamicResolutionTargetFPS);
			data(dynamicResolution);
			data(directionalShadowResolution);
			data(spotShadowResolution);
			data(pointShadowResolution);
			data(shadowCascadesCount);
			data(shadowSamplesCount);
			data(shadowCascadesBlending);
			data(shadowsEnabled);
			data(shadowDistance);
			data(textureCompression);
			data(textureCompressionQuality);
			data(textureMaxResolution);
			data(enableSteamAPI);
			data(steamAppId);
			data(tags);
			data(layers);
			data(scenes);
			
			for (int i = 0; i < 32; ++i)
			{
				for (int j = 0; j < 32; ++j)
				{
					data(collisionMatrix[i][j]);
				}
			}
            // Append fields after the legacy payload; v0 files have no extension.
            if (version >= 1)
            {
                data(tonemap); data(exposure); data(brightness); data(contrast); data(saturation);
            }
            else if (s->getOperation() == Serializer::Operation::Deserialize)
            {
                tonemap = 1; exposure = 1.0f; brightness = 0.0f; contrast = 1.0f; saturation = 1.0f;
            }
            if (version >= 2)
            {
                data(bloom); data(bloomThreshold); data(bloomIntensity); data(bloomRadius);
            }
            else if (s->getOperation() == Serializer::Operation::Deserialize)
            {
                bloom = false; bloomThreshold = 1.0f; bloomIntensity = 0.1f; bloomRadius = 1.0f;
            }
            if (version >= 3)
            {
                data(ssao); data(ssaoRadius); data(ssaoIntensity); data(ssaoBias);
            }
            else if (s->getOperation() == Serializer::Operation::Deserialize)
            {
                ssao = false; ssaoRadius = 1.0f; ssaoIntensity = 1.0f; ssaoBias = 0.02f;
            }
            if (version >= 4) { data(ssaoSamples); data(bloomDownsample); }
            else if (s->getOperation() == Serializer::Operation::Deserialize) { ssaoSamples = 16; bloomDownsample = 2; }
            if (version >= 5) { data(ssr); data(ssrDistance); data(ssrThickness); }
            else if (s->getOperation() == Serializer::Operation::Deserialize) { ssr = false; ssrDistance = 30.0f; ssrThickness = 0.2f; }
            setSSRDistance(ssrDistance); setSSRThickness(ssrThickness);
            setSSAOSamples(ssaoSamples); setBloomDownsample(bloomDownsample);
            setSSAORadius(ssaoRadius); setSSAOIntensity(ssaoIntensity); setSSAOBias(ssaoBias);
            setBloomThreshold(bloomThreshold); setBloomIntensity(bloomIntensity); setBloomRadius(bloomRadius);
            setTonemap(tonemap); setExposure(exposure); setBrightness(brightness);
            setContrast(contrast); setSaturation(saturation);
		}

		void save();
		int getTonemap() const { return tonemap; }
		void setTonemap(int value);
		float getExposure() const { return exposure; }
		void setExposure(float value);
		float getBrightness() const { return brightness; }
		void setBrightness(float value);
		float getContrast() const { return contrast; }
		void setContrast(float value);
		float getSaturation() const { return saturation; }
		void setSaturation(float value);
        bool getBloom() const { return bloom; }
        void setBloom(bool value) { bloom = value; }
        float getBloomThreshold() const { return bloomThreshold; }
        void setBloomThreshold(float value);
        float getBloomIntensity() const { return bloomIntensity; }
        void setBloomIntensity(float value);
        float getBloomRadius() const { return bloomRadius; }
        void setBloomRadius(float value);
        bool getSSAO() const { return ssao; }
        void setSSAO(bool value) { ssao = value; }
        float getSSAORadius() const { return ssaoRadius; }
        void setSSAORadius(float value);
        float getSSAOIntensity() const { return ssaoIntensity; }
        void setSSAOIntensity(float value);
        float getSSAOBias() const { return ssaoBias; }
        void setSSAOBias(float value);
        int getSSAOSamples() const { return ssaoSamples; }
        void setSSAOSamples(int value) { ssaoSamples = value == 8 || value == 32 ? value : 16; }
        int getBloomDownsample() const { return bloomDownsample; }
        void setBloomDownsample(int value) { bloomDownsample = value == 1 || value == 4 ? value : 2; }
        // 0 Custom, 1 Low, 2 Medium, 3 High. Derived from actual settings.
        int getGraphicsPreset() const;
        void applyGraphicsPreset(int preset);
        bool getSSR() const { return ssr; }
        void setSSR(bool value) { ssr = value; }
        float getSSRDistance() const { return ssrDistance; }
        void setSSRDistance(float value);
        float getSSRThickness() const { return ssrThickness; }
        void setSSRThickness(float value);
		void load();

		std::string getStartupScene() { return startupScene; }
		void setStartupScene(std::string value) { startupScene = value; }

		int getScreenWidth() { return screenWidth; }
		void setScreenWidth(int value) { screenWidth = value; }

		int getScreenHeight() { return screenHeight; }
		void setScreenHeight(int value) { screenHeight = value; }

		bool getUseNativeResolution() { return useNativeResolution; }
		void setUseNativeResolution(bool value) { useNativeResolution = value; }

		bool getFullScreen() { return fullscreen; }
		void setFullScreen(bool value) { fullscreen = value; }

		bool getExclusiveMode() { return exclusiveMode; }
		void setExclusiveMode(bool value) { exclusiveMode = value; }

		bool getVSync() { return vsync; }
		void setVSync(bool value);

		bool getFXAA() { return fxaa; }
		void setFXAA(bool value) { fxaa = value; }

		bool getUseDynamicResolution() { return useDynamicResolution; }
		void setUseDynamicResolution(bool value);

		bool getAutoDynamicResolution() { return autoDynamicResolution; }
		void setAutoDynamicResolution(bool value);

		int getAutoDynamicResolutionTargetFPS() { return autoDynamicResolutionTargetFPS; }
		void setAutoDynamicResolutionTargetFPS(int value) { autoDynamicResolutionTargetFPS = value; }
		
		float getDynamicResolution() { return dynamicResolution; }
		void setDynamicResolution(float value);

		int getDirectionalShadowResolution() { return directionalShadowResolution; }
		void setDirectionalShadowResolution(int value);

		int getSpotShadowResolution() { return spotShadowResolution; }
		void setSpotShadowResolution(int value);

		int getPointShadowResolution() { return pointShadowResolution; }
		void setPointShadowResolution(int value);

		int getShadowCascadesCount() { return shadowCascadesCount; }
		void setShadowCascadesCount(int value);

		int getShadowSamplesCount() { return shadowSamplesCount; }
		void setShadowSamplesCount(int value);

		bool getShadowCascadesBlending() { return shadowCascadesBlending; }
		void setShadowCascadesBlending(bool value);

		bool getShadowsEnabled() { return shadowsEnabled; }
		void setShadowsEnabled(bool value) { shadowsEnabled = value; }

		float getShadowDistance() { return shadowDistance; }
		void setShadowDistance(float value) { shadowDistance = value; }

		int getTextureCompression() { return textureCompression; }
		void setTextureCompression(int value, std::function<void(std::string status, int progress)> callback = nullptr);

		int getTextureCompressionQuality() { return textureCompressionQuality; }
		void setTextureCompressionQuality(int value, std::function<void(std::string status, int progress)> callback = nullptr);

		int getTextureMaxResolution() { return textureMaxResolution; }
		void setTextureMaxResolution(int value, std::function<void(std::string status, int progress)> callback = nullptr);

		bool getEnableSteamAPI() { return enableSteamAPI; }
		void setEnableSteamAPI(bool value) { enableSteamAPI = value; }

		int getSteamAppId() { return steamAppId; }
		void setSteamAppId(int value) { steamAppId = value; }

		bool getCollisionMask(int i, int j) { return collisionMatrix[i][j]; }
		void setCollisionMask(int i, int j, bool value);

		std::vector<std::string> & getTags() { return tags; }
		std::vector<std::string> & getLayers() { return layers; }
		std::vector<std::string> & getScenes() { return scenes; }
	};
}
