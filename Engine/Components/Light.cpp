#include "Light.h"

#include <algorithm>
#include <cmath>
#include "../Core/Time.h"

#include <bx/bx.h>
#include <glm/ext/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include "../Core/Engine.h"
#include "../Core/APIManager.h"

#include "../Core/GameObject.h"
#include "Transform.h"
#include "Camera.h"

#include "../Assets/Texture.h"

#include "../Renderer/Renderer.h"
#include "../Renderer/Primitives.h"

#include "Math/AxisAlignedBox.h"

#include "Serialization/Settings/ProjectSettings.h"
#include "Serialization/Settings/ProjectSettings.h"

namespace GX
{
	std::string Light::COMPONENT_TYPE = "Light";

	Light::Light() : Component(APIManager::getSingleton()->light_class)
	{
		for (int i = 0; i < 4; ++i)
		{
			mShadowMtx[i] = glm::identity<glm::mat4x4>();
			mShadowFrustum[i] = new Frustum();
		}
	}

	Light::~Light()
	{
		for (int i = 0; i < 4; ++i)
		{
			delete mShadowFrustum[i];
			mShadowFrustum[i] = nullptr;
		}
	}

	std::string Light::getComponentType()
	{
		return COMPONENT_TYPE;
	}

	Component* Light::onClone()
	{
		Light* newComponent = new Light();

		newComponent->enabled = enabled;
		newComponent->lightType = lightType;
		newComponent->lightRenderMode = lightRenderMode;
		newComponent->color = color;
		newComponent->intensity = intensity;
		newComponent->stylePattern = stylePattern;
		newComponent->styleSpeed = styleSpeed;
		newComponent->radius = radius;
		newComponent->innerRadius = innerRadius;
		newComponent->outerRadius = outerRadius;
		newComponent->bias = bias;
		newComponent->castShadows = castShadows;

		return newComponent;
	}

	void Light::onStateChanged()
	{
		updateShadowMaps();
	}

	void Light::onAttach()
	{
		Component::onAttach();

		std::vector<Light*>& lights = Renderer::getSingleton()->getLights();
		if (std::find(lights.begin(), lights.end(), this) == lights.end())
			lights.push_back(this);

		updateShadowMaps();
	}

	void Light::onDetach()
	{
		Component::onDetach();

		std::vector<Light*>& lights = Renderer::getSingleton()->getLights();
		lights.erase(std::remove(lights.begin(), lights.end(), this), lights.end());

		destroyShadowMaps();
	}

	void Light::setLightType(LightType value)
	{
		lightType = value;
		updateShadowMaps();

		if (value == LightType::Point)
			setBias(0.002f);
		if (value == LightType::Spot)
			setBias(0.002f);
		if (value == LightType::Directional)
		{
			setBias(0.000010f);
			updateShadowMaps();
		}
	}

	void Light::setLightRenderMode(LightRenderMode value)
	{
		lightRenderMode = value;
		updateShadowMaps();
	}

	void Light::setInnerRadius(float value)
	{
		innerRadius = glm::clamp(value, 0.0f, 179.0f);
		if (innerRadius > outerRadius)
			outerRadius = innerRadius;
	}

	void Light::setOuterRadius(float value)
	{
		outerRadius = glm::clamp(value, 0.0f, 179.0f);
		if (outerRadius < innerRadius)
			innerRadius = outerRadius;
	}

	void Light::setCastShadows(bool value)
	{
		castShadows = value;
		updateShadowMaps();
	}

	void Light::destroyShadowMaps()
	{
		for (auto it = shadowMapFB.begin(); it != shadowMapFB.end(); ++it)
		{
			bgfx::FrameBufferHandle& fbh = *it;
			if (bgfx::isValid(fbh))
				bgfx::destroy(fbh);
		}

		shadowMapFB.clear();
		shadowTextures.clear();
		//rsmColorTextures.clear();
		//rsmNormalTextures.clear();

		for (int iii = 0; iii < 4; ++iii)
		{
			mShadowMtx[iii] = glm::identity<glm::mat4x4>();
			//mInvShadowMtx[iii] = glm::identity<glm::mat4x4>();
		}
	}

