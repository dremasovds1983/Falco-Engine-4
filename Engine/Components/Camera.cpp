#include <cmath>
#include "Camera.h"
#include "../Renderer/TargetSupport.h"

#include <bgfx/bgfx.h>
#include <bx/math.h>
#include <glm/gtc/type_ptr.hpp>
#include <algorithm>

#include "Transform.h"

#include "../Core/Engine.h"
#include "../Core/Debug.h"
#include "../Core/GameObject.h"
#include "../Renderer/Renderer.h"
#include "../Renderer/RenderTexture.h"
#include "../Components/Renderable.h"
#include "../Components/DecalRenderer.h"
#include "../Components/Light.h"
#include "../Components/MonoScript.h"
#include "../Math/Mathf.h"
#include "../Math/Plane.h"
#include "../Core/APIManager.h"
#include "../Renderer/Frustum.h"
#include "../Renderer/Primitives.h"
#include "../Core/Time.h"
#include "../Assets/Texture.h"
#include "../Assets/Shader.h"

namespace GX
{
	namespace
	{
		glm::mat4x4 makeCameraPerspective(float fovyDegrees, float aspect, float znear, float zfar)
		{
		#ifdef FALCO_MODERN_BGFX
			float projection[16];
			const bgfx::Caps* caps = bgfx::getCaps();
			bx::mtxProj(projection, fovyDegrees, aspect, znear, zfar,
				caps != nullptr && caps->homogeneousDepth, bx::Handedness::Right);
			return glm::make_mat4(projection);
		#else
			return glm::perspectiveRH(fovyDegrees * Mathf::fDeg2Rad, aspect, znear, zfar);
		#endif
		}

		glm::mat4x4 makeCameraOrtho(float left, float right, float bottom, float top, float znear, float zfar)
		{
		#ifdef FALCO_MODERN_BGFX
			float projection[16];
			const bgfx::Caps* caps = bgfx::getCaps();
			bx::mtxOrtho(projection, left, right, bottom, top, znear, zfar, 0.0f,
				caps != nullptr && caps->homogeneousDepth, bx::Handedness::Right);
			return glm::make_mat4(projection);
		#else
			return glm::orthoRH(left, right, bottom, top, znear, zfar);
		#endif
		}
	}

	std::string Camera::COMPONENT_TYPE = "Camera";
	float Camera::dynAutoRes = 1.0f;
	int Camera::dynAutoResFrames = 1.0f;
	int Camera::dynAutoResFramesToUpdate = 60;

	Camera* Camera::apiCamera = nullptr;
	int Camera::apiViewId = 0;
    int Camera::apiViewEnd = 0;

	std::string Camera::getComponentType()
	{
		return COMPONENT_TYPE;
	}

	Camera::Camera() : Component(APIManager::getSingleton()->camera_class)
	{
		frustum = new Frustum();
	}

	Camera::~Camera()
	{
		delete frustum;
		frustum = nullptr;
	}

	void Camera::initResources()
	{
		int width = Renderer::getSingleton()->getWidth() * viewport_w;
		int height = Renderer::getSingleton()->getHeight() * viewport_h;
		if (renderTarget != nullptr)
		{
			width = renderTarget->getWidth() * viewport_w;
			height = renderTarget->getHeight() * viewport_h;
		}
		if (width < 8) width = 8;
		if (height < 8) height = 8;

		// Intermediate radiance remains HDR through opaque, water and post effects.
		// Presentation/editor targets retain their explicitly selected output format.
		backBuffer = new RenderTexture(width, height, RenderTexture::TextureType::ColorWithDepth, bgfx::TextureFormat::RGBA16F);
		sceneBuffer = new RenderTexture(width, height, RenderTexture::TextureType::ColorWithDepth, bgfx::TextureFormat::RGBA16F);
		presentationBuffer = new RenderTexture(width, height, RenderTexture::TextureType::ColorOnly);

		gbDiffuseTexture = new Texture();
		gbNormalsTexture = new Texture();
		gbMRASTexture = new Texture();
		gbLightmapTexture = new Texture();
		gbDepthTexture = new Texture();
		
		resetFrameBuffers();
	}

	void Camera::destroyResources()
	{
		if (backBuffer != nullptr)
			delete backBuffer;

		if (sceneBuffer != nullptr)
			delete sceneBuffer;

		if (gbDiffuseTexture != nullptr)
			delete gbDiffuseTexture;

		if (gbNormalsTexture != nullptr)
			delete gbNormalsTexture;

		if (gbMRASTexture != nullptr)
			delete gbMRASTexture;

		if (gbLightmapTexture != nullptr)
			delete gbLightmapTexture;

		if (gbDepthTexture != nullptr)
			delete gbDepthTexture;

		backBuffer = nullptr;
		sceneBuffer = nullptr;
		delete presentationBuffer;
		presentationBuffer = nullptr;
        for (auto& buffer : bloomBuffers) { delete buffer; buffer = nullptr; }
		gbDiffuseTexture = nullptr;
		gbNormalsTexture = nullptr;
		gbMRASTexture = nullptr;
		gbLightmapTexture = nullptr;
		gbDepthTexture = nullptr;
		
		destroyFrameBuffers();
	}

	bool Camera::hasValidRenderResources()
	{
		if (backBuffer == nullptr || sceneBuffer == nullptr || presentationBuffer == nullptr)
			return false;
		if (!bgfx::isValid(backBuffer->getFrameBufferHandle())
			|| !bgfx::isValid(backBuffer->getColorTextureHandle())
			|| !bgfx::isValid(backBuffer->getDepthTextureHandle())
			|| !bgfx::isValid(sceneBuffer->getFrameBufferHandle())
			|| !bgfx::isValid(sceneBuffer->getColorTextureHandle())
			|| !bgfx::isValid(sceneBuffer->getDepthTextureHandle())
			|| !bgfx::isValid(presentationBuffer->getFrameBufferHandle())
			|| !bgfx::isValid(presentationBuffer->getColorTextureHandle()))
			return false;

		if (editorCamera && (!bgfx::isValid(outlineBuffer)
			|| !bgfx::isValid(outlineBufferTex)
			|| !bgfx::isValid(outlineDepthTex)))
			return false;

		if (!bgfx::isValid(gbuffer) || !bgfx::isValid(decalBuffer[0]) || !bgfx::isValid(decalBuffer[1])
			|| !bgfx::isValid(lightBuffer) || !bgfx::isValid(lightBufferTex))
			return false;

		for (bgfx::TextureHandle handle : gbufferTex)
			if (!bgfx::isValid(handle))
				return false;
		for (const auto& bufferTextures : decalBufferTex)
			for (bgfx::TextureHandle handle : bufferTextures)
				if (!bgfx::isValid(handle))
					return false;

		return true;
	}

	void Camera::onAttach()
	{
		Component::onAttach();

		transform = getGameObject()->getTransform();

		std::vector<Camera*>& cameras = Renderer::getSingleton()->getCameras();

		cameras.push_back(this);

		if (Engine::getSingleton()->getIsRuntimeMode() || cameras.size() == 1)
			initResources();
	}

	void Camera::onDetach()
	{
		Component::onDetach();

		std::vector<Camera*>& cameras = Renderer::getSingleton()->getCameras();
		auto it = std::find(cameras.begin(), cameras.end(), this);

		if (it != cameras.end())
		{
			cameras.erase(it);
			destroyResources();
		}
	}

	Component* Camera::onClone()
	{
		Camera* newComponent = new Camera();
		newComponent->fovy = fovy;
		newComponent->znear = znear;
		newComponent->zfar = zfar;
		newComponent->viewport_l = viewport_l;
		newComponent->viewport_t = viewport_t;
		newComponent->viewport_w = viewport_w;
		newComponent->viewport_h = viewport_h;
		newComponent->screenOffset_left = screenOffset_left;
		newComponent->screenOffset_top = screenOffset_top;
		newComponent->depth = depth;
		newComponent->clearColor = clearColor;
		newComponent->renderTarget = renderTarget;
		newComponent->projectionType = projectionType;
		newComponent->orthographicSize = orthographicSize;
		newComponent->cullingMask = cullingMask;
		newComponent->clearFlags = clearFlags;

		return newComponent;
	}