	void Light::updateShadowMaps()
	{
		destroyShadowMaps();

		if (!castShadows)
			return;

		if (lightRenderMode == LightRenderMode::Baked)
			return;

		if (getGameObject() != nullptr)
		{
			if (!getGameObject()->getActive())
				return;
		}
		else
			return;

		if (!getEnabled())
			return;

		ProjectSettings* projectSettings = Engine::getSingleton()->getSettings();

		int shadowMapDirectionalSize = projectSettings->getDirectionalShadowResolution();
		int shadowMapPointSize = projectSettings->getPointShadowResolution();
		int shadowMapSpotSize = projectSettings->getSpotShadowResolution();
		int numTextures = 1;

		if (lightType == LightType::Directional)
		{
			shadowMapSize = shadowMapDirectionalSize;
			numTextures = projectSettings->getShadowCascadesCount();
		}

		if (lightType == LightType::Point)
		{
			shadowMapSize = shadowMapPointSize;
			numTextures = 1;
		}

		if (lightType == LightType::Spot)
		{
			shadowMapSize = shadowMapSpotSize;
			numTextures = 1;
		}

		shadowTextures.resize(numTextures);
		shadowMapFB.resize(numTextures);
		// Cascaded directional shadows cover a much larger light-space depth
		// span than local lights. D16 quantization produces visible acne even at
		// a 1e-5 compare bias and forces a larger bias that leaks through thin
		// geometry. Use the already-supported 24-bit depth/stencil format for
		// Directional only; Point and Spot retain the cheaper D16 maps.
		const bgfx::TextureFormat::Enum shadowDepthFormat =
			lightType == LightType::Directional
				? bgfx::TextureFormat::D24S8
				: bgfx::TextureFormat::D16;

		for (int i = 0; i < numTextures; ++i)
		{
			//Regular shadow map
			shadowTextures[i] = bgfx::createTexture2D(
				shadowMapSize
				, shadowMapSize
				, false
				, 1
				, shadowDepthFormat
				, 0 |
				BGFX_TEXTURE_RT |
				BGFX_SAMPLER_UVW_CLAMP |
				BGFX_SAMPLER_COMPARE_LEQUAL
			);

			bgfx::Attachment fbtex[1];
			fbtex[0].init(shadowTextures[i], bgfx::Access::Write, 0, 1, 0, BGFX_RESOLVE_NONE);

			shadowMapFB[i] = bgfx::createFrameBuffer(1, fbtex, true);
		}
	}

	bgfx::TextureHandle & Light::getShadowMap(int i)
	{
		return shadowTextures[i];
	}

	int Light::getNumShadowMaps()
	{
		return shadowTextures.size();
	}

	void Light::setStylePattern(const std::string& value)
	{
		stylePattern.clear();
		for (char c : value)
		{
			if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
			if (c >= 'a' && c <= 'z') stylePattern.push_back(c);
			if (stylePattern.size() == 256) break;
		}
	}

	void Light::setStyleSpeed(float value)
	{
		styleSpeed = std::isfinite(value) ? glm::clamp(value, 0.0f, 120.0f) : 10.0f;
	}

	float Light::getStyleMultiplier() const
	{
		if (stylePattern.empty() || lightRenderMode == LightRenderMode::Baked) return 1.0f;
		double time = Time::getTimeSinceLevelStartScaled();
		if (!std::isfinite(time)) return 1.0f;
		size_t index = static_cast<size_t>(std::fmod(std::floor(std::max(time, 0.0) * styleSpeed), static_cast<double>(stylePattern.size())));
		int level = stylePattern[index] - 'a';
		return level <= 12 ? level / 12.0f : 1.0f + (level - 12) / 13.0f;
	}