	void Camera::setFOVy(float value)
	{
		fovy = value;
		if (fovy < 1.0f) fovy = 1.0f;
		if (fovy > 179.0f) fovy = 179.0f;
	}

	void Camera::setNear(float value)
	{
		znear = value;

		if (znear < 0.0f)
			znear = 0.0f;

		prevTransform = glm::identity<glm::mat4x4>();
	}

	void Camera::setFar(float value)
	{
		zfar = value;

		if (zfar > 10000.0f)
			zfar = 10000.0f;

		if (zfar < 1.0f)
			zfar = 1.0f;

		prevTransform = glm::identity<glm::mat4x4>();
	}

	Transform* Camera::getTransform()
	{
		return transform;
	}

	glm::mat4x4 Camera::getViewMatrix()
	{
		if (!injectView)
		{
			glm::vec3 eye = transform->getPosition();
			glm::vec3 at = eye + transform->getForward();

			glm::mat4 view = glm::identity<glm::mat4x4>();
			glm::vec3 up = glm::cross(transform->getRight(), transform->getForward());

			view = glm::lookAtRH(eye, at, -up);

			return view;
		}
		else
			return injectedViewMatrix;
	}

	glm::mat4x4 Camera::getProjectionMatrix(ProjectionType type)
	{
		if (type == ProjectionType::Perspective)
		{
			float aspect = getAspectRatio();
			glm::mat4 proj = glm::identity<glm::mat4x4>();

			proj = makeCameraPerspective(fovy, aspect, znear, zfar);

			if (flipH || flipV)
				proj = glm::scale(proj, glm::vec3(flipH ? -1 : 1, flipV ? -1 : 1, 1));

			return proj;
		}
		else
		{
			glm::mat4 proj = glm::identity<glm::mat4x4>();
			float aspect = getAspectRatio();

			proj = makeCameraOrtho(-orthographicSize * aspect, orthographicSize * aspect, -orthographicSize, orthographicSize, znear, zfar);

			if (flipH || flipV)
				proj = glm::scale(proj, glm::vec3(flipH ? -1 : 1, flipV ? -1 : 1, 1));

			return proj;
		}
	}

	glm::mat4x4 Camera::getProjectionMatrix()
	{
		if (!injectProj)
		{
			return getProjectionMatrix(projectionType);
		}
		else
			return injectedProjectionMatrix;
	}

	glm::mat4x4 Camera::makeViewMatrix(glm::vec3 position, glm::vec3 lookAt, glm::vec3 up)
	{
		glm::vec3 eye = position;
		glm::vec3 at = lookAt;

		glm::mat4 view = glm::identity<glm::mat4x4>();

		view = glm::lookAtRH(eye, lookAt, up);

		return view;
	}

	glm::mat4x4 Camera::makeProjectionMatrix(float fovy, float aspect, float znear, float zfar)
	{
		glm::mat4 proj = glm::identity<glm::mat4x4>();
		proj = makeCameraPerspective(fovy, aspect, znear, zfar);

		return proj;
	}

	void Camera::injectViewMatrix(glm::mat4x4 mtx)
	{
		injectView = true;
		injectedViewMatrix = mtx;
	}

	void Camera::injectProjectionMatrix(glm::mat4x4 mtx)
	{
		injectProj = true;
		injectedProjectionMatrix = mtx;
	}

	float Camera::getAspectRatio()
	{
		float w = (float)Renderer::getSingleton()->getWidth();
		float h = (float)Renderer::getSingleton()->getHeight();

		if (renderTarget != nullptr)
		{
			w = (float)renderTarget->getWidth();
			h = (float)renderTarget->getHeight();
		}
		else if (backBuffer != nullptr)
		{
			w = (float)backBuffer->getWidth();
			h = (float)backBuffer->getHeight();
		}

		return (w * viewport_w) / (h * viewport_h);
	}

	void Camera::setRenderTarget(RenderTexture* rt, int layer)
	{
		renderTarget = rt;
	}

	Ray Camera::getCameraToViewportRay(float screenX, float screenY)
	{
		float width1 = Renderer::getSingleton()->getWidth();
		float height1 = Renderer::getSingleton()->getHeight();

		if (renderTarget != nullptr)
		{
			width1 = renderTarget->getWidth();
			height1 = renderTarget->getHeight();
		}
		else if (backBuffer != nullptr)
		{
			width1 = backBuffer->getWidth();
			height1 = backBuffer->getHeight();
		}

		float width = width1 * viewport_w;
		float height = height1 * viewport_h;

		float mx = (screenX - width1 * screenOffset_left);
		float my = (screenY - height1 * screenOffset_top);

		glm::mat4x4 mViewProjInverse;
		mViewProjInverse = glm::inverse(getProjectionMatrix() * getViewMatrix());

		float mox = ((mx - 0) / width) * 2.f - 1.f;
		//float moy = ((my - 0) / height) * 2.f - 1.f;
		float moy = (1.f - ((my - 0) / height)) * 2.f - 1.f;

		glm::vec4 rayOrigin = mViewProjInverse * glm::vec4(mox, moy, 0.f, 1.f);
		rayOrigin *= 1.f / rayOrigin.w;
		glm::vec4 rayEnd = mViewProjInverse * glm::vec4(mox, moy, 1.f - FLT_EPSILON, 1.f);
		rayEnd *= 1.f / rayEnd.w;
		glm::vec4 rayDir = glm::normalize(rayEnd - rayOrigin);

		return Ray(rayOrigin, rayDir);
	}

	void Camera::setProjectionType(ProjectionType value)
	{
		projectionType = value;
		prevTransform = glm::identity<glm::mat4x4>();
	}

	void Camera::setOrthographicSize(float value)
	{
		orthographicSize = value;
		prevTransform = glm::identity<glm::mat4x4>();
	}

	float Camera::getDynamicResolution()
	{
		ProjectSettings* settings = Engine::getSingleton()->getSettings();
		if (settings->getUseDynamicResolution())
		{
			if (settings->getAutoDynamicResolution())
				return dynAutoRes;
			else
				return settings->getDynamicResolution();
		}
		else
			return 1.0f;
	}

	void Camera::updateDynamicResolution()
	{
		int fps = Time::getFramesPerSecond();
		ProjectSettings* settings = Engine::getSingleton()->getSettings();
		if (!settings->getUseDynamicResolution())
		{
			dynAutoRes = 1.0f;
			return;
		}

		if (!settings->getAutoDynamicResolution())
		{
			dynAutoRes = 1.0f;
			return;
		}

		if (Time::getTimeSinceLevelStart() > 1.0f)
		{
			int targetFPS = settings->getAutoDynamicResolutionTargetFPS();

			if (fps < targetFPS)
			{
				if (dynAutoRes >= 0.5f)
				{
					if (dynAutoResFrames < dynAutoResFramesToUpdate)
					{
						++dynAutoResFrames;
					}
					else
					{
						dynAutoResFrames = 0;
						dynAutoRes -= 0.05f;
						if (dynAutoRes < 0.5f)
							dynAutoRes = 0.5f;

						Renderer::getSingleton()->resetFrameBuffers();
					}
				}
			}

			if (fps > targetFPS + 30)
			{
				if (dynAutoRes <= 0.95)
				{
					if (dynAutoResFrames < dynAutoResFramesToUpdate)
					{
						++dynAutoResFrames;
					}
					else
					{
						dynAutoResFrames = 0;
						dynAutoRes += 0.05f;
						if (dynAutoRes > 1.0f)
							dynAutoRes = 1.0f;

						Renderer::getSingleton()->resetFrameBuffers();
					}
				}
			}
		}
	}

	void Camera::destroyFrameBuffers()
	{
        releaseOwnedTarget(outlineBuffer, {outlineBufferTex, outlineDepthTex});
        releaseOwnedTarget(gbuffer, {gbufferTex[0], gbufferTex[1], gbufferTex[2], gbufferTex[3], gbufferTex[4]});
        for (int i = 0; i < 2; ++i)
            releaseOwnedTarget(decalBuffer[i], {decalBufferTex[i][0], decalBufferTex[i][1], decalBufferTex[i][2], decalBufferTex[i][3]});
        releaseOwnedTarget(lightBuffer, {lightBufferTex});

		gbufferTex[0].idx = bgfx::kInvalidHandle;
		gbufferTex[1].idx = bgfx::kInvalidHandle;
		gbufferTex[2].idx = bgfx::kInvalidHandle;
		gbufferTex[3].idx = bgfx::kInvalidHandle;
		gbufferTex[4].idx = bgfx::kInvalidHandle;
		for (auto& bufferTextures : decalBufferTex)
			for (bgfx::TextureHandle& handle : bufferTextures)
				handle.idx = bgfx::kInvalidHandle;

		outlineBufferTex.idx = bgfx::kInvalidHandle;
		outlineDepthTex.idx = bgfx::kInvalidHandle;
		lightBufferTex.idx = bgfx::kInvalidHandle;

		outlineBuffer.idx = bgfx::kInvalidHandle;
		gbuffer.idx = bgfx::kInvalidHandle;
		decalBuffer[0].idx = bgfx::kInvalidHandle;
		decalBuffer[1].idx = bgfx::kInvalidHandle;
		lightBuffer.idx = bgfx::kInvalidHandle;
        // Wrappers do not own these attachments and must not expose retired handles.
        if (gbDiffuseTexture) gbDiffuseTexture->textureHandle = BGFX_INVALID_HANDLE;
        if (gbNormalsTexture) gbNormalsTexture->textureHandle = BGFX_INVALID_HANDLE;
        if (gbMRASTexture) gbMRASTexture->textureHandle = BGFX_INVALID_HANDLE;
        if (gbLightmapTexture) gbLightmapTexture->textureHandle = BGFX_INVALID_HANDLE;
        if (gbDepthTexture) gbDepthTexture->textureHandle = BGFX_INVALID_HANDLE;

	}

	void Camera::resetFrameBuffers()
	{
		destroyFrameBuffers();

		float dynRes = getDynamicResolution();

		int _w = Renderer::getSingleton()->getWidth();
		int _h = Renderer::getSingleton()->getHeight();

		if (renderTarget != nullptr)
		{
			_w = renderTarget->getWidth();
			_h = renderTarget->getHeight();
		}

		int width = _w * dynRes;
		int height = _h * dynRes;

		if (width < 8) width = 8;
		if (height < 8) height = 8;

        if (!supportsSceneTargets(width, height)) return;

		if (editorCamera)
		{
			outlineBufferTex = bgfx::createTexture2D(uint16_t(width), uint16_t(height), false, 1, bgfx::TextureFormat::BGRA8, BGFX_TEXTURE_RT
				| BGFX_SAMPLER_MIN_POINT
				| BGFX_SAMPLER_MAG_POINT
				| BGFX_SAMPLER_MIP_POINT
				| BGFX_SAMPLER_U_CLAMP
				| BGFX_SAMPLER_V_CLAMP);
			outlineDepthTex = bgfx::createTexture2D(uint16_t(width), uint16_t(height), false, 1, bgfx::TextureFormat::D24S8, BGFX_TEXTURE_RT
				| BGFX_SAMPLER_MIN_POINT
				| BGFX_SAMPLER_MAG_POINT
				| BGFX_SAMPLER_MIP_POINT
				| BGFX_SAMPLER_U_CLAMP
				| BGFX_SAMPLER_V_CLAMP);

			outlineBuffer = createOwnedTarget({outlineBufferTex, outlineDepthTex});
		}

		////

		if (backBuffer->getWidth() != width || backBuffer->getHeight() != height)
			backBuffer->reset(width, height);
		if (sceneBuffer->getWidth() != width || sceneBuffer->getHeight() != height)
			sceneBuffer->reset(width, height);
		if (presentationBuffer->getWidth() != width || presentationBuffer->getHeight() != height)
			presentationBuffer->reset(width, height);

		//--------GBuffer--------//

		const uint64_t tsFlags = 0
			| BGFX_SAMPLER_MIN_ANISOTROPIC
			| BGFX_SAMPLER_MAG_ANISOTROPIC
			//| BGFX_SAMPLER_MIP_POINT
			| BGFX_SAMPLER_U_CLAMP
			| BGFX_SAMPLER_V_CLAMP
			;

		const uint64_t tsFlagsD = 0
			| BGFX_SAMPLER_MIN_POINT
			| BGFX_SAMPLER_MAG_POINT
			| BGFX_SAMPLER_MIP_POINT
			| BGFX_SAMPLER_U_CLAMP
			| BGFX_SAMPLER_V_CLAMP
			;


		//GBuffer
		gbufferTex[0] = bgfx::createTexture2D(uint16_t(width), uint16_t(height), false, 1, bgfx::TextureFormat::RGBA16F, BGFX_TEXTURE_RT | tsFlags);
		gbufferTex[1] = bgfx::createTexture2D(uint16_t(width), uint16_t(height), false, 1, bgfx::TextureFormat::BGRA8, BGFX_TEXTURE_RT | tsFlags);
		gbufferTex[2] = bgfx::createTexture2D(uint16_t(width), uint16_t(height), false, 1, bgfx::TextureFormat::BGRA8, BGFX_TEXTURE_RT | tsFlags);
		gbufferTex[3] = bgfx::createTexture2D(uint16_t(width), uint16_t(height), false, 1, bgfx::TextureFormat::BGRA8, BGFX_TEXTURE_RT | tsFlags);
		gbufferTex[4] = bgfx::createTexture2D(uint16_t(width), uint16_t(height), false, 1, bgfx::TextureFormat::D24S8, BGFX_TEXTURE_RT | tsFlagsD);

		gbuffer = createOwnedTarget({gbufferTex[0], gbufferTex[1], gbufferTex[2], gbufferTex[3], gbufferTex[4]});

		// Decals must not sample the same attachments they are writing to. Keep
		// two color G-buffers so every decal can read the fully composed result of
		// the preceding decal and write into the other buffer.
		for (uint8_t buffer = 0; buffer < 2; ++buffer)
		{

			for (uint8_t attachment = 0; attachment < 4; ++attachment)
			{
				decalBufferTex[buffer][attachment] = bgfx::createTexture2D(
					uint16_t(width), uint16_t(height), false, 1,
                    attachment == 0 ? bgfx::TextureFormat::RGBA16F : bgfx::TextureFormat::BGRA8,
					BGFX_TEXTURE_RT | tsFlags);
			}

			decalBuffer[buffer] = createOwnedTarget({decalBufferTex[buffer][0], decalBufferTex[buffer][1], decalBufferTex[buffer][2], decalBufferTex[buffer][3]});
		}

		//Light buffer
		lightBufferTex = bgfx::createTexture2D(uint16_t(width), uint16_t(height), false, 1, bgfx::TextureFormat::RGBA16F, BGFX_TEXTURE_RT | tsFlags);
		lightBuffer = createOwnedTarget({lightBufferTex});

        if (!bgfx::isValid(gbuffer) || !bgfx::isValid(decalBuffer[0]) || !bgfx::isValid(decalBuffer[1])
            || !bgfx::isValid(lightBuffer) || (editorCamera && !bgfx::isValid(outlineBuffer)))
        {
            Debug::logError("Camera HDR target group allocation failed");
            destroyFrameBuffers();
            return;
        }

		gbDiffuseTexture->textureHandle = gbufferTex[0]; gbDiffuseTexture->format = bgfx::TextureFormat::RGBA16F;
		gbNormalsTexture->textureHandle = gbufferTex[1]; gbNormalsTexture->format = bgfx::TextureFormat::BGRA8;
		gbMRASTexture->textureHandle = gbufferTex[2]; gbMRASTexture->format = bgfx::TextureFormat::BGRA8;
		gbLightmapTexture->textureHandle = gbufferTex[3]; gbLightmapTexture->format = bgfx::TextureFormat::BGRA8;
		gbDepthTexture->textureHandle = gbufferTex[4]; gbDepthTexture->format = bgfx::TextureFormat::D24S8;

		gbDiffuseTexture->width = width; gbDiffuseTexture->originalWidth = width;
		gbDiffuseTexture->height = height; gbDiffuseTexture->originalHeight = height;

		gbNormalsTexture->width = width; gbNormalsTexture->originalWidth = width;
		gbNormalsTexture->height = height; gbNormalsTexture->originalHeight = height;

		gbMRASTexture->width = width; gbMRASTexture->originalWidth = width;
		gbMRASTexture->height = height; gbMRASTexture->originalHeight = height;

		gbLightmapTexture->width = width; gbLightmapTexture->originalWidth = width;
		gbLightmapTexture->height = height; gbLightmapTexture->originalHeight = height;

		gbDepthTexture->width = width; gbDepthTexture->originalWidth = width;
		gbDepthTexture->height = height; gbDepthTexture->originalHeight = height;

		//--------------------------------//
	}