	bool Light::submitUniforms()
	{
		if (!getGameObject()->getActive())
			return false;

		if (!getEnabled())
			return false;

		Transform* transform = getGameObject()->getTransform();
		if (transform == nullptr)
			return false;

		ProjectSettings* settings = Engine::getSingleton()->getSettings();
		bool shadows = settings->getShadowsEnabled() && Renderer::getSingleton()->getShadowsEnabled();
		float pcfSamples = settings->getShadowSamplesCount();

		int numSplits = settings->getShadowCascadesCount();

		float mScl = 1024.0f / shadowMapSize;
		
		float m1 = 1.05f * mScl;
		float m2 = 1.20f * mScl;
		float m3 = 4.40f * mScl;

		if (numSplits == 3)
		{
			m1 = 1.20f * mScl;
			m2 = 4.40f * mScl;
		}

		if (numSplits == 2)
			m1 = 4.40f * mScl;

		glm::vec4 _position = glm::vec4(transform->getPosition(), 0.0f);
		glm::vec4 _direction = glm::vec4(transform->getForward(), 0.0f);
		glm::vec4 _color = glm::make_vec4(getColor().toLinear().ptr());
		glm::vec4 _type = glm::vec4(static_cast<int>(getLightType()), 0.0f, 0.0f, 0.0f);
		glm::vec4 _mode = glm::vec4(static_cast<int>(getLightRenderMode()), 0.0f, 0.0f, 0.0f);
		// Spot angles are full cone angles in degrees in the editor. Shaders need
		// the cosine of each half angle for their light-direction dot product.
		const float innerCos = glm::cos(glm::radians(glm::clamp(innerRadius, 0.0f, 179.0f) * 0.5f));
		const float outerCos = glm::cos(glm::radians(glm::clamp(outerRadius, 0.0f, 179.0f) * 0.5f));
		glm::vec4 _radius = glm::vec4(glm::max(radius, 0.0f), innerCos, outerCos, 0.0f);
		// Retain the existing authoring intensity scale until visual calibration.
		// BRDF normalization and display transfer are now handled separately.
		constexpr float modernDirectScale = 1.53f;
		glm::vec4 _intensity = glm::vec4(intensity * getStyleMultiplier() * modernDirectScale, 0.0f, 0.0f, 0.0f);
		glm::vec4 _bias = glm::vec4(bias, bias * m1, bias * m2, bias * m3);
		glm::vec4 _castShadows = glm::vec4((getCastShadows() && shadows) ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f);

		glm::vec4 _texelSize = glm::vec4(1.0f / getShadowMapSize(), 0.0f, 0.0f, 0.0f);
		glm::vec4 _samplingParams = glm::vec4(
			pcfSamples,
			getLightType() == LightType::Directional ? static_cast<float>(numSplits) : 0.0f,
			1.0f,
			1.0f);

		bgfx::setUniform(Renderer::uLightPosition, glm::value_ptr(_position), 1);
		bgfx::setUniform(Renderer::uLightDirection, glm::value_ptr(_direction), 1);
		bgfx::setUniform(Renderer::uLightType, glm::value_ptr(_type), 1);
		bgfx::setUniform(Renderer::uLightRenderMode, glm::value_ptr(_mode), 1);
		bgfx::setUniform(Renderer::uLightRadius, glm::value_ptr(_radius), 1);
		bgfx::setUniform(Renderer::uLightColor, glm::value_ptr(_color), 1);
		bgfx::setUniform(Renderer::uLightIntensity, glm::value_ptr(_intensity), 1);
		bgfx::setUniform(Renderer::uLightShadowBias, glm::value_ptr(_bias), 1);
		bgfx::setUniform(Renderer::uLightCastShadows, glm::value_ptr(_castShadows), 1);

		bgfx::setUniform(Renderer::uShadowMapTexelSize, glm::value_ptr(_texelSize), 1);
		bgfx::setUniform(Renderer::uShadowSamplingParams, glm::value_ptr(_samplingParams), 1);
		bgfx::setUniform(Renderer::uShadowCascadeSplits, glm::value_ptr(mShadowCascadeSplits), 1);

		const int numShadowMaps = getNumShadowMaps();
		if (numShadowMaps > 0)
		{
			for (int s = 0; s < numShadowMaps; ++s)
				bgfx::setTexture(12 + s, Renderer::uShadowMaps[s], getShadowMap(s));

			// Every SAMPLER2DSHADOW slot must reference a comparison-capable
			// depth texture on D3D11. Reuse the first valid map for unused
			// point/spot/cascade slots; a BGRA8 null texture is not legal here.
			for (int s = numShadowMaps; s < 4; ++s)
				bgfx::setTexture(12 + s, Renderer::uShadowMaps[s], getShadowMap(0));
		}

		bgfx::setUniform(Renderer::uShadowMtx, &mShadowMtx, 4);
		//bgfx::setUniform(Renderer::uInvShadowMtx, &mInvShadowMtx, 4);

		return true;
	}

	void Light::onRender(Camera* camera, int view, uint64_t state, bgfx::ProgramHandle program, std::function<void()> preRenderCallback)
	{
		if (!getGameObject()->getActive())
			return;

		if (!getEnabled())
			return;

		int width = Renderer::getSingleton()->getWidth();
		int height = Renderer::getSingleton()->getHeight();

		glm::mat4x4 viewMatrix = camera->getViewMatrix();
		glm::mat4x4 projMatrix = camera->getProjectionMatrix();

		glm::mat4x4 viewProj = projMatrix * viewMatrix;

		Transform* transform = getGameObject()->getTransform();

		glm::vec3 center = transform->getPosition();

		if (lightType != LightType::Directional)
		{
			if (!camera->getFrustum()->sphereInFrustum(center, radius))
				return;
		}

		if (!submitUniforms())
			return;

		if (preRenderCallback != nullptr)
			preRenderCallback();

		bgfx::setState(state);

		Primitives::screenSpaceQuad();

		bgfx::submit(view, program);
	}
}