	glm::vec3 Camera::worldToScreenPoint(glm::vec3 point)
	{
		float width = (float)backBuffer->getWidth();
		float height = (float)backBuffer->getHeight();

		glm::vec4 spPoint = getProjectionMatrix() * (getViewMatrix() * glm::vec4(point, 1.0f));

		bool isInFrustum =
			(spPoint.x < -1.0f) ||
			(spPoint.x > 1.0f) ||
			(spPoint.y < -1.0f) ||
			(spPoint.y > 1.0f);

		Plane cameraPlane = Plane(transform->getForward(), transform->getPosition());
		if (cameraPlane.getSide(point) == Plane::NEGATIVE_SIDE)
			isInFrustum = false;

		glm::vec3 spoint = glm::vec3(spPoint) / spPoint.w;
		
		glm::vec3 screenSpacePoint = glm::vec3(0);
		screenSpacePoint.x = ((spoint.x * 0.5f) + 0.5f) * width;
		screenSpacePoint.y = height - (((spoint.y * 0.5f) + 0.5f) * height);
		screenSpacePoint.z = isInFrustum ? 1.0f : -1.0f;

		return screenSpacePoint;
	}

	glm::vec3 Camera::screenToWorldPoint(glm::vec3 point)
	{
		float width = (float)backBuffer->getWidth();
		float height = (float)backBuffer->getHeight();

		float scrx = point.x / width;
		float scry = point.y / height;
		Ray ray = getCameraToViewportRay((float)(scrx), (float)(scry));
		glm::vec3 vect = ray.origin + ray.direction * point.z;

		return vect;
	}

	Frustum* Camera::getFrustum()
	{
		glm::mat4x4 mtx = transform->getTransformMatrix();
		if (mtx != prevTransform)
		{
			glm::mat4x4 view = getViewMatrix();
			glm::mat4x4 proj = getProjectionMatrix();
			frustum->calculateFrustum(view, proj);
			prevTransform = mtx;
		}

		return frustum;
	}

	bool Camera::isVisible(AxisAlignedBox aab, float bias)
	{
		if (aab.isNull())
			return false;

		Frustum* cullFrustum = getFrustum();
		if (!aab.isInfinite())
		{
			if (!cullFrustum->sphereInFrustum(aab.getCenter(), aab.getRadius() * bias))
				return false;
		}

		return true;
	}

	void Camera::renderFrame(int viewOffset)
	{
		if (!getEnabled())
			return;

		if (gameObject == nullptr || !gameObject->getActive())
			return;

        if (viewOffset < 0 || viewOffset + CAMERA_VIEW_GROUP_SIZE > RENDER_FIRST_SERVICE_PASS_ID)
        {
            static bool warned = false;
            if (!warned) { Debug::logWarning("Camera view budget exceeded; camera skipped."); warned = true; }
            return;
        }

		Renderer* renderer = Renderer::getSingleton();

		// Camera passes must keep stable view IDs when lights toggle shadows.
		// Reserve the complete shadow range for every camera group instead of
		// shifting all downstream passes by the current active-light count.
		int viewLayer = MAX_LIGHT_VIEW_SLOTS + viewOffset;

		bgfx::touch(RENDER_SKYBOX_PASS_ID + viewLayer);
		bgfx::touch(RENDER_SCENE_PASS_ID + viewLayer);

		if (getIsEditorCamera())
		{
			bgfx::touch(RENDER_OVERLAY_PASS_ID + viewLayer);
		}

		bgfx::touch(RENDER_FINAL_PASS_ID + viewLayer);

		//*
		bgfx::touch(RENDER_GEOMETRY_PASS_ID + viewLayer);
		bgfx::touch(RENDER_FORWARD_PASS_ID + viewLayer);
		//*

		bgfx::setViewMode(RENDER_FORWARD_PASS_ID + viewLayer, bgfx::ViewMode::Sequential);

		float left_scale = getViewportLeft();
		float top_scale = getViewportTop();
		float width_scale = getViewportWidth();
		float height_scale = getViewportHeight();

		float _width = Renderer::getSingleton()->getWidth();
		float _height = Renderer::getSingleton()->getHeight();

		if (getRenderTarget() != nullptr)
		{
			_width = getRenderTarget()->getWidth();
			_height = getRenderTarget()->getHeight();
		}

		//-->Render 3D begin
		float viewLeft = _width * left_scale;
		float viewRight = _width * width_scale;
		float viewTop = _height * top_scale;
		float viewBottom = _height * height_scale;

		glm::mat4x4 view = getViewMatrix();
		glm::mat4x4 invView = glm::inverse(view);
		glm::mat4x4 proj = getProjectionMatrix();

		glm::mat4x4 projPerspective = getProjectionMatrix(ProjectionType::Perspective);

		Color camColor = getClearColor();
        const Color linearClearColor = camColor.toLinear();
		//camColor[3] = 0.0f;

		if (getIsEditorCamera())
		{
			bgfx::setViewClear(RENDER_SKYBOX_PASS_ID + viewLayer, 0 | BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH | BGFX_CLEAR_STENCIL, Color::packRGBA(linearClearColor), 1.0f, 0);
			bgfx::setViewRect(RENDER_SKYBOX_PASS_ID + viewLayer, 0, 0, _width, _height);
			bgfx::setViewFrameBuffer(RENDER_SKYBOX_PASS_ID + viewLayer, sceneBuffer->getFrameBufferHandle());
			bgfx::setViewTransform(RENDER_SKYBOX_PASS_ID + viewLayer, glm::value_ptr(view), glm::value_ptr(projPerspective));

			bgfx::setViewClear(RENDER_SCENE_PASS_ID + viewLayer, 0 | BGFX_CLEAR_DEPTH | BGFX_CLEAR_STENCIL, 0x00000000, 1.0f, 0);
			bgfx::setViewRect(RENDER_SCENE_PASS_ID + viewLayer, 0, 0, _width, _height);
			bgfx::setViewFrameBuffer(RENDER_SCENE_PASS_ID + viewLayer, sceneBuffer->getFrameBufferHandle());
			bgfx::setViewTransform(RENDER_SCENE_PASS_ID + viewLayer, glm::value_ptr(view), glm::value_ptr(proj));

			bgfx::setViewClear(RENDER_FORWARD_PASS_ID + viewLayer, 0, 0x00000000, 1.0f, 0);
			bgfx::setViewRect(RENDER_FORWARD_PASS_ID + viewLayer, 0, 0, _width, _height);
			bgfx::setViewFrameBuffer(RENDER_FORWARD_PASS_ID + viewLayer, sceneBuffer->getFrameBufferHandle());
			bgfx::setViewTransform(RENDER_FORWARD_PASS_ID + viewLayer, glm::value_ptr(view), glm::value_ptr(proj));

			bgfx::setViewClear(RENDER_OVERLAY_PASS_ID + viewLayer, 0, 0x00000000, 1.0f, 0);
			bgfx::setViewRect(RENDER_OVERLAY_PASS_ID + viewLayer, 0, 0, _width, _height);
			bgfx::setViewFrameBuffer(RENDER_OVERLAY_PASS_ID + viewLayer, sceneBuffer->getFrameBufferHandle());
			bgfx::setViewTransform(RENDER_OVERLAY_PASS_ID + viewLayer, glm::value_ptr(view), glm::value_ptr(proj));

			bgfx::setViewClear(RENDER_OUTLINE_PASS_ID + viewLayer, 0 | BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, 0x00000000, 1.0f, 0);
			bgfx::setViewRect(RENDER_OUTLINE_PASS_ID + viewLayer, 0, 0, _width, _height);
			bgfx::setViewTransform(RENDER_OUTLINE_PASS_ID + viewLayer, glm::value_ptr(view), glm::value_ptr(proj));
			bgfx::setViewFrameBuffer(RENDER_OUTLINE_PASS_ID + viewLayer, outlineBuffer);
			// Submit the empty draw only after the outline view is fully configured.
			// This guarantees that a frame without a selected mesh still clears the
			// previous selection mask before OutlineFinal samples it.
			bgfx::touch(RENDER_OUTLINE_PASS_ID + viewLayer);

			glm::mat4x4 finalProj = makeCameraOrtho(0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 100.0f);
			bgfx::setViewClear(RENDER_FINAL_PASS_ID + viewLayer, 0 | BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH | BGFX_CLEAR_STENCIL, Color::packRGBA(camColor), 1.0f, 0);
			bgfx::setViewRect(RENDER_FINAL_PASS_ID + viewLayer, viewLeft, viewTop, viewRight, viewBottom);
			bgfx::setViewTransform(RENDER_FINAL_PASS_ID + viewLayer, NULL, glm::value_ptr(finalProj));
			if (getRenderTarget() != nullptr)
				bgfx::setViewFrameBuffer(RENDER_FINAL_PASS_ID + viewLayer, getRenderTarget()->getFrameBufferHandle());
			else if (renderer->backBuffer != nullptr)
				bgfx::setViewFrameBuffer(RENDER_FINAL_PASS_ID + viewLayer, renderer->backBuffer->getFrameBufferHandle());
		}
		else
		{
			uint16_t cullState = 0 | BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH | BGFX_CLEAR_STENCIL;
			uint32_t clearColor = Color::packRGBA(linearClearColor);
			if (getClearFlags() == Camera::ClearFlags::DepthOnly)
				clearColor = 0x00000000;
			if (getClearFlags() == Camera::ClearFlags::SolidColor)
				cullState = 0 | BGFX_CLEAR_COLOR | BGFX_CLEAR_STENCIL;
			if (getClearFlags() == Camera::ClearFlags::DontClear)
				cullState = 0;

			bgfx::setViewClear(RENDER_SKYBOX_PASS_ID + viewLayer, 0 | BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH | BGFX_CLEAR_STENCIL, clearColor, 1.0f, 0);
			bgfx::setViewRect(RENDER_SKYBOX_PASS_ID + viewLayer, 0, 0, _width, _height);
			bgfx::setViewTransform(RENDER_SKYBOX_PASS_ID + viewLayer, glm::value_ptr(view), glm::value_ptr(projPerspective));
			bgfx::setViewFrameBuffer(RENDER_SKYBOX_PASS_ID + viewLayer, sceneBuffer->getFrameBufferHandle());

			cullState = 0 | BGFX_CLEAR_DEPTH | BGFX_CLEAR_STENCIL;
			if (getClearFlags() == Camera::ClearFlags::SolidColor)
				cullState = 0 | BGFX_CLEAR_COLOR | BGFX_CLEAR_STENCIL;
			if (getClearFlags() == Camera::ClearFlags::DontClear)
				cullState = 0;

			bgfx::setViewClear(RENDER_SCENE_PASS_ID + viewLayer, cullState, 0x00000000, 1.0f, 0);
			bgfx::setViewRect(RENDER_SCENE_PASS_ID + viewLayer, 0, 0, _width, _height);
			bgfx::setViewTransform(RENDER_SCENE_PASS_ID + viewLayer, glm::value_ptr(view), glm::value_ptr(proj));
			bgfx::setViewFrameBuffer(RENDER_SCENE_PASS_ID + viewLayer, sceneBuffer->getFrameBufferHandle());

			bgfx::setViewClear(RENDER_FORWARD_PASS_ID + viewLayer, 0, 0x00000000, 1.0f, 0);
			bgfx::setViewRect(RENDER_FORWARD_PASS_ID + viewLayer, 0, 0, _width, _height);
			bgfx::setViewTransform(RENDER_FORWARD_PASS_ID + viewLayer, glm::value_ptr(view), glm::value_ptr(proj));
			bgfx::setViewFrameBuffer(RENDER_FORWARD_PASS_ID + viewLayer, sceneBuffer->getFrameBufferHandle());

			glm::mat4x4 finalProj = makeCameraOrtho(0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 100.0f);
			bgfx::setViewClear(RENDER_FINAL_PASS_ID + viewLayer, 0, clearColor, 1.0f, 0);
			bgfx::setViewRect(RENDER_FINAL_PASS_ID + viewLayer, viewLeft, viewTop, viewRight, viewBottom);
			bgfx::setViewTransform(RENDER_FINAL_PASS_ID + viewLayer, NULL, glm::value_ptr(finalProj));
			if (getRenderTarget() != nullptr)
				bgfx::setViewFrameBuffer(RENDER_FINAL_PASS_ID + viewLayer, getRenderTarget()->getFrameBufferHandle());
			else if (renderer->backBuffer != nullptr)
				bgfx::setViewFrameBuffer(RENDER_FINAL_PASS_ID + viewLayer, renderer->backBuffer->getFrameBufferHandle());
		}

		//----------------Update occlusion data-------------//

		if (getOcclusionCulling())
		{
			if (renderer->projectSettings != nullptr)
				renderer->calculateVisibility(view, proj, getFrustum(), getFar());
		}

		//----------------Render directional shadows-------------//

		renderer->renderDirectionalLightShadows(this, invView, viewOffset);

		//-------------------------------------------------------//

		const bgfx::TextureHandle* composedGBuffer = gbufferTex;
		//---Render G-Buffer
		if (bgfx::getCaps()->limits.maxFBAttachments > 1 && bgfx::isValid(renderer->combinePH))
		{
			bgfx::setViewClear(RENDER_GEOMETRY_PASS_ID + viewLayer, 0 | BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH | BGFX_CLEAR_STENCIL, 0x00000000, 1.0f, 0);
			// The light buffer has only a color attachment. Clear it after the
			// framebuffer is assigned so additive lighting cannot accumulate
			// across frames on modern bgfx backends.
			bgfx::setViewClear(RENDER_LIGHT_PASS_ID + viewLayer, BGFX_CLEAR_COLOR, 0x00000000, 1.0f, 0);

			bgfx::setViewRect(RENDER_GEOMETRY_PASS_ID + viewLayer, 0, 0, _width, _height);
			bgfx::setViewRect(RENDER_LIGHT_PASS_ID + viewLayer, 0, 0, _width, _height);

			if (gbuffer.idx != bgfx::kInvalidHandle)
			{
				bgfx::setViewFrameBuffer(RENDER_GEOMETRY_PASS_ID + viewLayer, gbuffer);
			}
			if (lightBuffer.idx != bgfx::kInvalidHandle)
			{
				bgfx::setViewFrameBuffer(RENDER_LIGHT_PASS_ID + viewLayer, lightBuffer);
			}

			bgfx::setViewTransform(RENDER_GEOMETRY_PASS_ID + viewLayer, glm::value_ptr(view), glm::value_ptr(proj));

			glm::mat4x4 invViewProj = glm::inverse(proj * view);

			auto& renderables = renderer->getRenderables();

			for (auto it = renderables.begin(); it != renderables.end(); ++it)
			{
				Renderable* comp = *it;

				if (comp->getSkipRendering() || comp->isDecal())
					continue;

				if (!comp->checkCullingMask(getCullingMask()))
					continue;

				if (!comp->isAlwaysVisible())
				{
					if (!isVisible(comp->getBounds()))
						continue;
				}

				comp->onRender(this, RENDER_GEOMETRY_PASS_ID + viewLayer, renderer->getRenderState(this, renderer->defaultRenderState), { bgfx::kInvalidHandle }, static_cast<int>(RenderMode::Deferred), [=]() {
					renderer->setSystemUniforms(this);
					});
			}

			std::vector<DecalRenderer*> deferredDecals;
			deferredDecals.reserve(MAX_DEFERRED_DECALS);

			// Decals are composed in renderable order. Each pass reads the complete
			// output of the preceding pass, so overlaps preserve both materials.
			for (auto it = renderables.begin(); it != renderables.end(); ++it)
			{
				Renderable* comp = *it;

				if (comp->getSkipRendering() || !comp->isDecal())
					continue;

				DecalRenderer* decal = static_cast<DecalRenderer*>(comp);
				if (!decal->canRenderDeferred())
					continue;

				if (!comp->checkCullingMask(getCullingMask()))
					continue;

				if (!comp->isAlwaysVisible())
				{
					if (!isVisible(comp->getBounds()))
						continue;
				}

                deferredDecals.push_back(decal);
			}

            struct DecalDraw
            {
                DecalRenderer* decal;
                int x0, y0, x1, y1;
            };
            const auto overlaps = [](const DecalDraw& a, const DecalDraw& b) {
                return a.x0 < b.x1 && b.x0 < a.x1 && a.y0 < b.y1 && b.y0 < a.y1;
            };
            std::vector<std::vector<DecalDraw>> decalPasses;
            const glm::mat4 cameraVP = proj * view;
            for (auto* decal : deferredDecals)
            {
                DecalDraw draw = {decal, 0, 0, static_cast<int>(_width), static_cast<int>(_height)};
                float minX = 1.0f, minY = 1.0f, maxX = -1.0f, maxY = -1.0f;
                bool fullScreen = bgfx::getCaps()->originBottomLeft;
                for (const auto& corner : decal->getBounds().getAllCorners())
                {
                    const glm::vec4 clip = cameraVP * glm::vec4(corner, 1.0f);
                    if (!std::isfinite(clip.w) || clip.w <= 0.0001f) { fullScreen = true; break; }
                    const float x = clip.x / clip.w, y = clip.y / clip.w;
                    if (!std::isfinite(x) || !std::isfinite(y)) { fullScreen = true; break; }
                    minX = std::min(minX, x); maxX = std::max(maxX, x);
                    minY = std::min(minY, y); maxY = std::max(maxY, y);
                }
                if (!fullScreen)
                {
                    // Scissor uses top-left pixel coordinates. Conservative padding also
                    // keeps touching projectors in separate passes at pixel boundaries.
                    const auto clampNdc = [](float v) { return std::max(-1.0f, std::min(1.0f, v)); };
                    draw.x0 = std::max(0, static_cast<int>(std::floor((clampNdc(minX) * 0.5f + 0.5f) * _width)) - 2);
                    draw.x1 = std::min(static_cast<int>(_width), static_cast<int>(std::ceil((clampNdc(maxX) * 0.5f + 0.5f) * _width)) + 2);
                    draw.y0 = std::max(0, static_cast<int>(std::floor((0.5f - clampNdc(maxY) * 0.5f) * _height)) - 2);
                    draw.y1 = std::min(static_cast<int>(_height), static_cast<int>(std::ceil((0.5f - clampNdc(minY) * 0.5f) * _height)) + 2);
                }
                if (draw.x0 >= draw.x1 || draw.y0 >= draw.y1) continue;
                size_t passIndex = 0;
                for (size_t i = 0; i < decalPasses.size(); ++i)
                    for (const auto& previous : decalPasses[i])
                        if (overlaps(draw, previous)) { passIndex = i + 1; break; }
                if (passIndex >= MAX_DEFERRED_DECALS)
                {
                    static bool warned = false;
                    if (!warned) { Debug::logWarning("Decal overlap pass budget exceeded (200). Reduce overlapping projectors."); warned = true; }
                    continue;
                }
                if (passIndex == decalPasses.size()) decalPasses.emplace_back();
                decalPasses[passIndex].push_back(draw);
            }

            for (size_t passIndex = 0; passIndex < decalPasses.size(); ++passIndex)
            {
                const uint8_t destinationBuffer = static_cast<uint8_t>(passIndex & 1);
                const bgfx::TextureHandle* sourceTextures = passIndex == 0 ? gbufferTex : decalBufferTex[destinationBuffer ^ 1];
                const bgfx::ViewId decalView = static_cast<bgfx::ViewId>(RENDER_DECAL_PASS_ID + passIndex + viewLayer);
                bgfx::setViewClear(decalView, BGFX_CLEAR_NONE, 0x00000000, 1.0f, 0);
                bgfx::setViewRect(decalView, 0, 0, _width, _height);
                bgfx::setViewFrameBuffer(decalView, decalBuffer[destinationBuffer]);
                bgfx::setViewTransform(decalView, glm::value_ptr(view), glm::value_ptr(proj));
                bgfx::setViewMode(decalView, bgfx::ViewMode::Sequential);
                bool first = true;
                for (const auto& draw : decalPasses[passIndex])
                {
                    // The first draw copies through the complete source G-buffer.
                    // Disjoint subsequent decals only replace their own screen regions.
                    draw.decal->onRender(this, decalView, 0, {bgfx::kInvalidHandle}, static_cast<int>(RenderMode::Deferred), [=]() {
                        renderer->setSystemUniforms(this);
                        bgfx::setTexture(0, renderer->u_albedoMap, sourceTextures[0]);
                        bgfx::setTexture(1, renderer->u_normalMap, sourceTextures[1]);
                        bgfx::setTexture(2, renderer->u_mraMap, sourceTextures[2]);
                        bgfx::setTexture(3, renderer->uLightmap, sourceTextures[3]);
                        bgfx::setTexture(4, renderer->u_depthMap, gbufferTex[4]);
                        if (first) bgfx::setScissor(UINT16_MAX);
                        else bgfx::setScissor(static_cast<uint16_t>(draw.x0), static_cast<uint16_t>(draw.y0),
                            static_cast<uint16_t>(draw.x1 - draw.x0), static_cast<uint16_t>(draw.y1 - draw.y0));
                    });
                    first = false;
                }
                composedGBuffer = decalBufferTex[destinationBuffer];
            }

			glm::mat4x4 geomProj = makeCameraOrtho(0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 100.0f);
			bgfx::setViewTransform(RENDER_LIGHT_PASS_ID + viewLayer, NULL, glm::value_ptr(geomProj));
			bgfx::setViewTransform(RENDER_SCENE_PASS_ID + viewLayer, NULL, glm::value_ptr(geomProj));

			// Submit the empty light draw only after clear state, rectangle,
			// framebuffer and transform are all configured for this camera.
			bgfx::touch(RENDER_LIGHT_PASS_ID + viewLayer);

			auto& lights = renderer->getLights();

			for (auto& light : lights)
			{
				//Render light
				light->onRender(this, RENDER_LIGHT_PASS_ID + viewLayer, renderer->lightRenderState, renderer->lightPH, [=]() {
					renderer->setSystemUniforms(this);

					bgfx::setTexture(0, renderer->u_albedoMap, composedGBuffer[0]);
					bgfx::setTexture(1, renderer->u_normalMap, composedGBuffer[1]);
					bgfx::setTexture(2, renderer->u_mraMap, composedGBuffer[2]);
					bgfx::setTexture(3, renderer->uLightmap, composedGBuffer[3]);
					bgfx::setTexture(4, renderer->u_depthMap, gbufferTex[4]);

					bgfx::setUniform(renderer->u_invVP, glm::value_ptr(invViewProj), 1);
				});
			}

			renderer->setSystemUniforms(this);
            renderer->setEnvironmentCacheUniforms();
			bgfx::setTexture(0, renderer->u_albedoMap, composedGBuffer[0]);
			bgfx::setTexture(1, renderer->u_depthMap, gbufferTex[4]);
			bgfx::setTexture(2, renderer->u_lightBuffer, lightBufferTex);
			bgfx::setTexture(3, renderer->uLightmap, composedGBuffer[3]);
			bgfx::setTexture(4, renderer->u_normalMap, composedGBuffer[1]);
			bgfx::setTexture(5, renderer->u_mraMap, composedGBuffer[2]);
			bgfx::setUniform(renderer->u_invVP, glm::value_ptr(invViewProj), 1);
			bgfx::setState(renderer->combineRenderState);
			Primitives::screenSpaceQuad();
			bgfx::submit(RENDER_SCENE_PASS_ID + viewLayer, renderer->combinePH);
		}
		//---

		glm::mat4x4 skyMtx = glm::identity<glm::mat4x4>();
		skyMtx = glm::translate(skyMtx, getTransform()->getPosition());
		skyMtx = glm::scale(skyMtx, glm::vec3(100.0f, 100.0f, 100.0f));

		//-------------Render scene 0-------------//
		renderer->renderObjects(this, viewLayer, 0, view, proj, skyMtx);
		//----------------------------------------//

        auto copyOpaqueScene = [&](int vid)
        {
            if (!bgfx::isValid(renderer->cameraBackBufferPH)) return false;
			glm::mat4x4 finalProj = makeCameraOrtho(0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 100.0f);
			bgfx::setViewClear(vid, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH | BGFX_CLEAR_STENCIL, 0x000000ff, 1.0f, 0);
			bgfx::setViewRect(vid, 0, 0, _width, _height);
			bgfx::setViewTransform(vid, NULL, glm::value_ptr(finalProj));
			bgfx::setViewFrameBuffer(vid, getBackBuffer()->getFrameBufferHandle());

			bgfx::setTexture(0, renderer->u_albedoMap, sceneBuffer->getColorTextureHandle());
			bgfx::setTexture(1, renderer->u_depthMap, sceneBuffer->getDepthTextureHandle());
			renderer->setSystemUniforms(this);
			bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_WRITE_Z | BGFX_STATE_DEPTH_TEST_ALWAYS);
            if (Primitives::screenSpaceQuad())
            {
                bgfx::submit(vid, renderer->cameraBackBufferPH);
                return true;
            }
            bgfx::discard();
            return false;
        };
        const bool opaqueCopySubmitted = copyOpaqueScene(RENDER_BACKBUFFER_PASS_ID + viewLayer);
        bool ssrSubmitted = false;

        // SSR reads the immutable opaque copy and adds only the replacement delta.
        const auto* graphicsSettings = Engine::getSingleton()->getSettings();
        if (opaqueCopySubmitted && graphicsSettings->getSSR() && bgfx::isValid(renderer->ssrPH)
            && bgfx::isValid(renderer->cameraBackBufferPH) && bgfx::isValid(renderer->combinePH))
        {
            const int ssrView = RENDER_SSR_PASS_ID + viewLayer;
            const auto ssrProjection = makeCameraOrtho(0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 100.0f);
            bgfx::setViewRect(ssrView, 0, 0, static_cast<uint16_t>(_width), static_cast<uint16_t>(_height));
            bgfx::setViewClear(ssrView, BGFX_CLEAR_NONE);
            bgfx::setViewTransform(ssrView, nullptr, glm::value_ptr(ssrProjection));
            bgfx::setViewFrameBuffer(ssrView, sceneBuffer->getFrameBufferHandle());
            renderer->setSystemUniforms(this);
            renderer->setEnvironmentCacheUniforms();
            const glm::vec4 ssrParams(graphicsSettings->getSSRDistance(), graphicsSettings->getSSRThickness(), 0.0f, 0.0f);
            bgfx::setUniform(renderer->uSSRParams, glm::value_ptr(ssrParams));
            bgfx::setTexture(0, renderer->u_albedoMap, composedGBuffer[0]);
            bgfx::setTexture(1, renderer->u_depthMap, gbufferTex[4]);
            bgfx::setTexture(2, renderer->u_lightBuffer, backBuffer->getColorTextureHandle());
            bgfx::setTexture(3, renderer->u_forwardDepth, backBuffer->getDepthTextureHandle());
            bgfx::setTexture(4, renderer->u_normalMap, composedGBuffer[1]);
            bgfx::setTexture(5, renderer->u_mraMap, composedGBuffer[2]);
            bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_ONE, BGFX_STATE_BLEND_ONE));
            if (Primitives::screenSpaceQuad())
            {
                bgfx::submit(ssrView, renderer->ssrPH);
                ssrSubmitted = true;
            }
            else bgfx::discard();
        }

        // This later view may overwrite the copy only after SSR finished reading it.
        // Water refraction then sees the same opaque reflections as the visible scene.
        if (ssrSubmitted) copyOpaqueScene(RENDER_REFRACTION_COPY_PASS_ID + viewLayer);

		//-------------Render scene 1-------------//
	#ifdef FALCO_MODERN_BGFX
		// Water samples the completed opaque background, then overlays it without
		// clearing it. Do not submit it into the earlier opaque forward pass.
		const int waterView = RENDER_WATER_PASS_ID + viewLayer;
		bgfx::setViewClear(waterView, 0);
		bgfx::setViewRect(waterView, 0, 0, _width, _height);
		bgfx::setViewTransform(waterView, glm::value_ptr(view), glm::value_ptr(proj));
		bgfx::setViewFrameBuffer(waterView, sceneBuffer->getFrameBufferHandle());
		bgfx::setViewMode(waterView, bgfx::ViewMode::Sequential);
	#endif
		renderer->renderObjects(this, viewLayer, 1, view, proj, skyMtx);
		//----------------------------------------//

		//-------------Render scene 2-------------//
		renderer->renderObjects(this, viewLayer, 2, view, proj, skyMtx);
		//----------------------------------------//

		renderer->renderRenderCallbacks(this, viewLayer);

        // Display conversion is shared by Scene and Play; selection and FXAA follow it.
        auto present = [&](RenderTexture* source)
        {
            if (!bgfx::isValid(renderer->presentationPH) || source == nullptr
                || !bgfx::isValid(source->getColorTextureHandle())) return false;
            const auto* settings = Engine::getSingleton()->getSettings();
            bool bloomReady = settings->getBloom() && settings->getBloomIntensity() > 0.0f && bgfx::isValid(renderer->bloomPH);
            const int bloomWidth = std::max(8, presentationBuffer->getWidth() / settings->getBloomDownsample());
            const int bloomHeight = std::max(8, presentationBuffer->getHeight() / settings->getBloomDownsample());
            if (bloomReady)
            {
                for (auto& buffer : bloomBuffers)
                {
                    if (!buffer) buffer = new RenderTexture(bloomWidth, bloomHeight, RenderTexture::TextureType::ColorOnly, bgfx::TextureFormat::RGBA16F);
                    else if (buffer->getWidth() != bloomWidth || buffer->getHeight() != bloomHeight) buffer->reset(bloomWidth, bloomHeight);
                    bloomReady = bloomReady && bgfx::isValid(buffer->getFrameBufferHandle()) && bgfx::isValid(buffer->getColorTextureHandle());
                }
            }
            const glm::vec4 bloomParams(settings->getBloomThreshold(), std::max(0.00001f, settings->getBloomThreshold() * 0.5f), settings->getBloomIntensity(), 0.0f);
            const auto bloomProjection = makeCameraOrtho(0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 100.0f);
            for (int pass = 0; pass < 3 && bloomReady; ++pass)
            {
                const auto bloomView = RENDER_BLOOM_PASS_ID + pass + viewLayer;
                bgfx::setViewRect(bloomView, 0, 0, bloomWidth, bloomHeight);
                bgfx::setViewClear(bloomView, BGFX_CLEAR_COLOR, 0, 1.0f, 0);
                bgfx::setViewTransform(bloomView, nullptr, glm::value_ptr(bloomProjection));
                bgfx::setViewFrameBuffer(bloomView, bloomBuffers[pass]->getFrameBufferHandle());
                renderer->setSystemUniforms(this);
                const float radius = settings->getBloomRadius();
                const glm::vec4 bloomPass(pass == 0 ? 0.0f : 1.0f,
                    pass == 0 ? 1.0f / bloomWidth : (pass == 1 ? radius / bloomWidth : 0.0f),
                    pass == 0 ? 1.0f / bloomHeight : (pass == 2 ? radius / bloomHeight : 0.0f), 0.0f);
                bgfx::setUniform(renderer->uBloomParams, glm::value_ptr(bloomParams));
                bgfx::setUniform(renderer->uBloomPass, glm::value_ptr(bloomPass));
                // Gaussian paired taps require bilinear sampling, without anisotropic footprint expansion.
                bgfx::setTexture(0, renderer->u_albedoMap, pass == 0 ? source->getColorTextureHandle() : bloomBuffers[pass - 1]->getColorTextureHandle(), BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
                bgfx::setTexture(1, renderer->u_depthMap, sceneBuffer->getDepthTextureHandle());
                bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A);
                if (!Primitives::screenSpaceQuad()) { bgfx::discard(); bloomReady = false; break; }
                bgfx::submit(bloomView, renderer->bloomPH);
            }
            const auto presentationView = RENDER_PRESENTATION_PASS_ID + viewLayer;
            const auto projection = makeCameraOrtho(0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 100.0f);
            bgfx::setViewRect(presentationView, 0, 0, presentationBuffer->getWidth(), presentationBuffer->getHeight());
            bgfx::setViewClear(presentationView, BGFX_CLEAR_COLOR, 0, 1.0f, 0);
            bgfx::setViewTransform(presentationView, nullptr, glm::value_ptr(projection));
            bgfx::setViewFrameBuffer(presentationView, presentationBuffer->getFrameBufferHandle());
            renderer->setSystemUniforms(this);
            const glm::vec4 presentation(settings->getExposure(), float(settings->getTonemap()), settings->getBrightness(), settings->getContrast());
            const glm::vec4 grade(settings->getSaturation(), 0.0f, 0.0f, 0.0f);
            bgfx::setUniform(renderer->uPresentation, glm::value_ptr(presentation));
            bgfx::setUniform(renderer->uColorGrade, glm::value_ptr(grade));
            const glm::vec4 presentationBloom(0.0f, 0.0f, bloomReady ? settings->getBloomIntensity() : 0.0f, 0.0f);
            bgfx::setUniform(renderer->uBloomParams, glm::value_ptr(presentationBloom));
            bgfx::setTexture(2, renderer->uBloomMap, bloomReady ? bloomBuffers[2]->getColorTextureHandle() : source->getColorTextureHandle());
            bgfx::setTexture(0, renderer->u_albedoMap, source->getColorTextureHandle());
            bgfx::setTexture(1, renderer->u_depthMap, sceneBuffer->getDepthTextureHandle());
            bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A);
            if (!Primitives::screenSpaceQuad())
            {
                bgfx::discard();
                return false;
            }
            bgfx::submit(presentationView, renderer->presentationPH);
            return true;
        };

		if (getIsEditorCamera())
		{
			if (renderer->outlineFinalPH.idx != bgfx::kInvalidHandle && present(sceneBuffer))
			{
				renderer->setSystemUniforms(this);
				const float outlineWidth = float(std::max(1, sceneBuffer->getWidth()));
				const float outlineHeight = float(std::max(1, sceneBuffer->getHeight()));
				const glm::vec4 outlineParams(1.0f / outlineWidth, 1.0f / outlineHeight, renderer->selectionOutlineThickness, float(editorDisplayMode));
				bgfx::setUniform(renderer->u_outlineParams, glm::value_ptr(outlineParams));
				bgfx::setUniform(renderer->u_outlineColor, renderer->selectionOutlineColor.ptr());
				// FXAA offsets are measured in source pixels, not destination viewport pixels.
                const glm::vec4 displaySize(float(presentationBuffer->getWidth()), float(presentationBuffer->getHeight()), 0.0f, 0.0f);
                bgfx::setUniform(renderer->uScreenParams, glm::value_ptr(displaySize));
                bgfx::setTexture(0, renderer->u_albedoMap, presentationBuffer->getColorTextureHandle(), BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
				bgfx::setTexture(1, renderer->u_depthMap, sceneBuffer->getDepthTextureHandle());
				bgfx::setTexture(2, renderer->u_forwardColor, outlineBufferTex);
				bgfx::setTexture(3, renderer->u_forwardDepth, outlineDepthTex);
				// Diagnostics read this camera's raw G-buffer, bypassing display FXAA,
				// tone mapping and selection outlines. Game cameras retain normal output.
				bgfx::setTexture(4, renderer->u_normalMap, composedGBuffer[1]);
				bgfx::setTexture(5, renderer->u_mraMap, composedGBuffer[2]);
				if (editorDisplayMode != 0)
				{
					bgfx::setTexture(2, renderer->u_forwardColor, editorDisplayMode == 7 ? lightBufferTex : composedGBuffer[0]);
					bgfx::setTexture(3, renderer->u_forwardDepth, gbufferTex[4]);
				}
				bgfx::setState(renderer->combineRenderState | BGFX_STATE_DEPTH_TEST_ALWAYS);
				Primitives::screenSpaceQuad();
				bgfx::submit(RENDER_FINAL_PASS_ID + viewLayer, renderer->outlineFinalPH);
			}
		}
		else
		{
			if (renderer->fxaaPH.idx != bgfx::kInvalidHandle)
			{
				if (sceneBuffer != nullptr)
				{
					postProcessRt = nullptr;

					apiCamera = this;
					apiViewId = RENDER_POSTPROCESS_PASS_ID + viewLayer;
                    apiViewEnd = apiViewId + MAX_POSTPROCESS_BLITS;

					GameObject* node = getGameObject();
					if (node != nullptr)
					{
						std::vector<MonoScript*> scripts = node->getMonoScripts();
						for (auto scr : scripts)
						{
							if (!scr->getEnabled())
								continue;

							if (!scr->isInitialized())
								continue;

							MonoObject* obj = scr->getManagedObject();

							RenderTexture* srcRt = sceneBuffer;
							if (postProcessRt != nullptr)
								srcRt = postProcessRt;

							void* args[2] = { sceneBuffer->getManagedObject(), srcRt->getManagedObject() };
							APIManager::getSingleton()->execute(obj, "OnRenderImage", args, "RenderTexture,RenderTexture");
						}
					}
				}

                apiCamera = nullptr;
                apiViewId = apiViewEnd = 0;

				RenderTexture* finalRt = sceneBuffer;
				if (postProcessRt != nullptr)
					finalRt = postProcessRt;

				if (present(finalRt))
                {
				renderer->setSystemUniforms(this);
				// FXAA offsets are measured in source pixels, not destination viewport pixels.
                const glm::vec4 displaySize(float(presentationBuffer->getWidth()), float(presentationBuffer->getHeight()), 0.0f, 0.0f);
                bgfx::setUniform(renderer->uScreenParams, glm::value_ptr(displaySize));
                bgfx::setTexture(0, renderer->u_albedoMap, presentationBuffer->getColorTextureHandle(), BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
				bgfx::setTexture(1, renderer->u_depthMap, sceneBuffer->getDepthTextureHandle());
				bgfx::setState(0
					| BGFX_STATE_WRITE_RGB
					| BGFX_STATE_WRITE_A
					| BGFX_STATE_BLEND_ALPHA);
				Primitives::screenSpaceQuad();
				bgfx::submit(RENDER_FINAL_PASS_ID + viewLayer, renderer->fxaaPH);
                }
			}
		}
		//-->Render 3D end

		renderer->collectStats();
	#ifndef FALCO_MODERN_BGFX
		renderer->frame();
	#endif
	}
}
