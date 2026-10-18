#include "Renderer.h"
#include <cmath>

#ifndef _WIN32
#include <string.h>
#endif

#include <imgui.h>
#include <imgui_internal.h>
#include <misc/cpp/imgui_stdlib.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <SDL2/SDL_syswm.h>

#ifndef FREEIMAGE_LIB
#define FALCO_DEFINED_FREEIMAGE_LIB
#define FREEIMAGE_LIB
#endif
#include <FreeImage.h>
#ifdef FALCO_DEFINED_FREEIMAGE_LIB
#undef FREEIMAGE_LIB
#undef FALCO_DEFINED_FREEIMAGE_LIB
#endif

#include <GL/gl.h>

#undef None

#include <bgfx/bgfx.h>
#include <bgfx/platform.h>
#include <bx/bx.h>
#include <bx/timer.h>

#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/euler_angles.hpp>
#include <bx/math.h>
#include <bx/string.h>
#include <bgfx/bgfx.h>

#ifdef FALCO_MODERN_BGFX
#include <cstdlib>
#include <mutex>
#endif

#include "Primitives.h"
#include "RenderTexture.h"
#include "Frustum.h"
#include "VertexLayouts.h"
#include "../Core/Engine.h"
#include "../Core/Debug.h"
#include "../Core/APIManager.h"
#include "../UI/imgui_impl_sdl.h"
#include "../UI/imgui_impl_bgfx.h"
#include "../Classes/IO.h"
#include "../Classes/Helpers.h"
#include "../Classes/StringConverter.h"
#include "../Classes/GUIDGenerator.h"
#include "../Core/GameObject.h"
#include "../Components/Component.h"
#include "../Components/Transform.h"
#include "../Components/Camera.h"
#include "../Components/Renderable.h"
#include "../Components/Light.h"
#include "../Components/CloudSystem.h"
#include "../Components/Canvas.h"
#include "../Components/UIElement.h"
#include "../Components/Mask.h"
#include "../Components/MonoScript.h"
#include "../Components/MeshRenderer.h"
#include "../Assets/Texture.h"
#include "../Assets/Cubemap.h"
#include "../Assets/Shader.h"
#include "../Assets/Material.h"
#include "../Assets/Mesh.h"
#include "../Serialization/Settings/ProjectSettings.h"
#include "../Math/Mathf.h"
#include "../Math/Raycast.h"
#include "../Core/Time.h"

#include "../Classes/brtshaderc.h"

#include "SystemShaders/ShadowCaster.h"
#include "SystemShaders/DefaultShader.h"
#include "SystemShaders/DeferredCombine.h"
#include "SystemShaders/Presentation.h"
#include "SystemShaders/Bloom.h"
#include "SystemShaders/SSR.h"
#include "SystemShaders/Irradiance.h"
#include "SystemShaders/SimpleShader.h"
#include "SystemShaders/SimpleTextureShader.h"
#include "SystemShaders/TerrainTreeBillboardShader.h"
#include "SystemShaders/TerrainTreeBillboardLightShader.h"
#include "SystemShaders/TransparentShader.h"
#include "SystemShaders/OutlineShader.h"
#include "SystemShaders/OutlineFinalShader.h"
#include "SystemShaders/CameraBackBuffer.h"
#include "SystemShaders/FXAA.h"
#include "SystemShaders/Skybox.h"

#include "../OcclusionCulling/CullingThreadpool.h"

constexpr auto SHADOW_AAB_VAL = 9000;

namespace
{
	std::string formatPointer(const void* value)
	{
		char buffer[32];
		bx::snprintf(buffer, sizeof(buffer), "%p", value);
		return buffer;
	}

#ifdef FALCO_MODERN_BGFX
	class FalcoBgfxDiagnosticCallback final : public bgfx::CallbackI
	{
	public:
		void fatal(const char* filePath, uint16_t line, bgfx::Fatal::Enum code, const char* message) override
		{
			std::lock_guard<std::mutex> lock(mutex);
			GX::Debug::logError(
				"[bgfx fatal] code=" + std::to_string(static_cast<int>(code)) +
				" " + (filePath != nullptr ? filePath : "<unknown>") +
				":" + std::to_string(line) + " " +
				(message != nullptr ? message : "<no message>"));

			if (code != bgfx::Fatal::DebugCheck)
				std::abort();
		}

		void traceVargs(const char* filePath, uint16_t line, const char* format, va_list args) override
		{
			char buffer[4096];
			va_list argsCopy;
			va_copy(argsCopy, args);
			bx::vsnprintf(buffer, sizeof(buffer), format, argsCopy);
			va_end(argsCopy);

			std::string message(buffer);
			while (!message.empty() && (message.back() == '\n' || message.back() == '\r'))
				message.pop_back();

			std::lock_guard<std::mutex> lock(mutex);
			GX::Debug::log(
				"[bgfx trace] " + std::string(filePath != nullptr ? filePath : "<unknown>") +
				":" + std::to_string(line) + " " + message);
		}

		void profilerBegin(const char*, uint32_t, const char*, uint16_t) override {}
		void profilerBeginLiteral(const char*, uint32_t, const char*, uint16_t) override {}
		void profilerEnd() override {}
		uint32_t cacheReadSize(uint64_t) override { return 0; }
		bool cacheRead(uint64_t, void*, uint32_t) override { return false; }
		void cacheWrite(uint64_t, const void*, uint32_t) override {}
		void screenShot(const char* path, uint32_t width, uint32_t height, uint32_t pitch,
			bgfx::TextureFormat::Enum format, const void* data, uint32_t size, bool yflip) override
		{
			std::string result = "[Screenshot] FAILED: " + std::string(path ? path : "<null>");
			if (path && data && width > 0 && height > 0 && width <= INT_MAX && height <= INT_MAX &&
				uint64_t(width) * 4 <= pitch && uint64_t(height) * pitch <= size &&
				(format == bgfx::TextureFormat::BGRA8 || format == bgfx::TextureFormat::RGBA8))
			{
				// PNG screenshots are opaque RGB. Ignore swapchain alpha, which can
				// be zero even when the displayed window contains a valid image.
				FIBITMAP* bitmap = FreeImage_Allocate(int(width), int(height), 24);
				if (bitmap)
				{
					for (uint32_t y = 0; y < height; ++y)
					{
						const BYTE* src = static_cast<const BYTE*>(data) + size_t(yflip ? height - 1 - y : y) * pitch;
						BYTE* dst = FreeImage_GetScanLine(bitmap, height - 1 - y);
						for (uint32_t x = 0; x < width; ++x)
						{
							dst[x * 3 + FI_RGBA_RED] = src[x * 4 + (format == bgfx::TextureFormat::BGRA8 ? 2 : 0)];
							dst[x * 3 + FI_RGBA_GREEN] = src[x * 4 + 1];
							dst[x * 3 + FI_RGBA_BLUE] = src[x * 4 + (format == bgfx::TextureFormat::BGRA8 ? 0 : 2)];
						}
					}
#ifdef _WIN32
					int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, nullptr, 0);
					std::vector<wchar_t> widePath(length > 0 ? length : 1);
					bool saved = length > 0 && MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, widePath.data(), length) &&
						FreeImage_SaveU(FIF_PNG, bitmap, widePath.data());
#else
					bool saved = FreeImage_Save(FIF_PNG, bitmap, path);
#endif
					FreeImage_Unload(bitmap);
					if (saved) result = "[Screenshot] Saved " + std::to_string(width) + "x" + std::to_string(height) + ": " + path;
				}
			}
			// bgfx may invoke this on its render thread. Defer editor/log callbacks
			// to the main thread instead of touching the Console UI here.
			std::lock_guard<std::mutex> lock(mutex);
			screenshotResults.push_back(result);
		}

		void flushScreenshotResults()
		{
			std::vector<std::string> results;
			{
				std::lock_guard<std::mutex> lock(mutex);
				results.swap(screenshotResults);
			}
			for (const auto& result : results)
			{
				if (result.find("[Screenshot] FAILED:") == 0) GX::Debug::logError(result);
				else GX::Debug::log(result);
			}
		}
		void captureBegin(uint32_t, uint32_t, uint32_t, bgfx::TextureFormat::Enum, bool) override {}
		void captureEnd() override {}
		void captureFrame(const void*, uint32_t) override {}

	private:
		std::mutex mutex;
		std::vector<std::string> screenshotResults;
	};

	FalcoBgfxDiagnosticCallback bgfxDiagnosticCallback;

#endif
}

namespace GX
{
	Renderer Renderer::singleton;

	//System uniforms
	bgfx::UniformHandle Renderer::uLightPosition = { bgfx::kInvalidHandle };
	bgfx::UniformHandle Renderer::uLightDirection = { bgfx::kInvalidHandle };
	bgfx::UniformHandle Renderer::uLightColor = { bgfx::kInvalidHandle };
	bgfx::UniformHandle Renderer::uLightRadius = { bgfx::kInvalidHandle };
	bgfx::UniformHandle Renderer::uLightType = { bgfx::kInvalidHandle };
	bgfx::UniformHandle Renderer::uLightRenderMode = { bgfx::kInvalidHandle };
	bgfx::UniformHandle Renderer::uLightIntensity = { bgfx::kInvalidHandle };
	bgfx::UniformHandle Renderer::uLightCastShadows = { bgfx::kInvalidHandle };
	bgfx::UniformHandle Renderer::uCamPos = { bgfx::kInvalidHandle };
	bgfx::UniformHandle Renderer::uCamDir = { bgfx::kInvalidHandle };
	bgfx::UniformHandle Renderer::uCamFov = { bgfx::kInvalidHandle };
	bgfx::UniformHandle Renderer::uCamClearColor = { bgfx::kInvalidHandle };
	bgfx::UniformHandle Renderer::uCamView = { bgfx::kInvalidHandle };
	bgfx::UniformHandle Renderer::uCamInvView = { bgfx::kInvalidHandle };
	bgfx::UniformHandle Renderer::uCamProj = { bgfx::kInvalidHandle };
	bgfx::UniformHandle Renderer::uCamInvProj = { bgfx::kInvalidHandle };
	bgfx::UniformHandle Renderer::uClipPlane = { bgfx::kInvalidHandle };
	bgfx::UniformHandle Renderer::uNormalMatrix = { bgfx::kInvalidHandle };
	bgfx::UniformHandle Renderer::uShadowMaps[] = { { bgfx::kInvalidHandle }, { bgfx::kInvalidHandle }, { bgfx::kInvalidHandle }, { bgfx::kInvalidHandle } };
	bgfx::UniformHandle Renderer::uShadowMtx = { bgfx::kInvalidHandle };
	bgfx::UniformHandle Renderer::uShadowMapTexelSize = { bgfx::kInvalidHandle };
	bgfx::UniformHandle Renderer::uShadowSamplingParams = { bgfx::kInvalidHandle };
	bgfx::UniformHandle Renderer::uShadowCascadeSplits = { bgfx::kInvalidHandle };
	bgfx::UniformHandle Renderer::uAmbientColor = { bgfx::kInvalidHandle };
	bgfx::UniformHandle Renderer::uLightShadowBias = { bgfx::kInvalidHandle };
	bgfx::UniformHandle Renderer::uBoneMtx = { bgfx::kInvalidHandle };
	bgfx::UniformHandle Renderer::uGpuSkinning = { bgfx::kInvalidHandle };
	bgfx::UniformHandle Renderer::uTime = { bgfx::kInvalidHandle };
	bgfx::UniformHandle Renderer::uScaledTime = { bgfx::kInvalidHandle };
	bgfx::UniformHandle Renderer::uInvModel = { bgfx::kInvalidHandle };
    bgfx::UniformHandle Renderer::uCloudShadowParams = { bgfx::kInvalidHandle };
    bgfx::UniformHandle Renderer::uCloudShadowWind = { bgfx::kInvalidHandle };
    bgfx::UniformHandle Renderer::uCloudLayer = { bgfx::kInvalidHandle };
    bgfx::UniformHandle Renderer::uDecalBoundary = { bgfx::kInvalidHandle };
    bgfx::UniformHandle Renderer::uDecalBoundaryCount = { bgfx::kInvalidHandle };
	bgfx::UniformHandle Renderer::uScreenParams = { bgfx::kInvalidHandle };
	bgfx::UniformHandle Renderer::uRenderTargetOrigin = { bgfx::kInvalidHandle };
	bgfx::UniformHandle Renderer::uUseFXAA = { bgfx::kInvalidHandle };
    bgfx::UniformHandle Renderer::uBRDFMap = { bgfx::kInvalidHandle };
    bgfx::UniformHandle Renderer::uBRDFCache = { bgfx::kInvalidHandle };
    bgfx::UniformHandle Renderer::uPrefilteredMap = { bgfx::kInvalidHandle };
    bgfx::UniformHandle Renderer::uIrradianceMap = { bgfx::kInvalidHandle };
    bgfx::UniformHandle Renderer::uTerrainEnvironmentCache = { bgfx::kInvalidHandle };
    bgfx::UniformHandle Renderer::uEnvironmentCache = { bgfx::kInvalidHandle };
    bgfx::UniformHandle Renderer::uCubeFace = { bgfx::kInvalidHandle };
    bgfx::UniformHandle Renderer::uSSRParams = { bgfx::kInvalidHandle };
    bgfx::UniformHandle Renderer::uSSAOParams = { bgfx::kInvalidHandle };
    bgfx::UniformHandle Renderer::uPresentation = { bgfx::kInvalidHandle };
    bgfx::UniformHandle Renderer::uBloomParams = { bgfx::kInvalidHandle };
    bgfx::UniformHandle Renderer::uBloomPass = { bgfx::kInvalidHandle };
    bgfx::UniformHandle Renderer::uBloomMap = { bgfx::kInvalidHandle };
    bgfx::UniformHandle Renderer::uColorGrade = { bgfx::kInvalidHandle };
	bgfx::UniformHandle Renderer::uFogEnabled = { bgfx::kInvalidHandle };
	bgfx::UniformHandle Renderer::uFogParams = { bgfx::kInvalidHandle };
	bgfx::UniformHandle Renderer::uFogColor = { bgfx::kInvalidHandle };
	bgfx::UniformHandle Renderer::uLightmap = { bgfx::kInvalidHandle };
	bgfx::UniformHandle Renderer::uHasLightmap = { bgfx::kInvalidHandle };
	bgfx::UniformHandle Renderer::uEnvMap = { bgfx::kInvalidHandle };
	bgfx::UniformHandle Renderer::uGIParams = { bgfx::kInvalidHandle };
	bgfx::UniformHandle Renderer::uNormal = { bgfx::kInvalidHandle };

	Material* Renderer::defaultMaterial = nullptr;
	Material* Renderer::defaultStandardMaterial = nullptr;
	Shader* Renderer::defaultShader = nullptr;

	Renderer::Renderer()
	{
		
	}

	Renderer::~Renderer()
	{
		
	}

	uint64_t Renderer::getRenderState(Camera* camera, uint64_t defaultState)
	{
		uint64_t state = defaultState;

		if (camera->getInvertCulling())
		{
			state &= ~BGFX_STATE_CULL_CW;
			state |= BGFX_STATE_CULL_CCW;
		}

		return state;
	}

	//t800

	/*

	void Renderer::init(void* window, void* ctx)
	{
		sdlWindow = window;
		context = ctx;
		
		std::string version = Helper::getVersion();

		bgfx::PlatformData pd;
		pd.ndt = NULL;

		SDL_SysWMinfo wmi;
		SDL_VERSION(&wmi.version);
		if (!SDL_GetWindowWMInfo((SDL_Window*)window, &wmi))
		{
			std::cout << "SDL init error!" << std::endl;
		}

	#ifdef _WIN32
		pd.nwh = wmi.info.win.window;
	#else
		pd.ndt = wmi.info.x11.display;
		pd.nwh = (void*)(uintptr_t)wmi.info.x11.window;
	#endif
		pd.context = ctx;

		bgfx::setPlatformData(pd);

		bgfx::renderFrame();

		int w = 0;
		int h = 0;

		SDL_GetWindowSize((SDL_Window*)window, &w, &h);

		bgfx::Init init;
	#ifdef FALCO_MODERN_BGFX
		init.type = bgfx::RendererType::Direct3D11;
	#else
		init.type = bgfx::RendererType::OpenGL;
	#endif
		init.resolution.width = w;
		init.resolution.height = h;

		bgfx::init(init);

		width = w;
		height = h;

		//ImGUI
		ImGui::CreateContext();
		ImGuiIO& io = ImGui::GetIO();

		//if (!Engine::getSingleton()->getAssetsPath().empty())
		//	io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;

		ImGui_Implbgfx_Init(RENDER_UI_PASS_ID);
	#ifdef FALCO_MODERN_BGFX
		ImGui_ImplSDL2_InitForD3D((SDL_Window*)window);
	#else
		ImGui_ImplSDL2_InitForOpenGL((SDL_Window*)window, nullptr);
	#endif

		Texture::createNullTexture();
		Cubemap::createNullCubemap();

		if (!Engine::getSingleton()->getAssetsPath().empty())
		{
			///--------------------------------------------------------///
			///-------------------------LOG----------------------------///

		#ifdef FALCO_MODERN_BGFX
			std::string vendorName = "Unknown";
			std::string gpuName = bgfx::getRendererName(bgfx::getRendererType());
		#else
			const GLubyte* vendor = glGetString(GL_VENDOR); // Returns the vendor
			const GLubyte* renderer = glGetString(GL_RENDERER); // Returns a hint to the model

			GLint nTotalMemoryInKB = 0;
			glGetIntegerv(0x9048, &nTotalMemoryInKB);
			std::string gpuMemTotal = std::to_string(nTotalMemoryInKB / 1000);

			std::string vendorName = vendor != nullptr ? (const char*)vendor : "Unknown";
			std::string gpuName = renderer != nullptr ? (const char*)renderer : "Unknown";
		#endif

			Debug::log("Renderer initialized", Debug::DbgColorGreen);
			Debug::log("--Version " + version);
			Debug::log("--GPU Vendor: " + vendorName);
			Debug::log("--GPU Name: " + gpuName);
		#ifdef FALCO_MODERN_BGFX
			Debug::log("--GPU Memory total: Unavailable");
		#else
			Debug::log("--GPU Memory total: " + gpuMemTotal + "MB");
		#endif
			Debug::log("\n");

			///--------------------------------------------------------///
			///--------------------------------------------------------///

			projectSettings = Engine::getSingleton()->getSettings();
			
			createResources();

		//	SYSTEM_INFO sysinfo;
		//	GetSystemInfo(&sysinfo);
		//	int numCPU = sysinfo.dwNumberOfProcessors;
		//	numCPU = numCPU >= 2 ? 2 : 1;

			moc = MaskedOcclusionCulling::Create();
			//auto impl = moc->GetImplementation();
			//cmoc = new CullingThreadpool(numCPU, numCPU, numCPU);
			//cmoc->SetBuffer(moc);

			const int _width = 1280, _height = 960;
			moc->SetResolution(_width, _height);
			moc->SetNearClipPlane(0.1f);

			//cmoc->WakeThreads();
		}

		isRunning = true;
	}

*/



	void Renderer::init(void* window, void* ctx)
	{
		Debug::log("Renderer initialization started", Debug::DbgColorYellow);

		sdlWindow = window;
		context = ctx;
		Debug::log("SDL window and context set");

		std::string version = Helper::getVersion();
		Debug::log("Engine version: " + version);

		bgfx::PlatformData pd;
		pd.ndt = NULL;

		SDL_SysWMinfo wmi;
		SDL_VERSION(&wmi.version);
		if (!SDL_GetWindowWMInfo((SDL_Window*)window, &wmi))
		{
			Debug::log("Error: SDL_GetWindowWMInfo failed!", Debug::DbgColorRed);
			return; // РџСЂРµСЂС‹РІР°РЅРёРµ, РµСЃР»Рё РЅРµ СѓРґР°Р»РѕСЃСЊ РїРѕР»СѓС‡РёС‚СЊ РёРЅС„РѕСЂРјР°С†РёСЋ Рѕ РѕРєРЅРµ
		}
		Debug::log("SDL_GetWindowWMInfo succeeded");

#ifdef _WIN32
		pd.nwh = wmi.info.win.window;
		Debug::log("Platform: Windows");
#else
		pd.ndt = wmi.info.x11.display;
		pd.nwh = (void*)(uintptr_t)wmi.info.x11.window;
		Debug::log("Platform: X11");
#endif
		pd.context = ctx;
	#ifdef FALCO_MODERN_BGFX
		Debug::log("BGFX platform data prepared for bgfx::Init");
	#else
		bgfx::setPlatformData(pd);
		Debug::log("BGFX platform data set");
	#endif
		Debug::log("BGFX platform data nwh: " + formatPointer(pd.nwh));
		Debug::log("BGFX platform data context: " + formatPointer(pd.context));
#ifdef _WIN32
		Debug::log("BGFX platform HWND IsWindow: " + std::string(IsWindow(static_cast<HWND>(pd.nwh)) ? "true" : "false"));
#endif

	#ifdef FALCO_MODERN_BGFX
		bgfx::RendererType::Enum supportedRenderers[bgfx::RendererType::Count];
		const uint8_t supportedRendererCount = bgfx::getSupportedRenderers(bgfx::RendererType::Count, supportedRenderers);
		std::string supportedRendererNames;
		for (uint8_t i = 0; i < supportedRendererCount; ++i)
		{
			if (!supportedRendererNames.empty())
				supportedRendererNames += ", ";
			supportedRendererNames += bgfx::getRendererName(supportedRenderers[i]);
		}
		Debug::log("BGFX supported renderers (" + std::to_string(supportedRendererCount) + "): " + supportedRendererNames);
	#endif

		bgfx::renderFrame();
		Debug::log("BGFX render frame called");

		int w = 0;
		int h = 0;
		SDL_GetWindowSize((SDL_Window*)window, &w, &h);
		Debug::log("Window size retrieved: " + std::to_string(w) + "x" + std::to_string(h));

		bgfx::Init init;
	#ifdef FALCO_MODERN_BGFX
		init.type = bgfx::RendererType::Direct3D11;
		// All modern shaders are DXBC. Do not silently enter another backend
		// (including an installed but broken Vulkan loader) after D3D11 failure.
		init.fallback = false;
		Debug::log("BGFX renderer fallback disabled (D3D11 only)");
	#else
		init.type = bgfx::RendererType::OpenGL;
	#endif
		init.resolution.width = w;
		init.resolution.height = h;
	#ifdef FALCO_MODERN_BGFX
		init.platformData = pd;
		init.callback = &bgfxDiagnosticCallback;
	#endif
		Debug::log("BGFX init renderer: " + std::string(bgfx::getRendererName(init.type)));
		Debug::log("BGFX init resolution: " + std::to_string(init.resolution.width) + "x" + std::to_string(init.resolution.height));
	#ifdef FALCO_MODERN_BGFX
		Debug::log("BGFX init platformData nwh: " + formatPointer(init.platformData.nwh));
		Debug::log("BGFX init platformData context: " + formatPointer(init.platformData.context));
	#endif
		Debug::log("SDL error before BGFX init: " + std::string(SDL_GetError()));

		if (!bgfx::init(init))
		{
			Debug::log("Error: BGFX initialization failed!", Debug::DbgColorRed);
			return;
		}
		Debug::log("BGFX initialized successfully");

		width = w;
		height = h;

		Debug::log("Initializing ImGui...");
		ImGui::CreateContext();
		ImGuiIO& io = ImGui::GetIO();
		ImGui_Implbgfx_Init(RENDER_UI_PASS_ID);
	#ifdef FALCO_MODERN_BGFX
		ImGui_ImplSDL2_InitForD3D((SDL_Window*)window);
	#else
		ImGui_ImplSDL2_InitForOpenGL((SDL_Window*)window, nullptr);
	#endif
		Debug::log("ImGui initialized");

		Debug::log("Creating null textures...");
		Texture::createNullTexture();
		Cubemap::createNullCubemap();
		Debug::log("Null textures created");

		if (!Engine::getSingleton()->getAssetsPath().empty())
		{
		#ifdef FALCO_MODERN_BGFX
			std::string vendorName = "Unknown";
			std::string gpuName = bgfx::getRendererName(bgfx::getRendererType());
		#else
			const GLubyte* vendor = glGetString(GL_VENDOR);
			const GLubyte* renderer = glGetString(GL_RENDERER);

			GLint nTotalMemoryInKB = 0;
			glGetIntegerv(0x9048, &nTotalMemoryInKB);
			std::string gpuMemTotal = std::to_string(nTotalMemoryInKB / 1000);

			std::string vendorName = vendor != nullptr ? (const char*)vendor : "Unknown";
			std::string gpuName = renderer != nullptr ? (const char*)renderer : "Unknown";
		#endif

			Debug::log("GPU Information:");
			Debug::log("-- Vendor: " + vendorName);
			Debug::log("-- Renderer: " + gpuName);
		#ifdef FALCO_MODERN_BGFX
			Debug::log("-- Total Memory: Unavailable");
		#else
			Debug::log("-- Total Memory: " + gpuMemTotal + "MB");
		#endif


			Debug::log("Loading project settings...");
			projectSettings = Engine::getSingleton()->getSettings();
			Debug::log("Project settings loaded");

			Debug::log("Creating resources...");
			createResources();
			Debug::log("Resources created");

			moc = MaskedOcclusionCulling::Create();
			if (moc == nullptr)
			{
				Debug::log("Error: Failed to initialize MaskedOcclusionCulling", Debug::DbgColorRed);
				return;
			}
			Debug::log("MaskedOcclusionCulling initialized");

			const int _width = 1280, _height = 960;
			moc->SetResolution(_width, _height);
			Debug::log("Occlusion culling resolution set to: " + std::to_string(_width) + "x" + std::to_string(_height));

			moc->SetNearClipPlane(0.1f);
			Debug::log("Occlusion culling near clip plane set to 0.1");
		}
	isRunning = true;
	Debug::log("Renderer initialization completed successfully", Debug::DbgColorGreen);

}


	void Renderer::createResources()
	{
		Debug::log("Initializing Vertex Layouts...");
		VertexLayouts::init();

		//Compile shadow caster shader
		Debug::log("Compiling shadow caster shader...");
		const bgfx::Memory* memVsh = shaderc::compileShaderFromSources(shaderc::ST_VERTEX, "/", shaders::shadowCasterVertex.c_str(), "", shaders::shadowCasterVarying.c_str());
		const bgfx::Memory* memFsh = shaderc::compileShaderFromSources(shaderc::ST_FRAGMENT, "/", shaders::shadowCasterFragment.c_str(), "", shaders::shadowCasterVarying.c_str());
		bgfx::ShaderHandle vsh = bgfx::createShader(memVsh);
		bgfx::ShaderHandle fsh = bgfx::createShader(memFsh);
		shadowCasterPH = bgfx::createProgram(vsh, fsh, true);

		//Compile renderable shader
		Debug::log("Compiling renderable shader...");
		memVsh = shaderc::compileShaderFromSources(shaderc::ST_VERTEX, "/", shaders::outlineVertex.c_str(), "", shaders::outlineVarying.c_str());
		memFsh = shaderc::compileShaderFromSources(shaderc::ST_FRAGMENT, "/", shaders::outlineFragment.c_str(), "", shaders::outlineVarying.c_str());
		vsh = bgfx::createShader(memVsh);
		fsh = bgfx::createShader(memFsh);
		outlinePH = bgfx::createProgram(vsh, fsh, true);

		//Compile simple shader
		Debug::log("Compiling simple shader...");
		memVsh = shaderc::compileShaderFromSources(shaderc::ST_VERTEX, "/", shaders::simpleVertex.c_str(), "", shaders::simpleVarying.c_str());
		memFsh = shaderc::compileShaderFromSources(shaderc::ST_FRAGMENT, "/", shaders::simpleFragment.c_str(), "", shaders::simpleVarying.c_str());
		vsh = bgfx::createShader(memVsh);
		fsh = bgfx::createShader(memFsh);
		simplePH = bgfx::createProgram(vsh, fsh, true);

		//Compile simple texture shader
		Debug::log("Compiling simple texture shader...");
		memVsh = shaderc::compileShaderFromSources(shaderc::ST_VERTEX, "/", shaders::simpleTextureVertex.c_str(), "", shaders::simpleTextureVarying.c_str());
		memFsh = shaderc::compileShaderFromSources(shaderc::ST_FRAGMENT, "/", shaders::simpleTextureFragment.c_str(), "", shaders::simpleTextureVarying.c_str());
		vsh = bgfx::createShader(memVsh);
		fsh = bgfx::createShader(memFsh);
		simpleTexturePH = bgfx::createProgram(vsh, fsh, true);

		//Compile terrain tree billboard shader
		Debug::log("Compiling terrain tree billboard shader...");
		memVsh = shaderc::compileShaderFromSources(shaderc::ST_VERTEX, "/", shaders::terrainTreeBillboardLightVertex.c_str(), "", shaders::terrainTreeBillboardLightVarying.c_str());
		memFsh = shaderc::compileShaderFromSources(shaderc::ST_FRAGMENT, "/", shaders::terrainTreeBillboardFragment.c_str(), "", shaders::terrainTreeBillboardLightVarying.c_str());
		vsh = bgfx::createShader(memVsh);
		fsh = bgfx::createShader(memFsh);
		terrainTreeBillboardPH = bgfx::createProgram(vsh, fsh, true);
        memVsh = shaderc::compileShaderFromSources(shaderc::ST_VERTEX, "/", shaders::terrainTreeBillboardLightVertex.c_str(), "", shaders::terrainTreeBillboardLightVarying.c_str());
        memFsh = shaderc::compileShaderFromSources(shaderc::ST_FRAGMENT, "/", shaders::terrainTreeCaptureFragment.c_str(), "", shaders::terrainTreeBillboardLightVarying.c_str());
        vsh = memVsh ? bgfx::createShader(memVsh) : bgfx::ShaderHandle{ bgfx::kInvalidHandle };
        fsh = memFsh ? bgfx::createShader(memFsh) : bgfx::ShaderHandle{ bgfx::kInvalidHandle };
        if (bgfx::isValid(vsh) && bgfx::isValid(fsh))
            terrainTreeCapturePH = bgfx::createProgram(vsh, fsh, true);
        else
        {
            if (bgfx::isValid(vsh)) bgfx::destroy(vsh);
            if (bgfx::isValid(fsh)) bgfx::destroy(fsh);
            Debug::logError("Terrain tree capture shader compilation failed");
        }


		//Compile terrain tree billboard light shader
		Debug::log("Compiling terrain tree billboard light shader...");
		memFsh = shaderc::compileShaderFromSources(shaderc::ST_FRAGMENT, "/", shaders::terrainTreeBillboardLightFragment.c_str(), "", shaders::terrainTreeBillboardLightVarying.c_str());
		fsh = bgfx::createShader(memFsh);
		terrainTreeBillboardLightPH = bgfx::createProgram(vsh, fsh, true);

		//Compile billboard shader
		Debug::log("Compiling billboard shader...");
		memVsh = shaderc::compileShaderFromSources(shaderc::ST_VERTEX, "/", shaders::transparentVertex.c_str(), "", shaders::transparentVarying.c_str());
		memFsh = shaderc::compileShaderFromSources(shaderc::ST_FRAGMENT, "/", shaders::transparentFragment.c_str(), "", shaders::transparentVarying.c_str());
		vsh = bgfx::createShader(memVsh);
		fsh = bgfx::createShader(memFsh);
		transparentPH = bgfx::createProgram(vsh, fsh, true);

		//Compile combine shader
		Debug::log("Compiling combine shader...");
		memVsh = shaderc::compileShaderFromSources(shaderc::ST_VERTEX, "/", shaders::deferredCombineVertex.c_str(), "", shaders::deferredCombineVarying.c_str());
		memFsh = shaderc::compileShaderFromSources(shaderc::ST_FRAGMENT, "/", shaders::deferredCombineFragment.c_str(), "", shaders::deferredCombineVarying.c_str());
		vsh = bgfx::createShader(memVsh);
		fsh = bgfx::createShader(memFsh);
		combinePH = bgfx::createProgram(vsh, fsh, true);

		//Compile final shader
		Debug::log("Compiling final shader...");
		memVsh = shaderc::compileShaderFromSources(shaderc::ST_VERTEX, "/", shaders::outlineFinalVertex.c_str(), "", shaders::outlineFinalVarying.c_str());
		memFsh = shaderc::compileShaderFromSources(shaderc::ST_FRAGMENT, "/", shaders::outlineFinalFragment.c_str(), "", shaders::outlineFinalVarying.c_str());
		vsh = bgfx::createShader(memVsh);
		fsh = bgfx::createShader(memFsh);
		outlineFinalPH = bgfx::createProgram(vsh, fsh, true);

		//Camera back buffer shader
		Debug::log("Compiling СЃamera back buffer shader...");
		memVsh = shaderc::compileShaderFromSources(shaderc::ST_VERTEX, "/", shaders::cameraBackBufferVertex.c_str(), "", shaders::cameraBackBufferVarying.c_str());
		memFsh = shaderc::compileShaderFromSources(shaderc::ST_FRAGMENT, "/", shaders::cameraBackBufferFragment.c_str(), "", shaders::cameraBackBufferVarying.c_str());
		vsh = bgfx::createShader(memVsh);
		fsh = bgfx::createShader(memFsh);
		cameraBackBufferPH = bgfx::createProgram(vsh, fsh, true);

		//FXAA shader
		Debug::log("Compiling FXAA shader...");
		memVsh = shaderc::compileShaderFromSources(shaderc::ST_VERTEX, "/", shaders::fxaaVertex.c_str(), "", shaders::fxaaVarying.c_str());
		memFsh = shaderc::compileShaderFromSources(shaderc::ST_FRAGMENT, "/", shaders::fxaaFragment.c_str(), "", shaders::fxaaVarying.c_str());
		vsh = bgfx::createShader(memVsh);
		fsh = bgfx::createShader(memFsh);
		fxaaPH = bgfx::createProgram(vsh, fsh, true);
        // createShader consumes bgfx::Memory; release is not part of the public API.
        auto discardShaderMemory = [](const bgfx::Memory* memory)
        {
            if (!memory) return;
            auto shader = bgfx::createShader(memory);
            if (bgfx::isValid(shader)) bgfx::destroy(shader);
        };
        memVsh = shaderc::compileShaderFromSources(shaderc::ST_VERTEX, "/", shaders::deferredCombineVertex.c_str(), "", shaders::deferredCombineVarying.c_str());
        memFsh = shaderc::compileShaderFromSources(shaderc::ST_FRAGMENT, "/", shaders::presentationFragment.c_str(), "", shaders::deferredCombineVarying.c_str());
        if (memVsh && memFsh)
            presentationPH = bgfx::createProgram(bgfx::createShader(memVsh), bgfx::createShader(memFsh), true);
        else
        {
            if (memVsh) discardShaderMemory(memVsh);
            if (memFsh) discardShaderMemory(memFsh);
            Debug::logError("Presentation shader compilation failed");
        }

        memVsh = shaderc::compileShaderFromSources(shaderc::ST_VERTEX, "/", shaders::deferredCombineVertex.c_str(), "", shaders::deferredCombineVarying.c_str());
        memFsh = shaderc::compileShaderFromSources(shaderc::ST_FRAGMENT, "/", shaders::bloomFragment.c_str(), "", shaders::deferredCombineVarying.c_str());
        if (memVsh && memFsh)
            bloomPH = bgfx::createProgram(bgfx::createShader(memVsh), bgfx::createShader(memFsh), true);
        else
        {
            if (memVsh) discardShaderMemory(memVsh);
            if (memFsh) discardShaderMemory(memFsh);
            Debug::logError("Bloom shader compilation failed");
        }
        memVsh = shaderc::compileShaderFromSources(shaderc::ST_VERTEX, "/", shaders::deferredCombineVertex.c_str(), "", shaders::deferredCombineVarying.c_str());
        memFsh = shaderc::compileShaderFromSources(shaderc::ST_FRAGMENT, "/", shaders::ssrFragment.c_str(), "", shaders::deferredCombineVarying.c_str());
        if (memVsh && memFsh) ssrPH = bgfx::createProgram(bgfx::createShader(memVsh), bgfx::createShader(memFsh), true);
        else
        {
            if (memVsh) discardShaderMemory(memVsh);
            if (memFsh) discardShaderMemory(memFsh);
            Debug::logError("SSR shader compilation failed");
        }
        memVsh = shaderc::compileShaderFromSources(shaderc::ST_VERTEX, "/", shaders::deferredCombineVertex.c_str(), "", shaders::deferredCombineVarying.c_str());
        memFsh = shaderc::compileShaderFromSources(shaderc::ST_FRAGMENT, "/", shaders::irradianceFragment.c_str(), "", shaders::deferredCombineVarying.c_str());
        if (memVsh && memFsh) irradiancePH = bgfx::createProgram(bgfx::createShader(memVsh), bgfx::createShader(memFsh), true);
        else
        {
            if (memVsh) discardShaderMemory(memVsh);
            if (memFsh) discardShaderMemory(memFsh);
            Debug::logError("Irradiance shader compilation failed");
        }
		//Skybox shader
		Debug::log("Compiling Skybox shader...");
		memVsh = shaderc::compileShaderFromSources(shaderc::ST_VERTEX, "/", shaders::skyboxVertex.c_str(), "", shaders::skyboxVarying.c_str());
		memFsh = shaderc::compileShaderFromSources(shaderc::ST_FRAGMENT, "/", shaders::skyboxFragment.c_str(), "", shaders::skyboxVarying.c_str());
		vsh = bgfx::createShader(memVsh);
		fsh = bgfx::createShader(memFsh);
		skyboxPH = bgfx::createProgram(vsh, fsh, true);

		//Light shader
		/*
		
		Debug::log("Compiling Light shader...");
		lightShader = Shader::load(Engine::getSingleton()->getBuiltinResourcesPath(), "Shaders/StandardDeferredLight.shader");
		lightShader->compile("");
		if (lightShader->getPassCount() > 0)
			lightPH = lightShader->getPass(0)->getProgramHandle("");

        */

		
		/*
		
		//Light shader
		Debug::log("Compiling Light shader...");
		try {
			Debug::log("Loading shader: Shaders/StandardDeferredLight.shader");
			lightShader = Shader::load(Engine::getSingleton()->getBuiltinResourcesPath(), "Shaders/StandardDeferredLight.shader");
			Debug::log("Shader loaded successfully. Compiling...");
			lightShader->compile("");
			Debug::log("Shader compiled successfully. Checking pass count...");
			if (lightShader->getPassCount() > 0) {
				Debug::log("Shader has valid passes. Retrieving program handle...");
				lightPH = lightShader->getPass(0)->getProgramHandle("");
				Debug::log("Program handle retrieved successfully.");
			}
			else {
				Debug::logError("Shader has no valid passes.");
			}
		}
		catch (const std::exception& e) {
			Debug::logError(std::string("Exception during shader compilation: ") + e.what());
		}
		catch (...) {
			Debug::logError("Unknown error during shader compilation.");
		}


		*/

		//Light shader
		Debug::log("Compiling Light shader...");
		try {
			// РџСЂРµРѕР±СЂР°Р·СѓРµРј РїСѓС‚СЊ РІСЂСѓС‡РЅСѓСЋ, Р·Р°РјРµРЅСЏСЏ РѕР±СЂР°С‚РЅС‹Рµ СЃР»РµС€Рё РЅР° РїСЂСЏРјС‹Рµ
			std::string shaderPath = Engine::getSingleton()->getBuiltinResourcesPath() + "Shaders/StandardDeferredLight.shader";
			Debug::log("Original shader path: " + shaderPath);

			// Р—Р°РјРµРЅСЏРµРј '\\' РЅР° '/' Р±РµР· РёСЃРїРѕР»СЊР·РѕРІР°РЅРёСЏ Boost
			for (char& ch : shaderPath) {
				if (ch == '\\') ch = '/';
			}
			Debug::log("Normalized shader path: " + shaderPath);

			Debug::log("Loading shader...");
			lightShader = Shader::load(Engine::getSingleton()->getBuiltinResourcesPath(), "Shaders/StandardDeferredLight.shader");
			if (!lightShader) {
				Debug::logError("Failed to load shader.");
				return;
			}

			Debug::log("Shader loaded successfully. Compiling...");
			lightShader->compile("");
			Debug::log("Shader compiled successfully. Checking pass count...");

			if (lightShader->getPassCount() > 0) {
				Debug::log("Shader has valid passes. Retrieving program handle...");
				lightPH = lightShader->getPass(0)->getProgramHandle("");
				Debug::log("Program handle retrieved successfully.");
			}
			else {
				Debug::logError("Shader has no valid passes.");
			}
		}
		catch (const std::exception& e) {
			Debug::logError(std::string("Exception during shader compilation: ") + e.what());
		}
		catch (...) {
			Debug::logError("Unknown error during shader compilation.");
		}

		Debug::log("Creating Light Shader uniforms");

		uLightPosition = bgfx::createUniform("u_lightPosition", bgfx::UniformType::Vec4, 1);
		uLightDirection = bgfx::createUniform("u_lightDirection", bgfx::UniformType::Vec4, 1);
		uLightColor = bgfx::createUniform("u_lightColor", bgfx::UniformType::Vec4, 1);
		uLightRadius = bgfx::createUniform("u_lightRadius", bgfx::UniformType::Vec4, 1);
		uLightIntensity = bgfx::createUniform("u_lightIntensity", bgfx::UniformType::Vec4, 1);
		uLightType = bgfx::createUniform("u_lightType", bgfx::UniformType::Vec4, 1);
		uLightRenderMode = bgfx::createUniform("u_lightRenderMode", bgfx::UniformType::Vec4, 1);
		uLightShadowBias = bgfx::createUniform("u_lightShadowBias", bgfx::UniformType::Vec4, 1);
		uLightCastShadows = bgfx::createUniform("u_lightCastShadows", bgfx::UniformType::Vec4, 1);

		Debug::log("Creating Camera uniforms");

		uCamPos = bgfx::createUniform("u_camPos", bgfx::UniformType::Vec4, 1);
		uCamDir = bgfx::createUniform("u_camDir", bgfx::UniformType::Vec4, 1);
		uCamFov = bgfx::createUniform("u_camFov", bgfx::UniformType::Vec4, 1);
		uCamClearColor = bgfx::createUniform("u_clearColor", bgfx::UniformType::Vec4, 1);
		uCamView = bgfx::createUniform("u_camView", bgfx::UniformType::Mat4, 1);
		uCamProj = bgfx::createUniform("u_camProj", bgfx::UniformType::Mat4, 1);
		uCamInvView = bgfx::createUniform("u_camInvView", bgfx::UniformType::Mat4, 1);
		uCamInvProj = bgfx::createUniform("u_camInvProj", bgfx::UniformType::Mat4, 1);
		uClipPlane = bgfx::createUniform("u_clipPlane", bgfx::UniformType::Vec4, 1);
		uScreenParams = bgfx::createUniform("u_screenParams", bgfx::UniformType::Vec4, 1);
		uRenderTargetOrigin = bgfx::createUniform("u_rtOrigin", bgfx::UniformType::Vec4, 1);
		uUseFXAA = bgfx::createUniform("u_useFXAA", bgfx::UniformType::Vec4, 1);
        uBRDFMap = bgfx::createUniform("u_brdfMap", bgfx::UniformType::Sampler);
        uBRDFCache = bgfx::createUniform("u_brdfCache", bgfx::UniformType::Vec4);
        uPrefilteredMap = bgfx::createUniform("u_prefilteredMap", bgfx::UniformType::Sampler);
        uIrradianceMap = bgfx::createUniform("u_irradianceMap", bgfx::UniformType::Sampler);
        uTerrainEnvironmentCache = bgfx::createUniform("u_terrainEnvironmentCache", bgfx::UniformType::Vec4);
        uEnvironmentCache = bgfx::createUniform("u_environmentCache", bgfx::UniformType::Vec4);
        uCubeFace = bgfx::createUniform("u_cubeFace", bgfx::UniformType::Vec4);
        uSSRParams = bgfx::createUniform("u_ssrParams", bgfx::UniformType::Vec4);
        uSSAOParams = bgfx::createUniform("u_ssaoParams", bgfx::UniformType::Vec4);
        uPresentation = bgfx::createUniform("u_presentation", bgfx::UniformType::Vec4);
        uBloomParams = bgfx::createUniform("u_bloomParams", bgfx::UniformType::Vec4);
        uBloomPass = bgfx::createUniform("u_bloomPass", bgfx::UniformType::Vec4);
        uBloomMap = bgfx::createUniform("u_bloomMap", bgfx::UniformType::Sampler);
        uColorGrade = bgfx::createUniform("u_colorGrade", bgfx::UniformType::Vec4);
		uFogEnabled = bgfx::createUniform("u_fogEnabled", bgfx::UniformType::Vec4, 1);
		uFogParams = bgfx::createUniform("u_fogParams", bgfx::UniformType::Vec4, 1);
		uFogColor = bgfx::createUniform("u_fogColor", bgfx::UniformType::Vec4, 1);

		Debug::log("Creating additional Light and Camera uniforms");

		uNormalMatrix = bgfx::createUniform("u_normalMatrix", bgfx::UniformType::Mat3, 1);

		uShadowMaps[0] = bgfx::createUniform("u_shadowMap0", bgfx::UniformType::Sampler, 1);
		uShadowMaps[1] = bgfx::createUniform("u_shadowMap1", bgfx::UniformType::Sampler, 1);
		uShadowMaps[2] = bgfx::createUniform("u_shadowMap2", bgfx::UniformType::Sampler, 1);
		uShadowMaps[3] = bgfx::createUniform("u_shadowMap3", bgfx::UniformType::Sampler, 1);

		uShadowMtx = bgfx::createUniform("u_shadowMatrix", bgfx::UniformType::Mat4, 4);
		uBoneMtx = bgfx::createUniform("u_boneMatrix", bgfx::UniformType::Mat4, 128);
		uGpuSkinning = bgfx::createUniform("u_skinned", bgfx::UniformType::Vec4, 1);
		uTime = bgfx::createUniform("u_time", bgfx::UniformType::Vec4, 1);
		uScaledTime = bgfx::createUniform("u_timeScaled", bgfx::UniformType::Vec4, 1);
		uInvModel = bgfx::createUniform("u_invModel", bgfx::UniformType::Mat4, 1);
        uCloudShadowParams = bgfx::createUniform("u_cloudShadowParams", bgfx::UniformType::Vec4);
        uCloudShadowWind = bgfx::createUniform("u_cloudShadowWind", bgfx::UniformType::Vec4);
        uCloudLayer = bgfx::createUniform("u_cloudLayer", bgfx::UniformType::Vec4);
        uDecalBoundary = bgfx::createUniform("u_decalBoundaryTree", bgfx::UniformType::Sampler);
        uDecalBoundaryCount = bgfx::createUniform("u_decalBoundaryCount", bgfx::UniformType::Vec4);

		uShadowMapTexelSize = bgfx::createUniform("u_shadowMapTexelSize", bgfx::UniformType::Vec4, 1);
		uShadowSamplingParams = bgfx::createUniform("u_shadowSamplingParams", bgfx::UniformType::Vec4, 1);
		uShadowCascadeSplits = bgfx::createUniform("u_shadowCascadeSplits", bgfx::UniformType::Vec4, 1);

		uAmbientColor = bgfx::createUniform("u_ambientColor", bgfx::UniformType::Vec4, 1);
		uEnvMap = bgfx::createUniform("u_envMap", bgfx::UniformType::Sampler, 1);
		uGIParams = bgfx::createUniform("u_giParams", bgfx::UniformType::Vec4, 1);
		//uGIMode = bgfx::createUniform("u_giMode", bgfx::UniformType::Vec4, 1);

		//Deferred
		u_albedoMap = bgfx::createUniform("u_albedoMap", bgfx::UniformType::Sampler, 1);
		u_normalMap = bgfx::createUniform("u_normalMap", bgfx::UniformType::Sampler, 1);
		u_mraMap = bgfx::createUniform("u_mraMap", bgfx::UniformType::Sampler, 1);
		u_depthMap = bgfx::createUniform("u_depthMap", bgfx::UniformType::Sampler, 1);
		u_lightBuffer = bgfx::createUniform("u_lightBuffer", bgfx::UniformType::Sampler, 1);
		u_VP = bgfx::createUniform("u_VP", bgfx::UniformType::Mat4, 1);
		u_invVP = bgfx::createUniform("u_invVP", bgfx::UniformType::Mat4, 1);
		u_aspect = bgfx::createUniform("u_aspect", bgfx::UniformType::Vec4, 1);
		u_forwardColor = bgfx::createUniform("u_forwardColor", bgfx::UniformType::Sampler, 1);
		u_forwardDepth = bgfx::createUniform("u_forwardDepth", bgfx::UniformType::Sampler, 1);
		u_outlineParams = bgfx::createUniform("u_outlineParams", bgfx::UniformType::Vec4, 1);
		u_outlineColor = bgfx::createUniform("u_outlineColor", bgfx::UniformType::Vec4, 1);

		//Other
		u_color = bgfx::createUniform("u_color", bgfx::UniformType::Vec4, 1);
		u_scale = bgfx::createUniform("u_scale", bgfx::UniformType::Vec4, 1);

		u_texture = bgfx::createUniform("u_texture", bgfx::UniformType::Sampler, 1);
		uLightmap = bgfx::createUniform("u_lightMap", bgfx::UniformType::Sampler, 1);
		uHasLightmap = bgfx::createUniform("u_hasLightMap", bgfx::UniformType::Vec4, 1);

		uNormal = bgfx::createUniform("u_normal", bgfx::UniformType::Vec4, 1);

		Debug::log("Uniforms created");


		if (Engine::getSingleton()->getIsRuntimeMode()) {
			Debug::log("Creating back buffer for runtime mode");
			backBuffer = new RenderTexture(width, height);
		}

		Debug::log("Creating default shader and material");
		defaultShader = Shader::create("system/shaders/", "DefaultShader");
		defaultShader->loadFromSource(shaders::defaultShader);
		defaultShader->compile("");
		defaultShader->setPersistent(true);

		defaultMaterial = Material::create("system/materials/1/", "DefaultMaterial");
		defaultMaterial->load();
		defaultMaterial->setShader(defaultShader);
		defaultMaterial->setPersistent(true);
		defaultMaterial->compileShader();

		Shader* standardShader = Shader::load(Engine::getSingleton()->getBuiltinResourcesPath(), "Shaders/StandardDeferred.shader");

		defaultStandardMaterial = Material::create("system/materials/2/", "DefaultMaterial");
		defaultStandardMaterial->load();
		defaultStandardMaterial->setShader(standardShader);
		defaultStandardMaterial->setPersistent(true);
		Debug::log("Renderer::createResources - Completed");
	}

	void Renderer::destroyResources()
	{
		if (backBuffer != nullptr)
			delete backBuffer;

		backBuffer = nullptr;

		if (bgfx::isValid(shadowCasterPH))
			bgfx::destroy(shadowCasterPH);

		if (bgfx::isValid(outlinePH))
			bgfx::destroy(outlinePH);
		
		if (bgfx::isValid(simplePH))
			bgfx::destroy(simplePH);
		
		if (bgfx::isValid(simpleTexturePH))
			bgfx::destroy(simpleTexturePH);
		
        if (bgfx::isValid(terrainTreeCapturePH)) bgfx::destroy(terrainTreeCapturePH);
		if (bgfx::isValid(terrainTreeBillboardPH))
			bgfx::destroy(terrainTreeBillboardPH);
		
		if (bgfx::isValid(terrainTreeBillboardLightPH))
			bgfx::destroy(terrainTreeBillboardLightPH);
		
		if (bgfx::isValid(transparentPH))
			bgfx::destroy(transparentPH);
		
		if (bgfx::isValid(combinePH))
			bgfx::destroy(combinePH);
		
		if (bgfx::isValid(outlineFinalPH))
			bgfx::destroy(outlineFinalPH);
		
		if (bgfx::isValid(cameraBackBufferPH))
			bgfx::destroy(cameraBackBufferPH);
		
		if (bgfx::isValid(fxaaPH))
			bgfx::destroy(fxaaPH);
		
		if (bgfx::isValid(skyboxPH))
			bgfx::destroy(skyboxPH);

		deleteEnvironmentMap();

		shadowCasterPH = { bgfx::kInvalidHandle };
		outlinePH = { bgfx::kInvalidHandle };
		simplePH = { bgfx::kInvalidHandle };
		simpleTexturePH = { bgfx::kInvalidHandle };
		terrainTreeBillboardPH = { bgfx::kInvalidHandle };
        terrainTreeCapturePH = { bgfx::kInvalidHandle };
		terrainTreeBillboardLightPH = { bgfx::kInvalidHandle };
		transparentPH = { bgfx::kInvalidHandle };
		combinePH = { bgfx::kInvalidHandle };
		outlineFinalPH = { bgfx::kInvalidHandle };
		cameraBackBufferPH = { bgfx::kInvalidHandle };
		fxaaPH = { bgfx::kInvalidHandle };
        if (bgfx::isValid(environmentBRDF)) bgfx::destroy(environmentBRDF);
        environmentBRDF = { bgfx::kInvalidHandle };
        if (bgfx::isValid(irradiancePH)) bgfx::destroy(irradiancePH);
        irradiancePH = { bgfx::kInvalidHandle };
        if (bgfx::isValid(ssrPH)) bgfx::destroy(ssrPH);
        ssrPH = { bgfx::kInvalidHandle };
        if (bgfx::isValid(bloomPH)) bgfx::destroy(bloomPH);
        bloomPH = { bgfx::kInvalidHandle };
        if (bgfx::isValid(presentationPH)) bgfx::destroy(presentationPH);
        presentationPH = { bgfx::kInvalidHandle };
		skyboxPH = { bgfx::kInvalidHandle };
	}

	void Renderer::deleteEnvironmentMap()
	{
        if (bgfx::isValid(diffuseIrradiance)) bgfx::destroy(diffuseIrradiance);
        diffuseIrradiance = { bgfx::kInvalidHandle };
        if (bgfx::isValid(specularEnvironment)) bgfx::destroy(specularEnvironment);
        specularEnvironment = { bgfx::kInvalidHandle };
		if (environmentMap != nullptr && environmentMap->isLoaded())
			delete environmentMap;

		environmentMap = nullptr;
	}

	void Renderer::setGIEnabled(bool value)
	{
		giEnabled = value;
		if (!giEnabled)
		{
			environmentMapUpdatePending = false;
			deleteEnvironmentMap();
		}
		else
			updateEnvironmentMap();
	}

	void Renderer::shutdown()
	{
		Texture::destroyNullTexture();
		Cubemap::destroyNullCubemap();

		isRunning = false;

		if (sdlWindow != nullptr)
		{
			ImGui_Implbgfx_Shutdown();
			ImGui_ImplSDL2_Shutdown();
		}

		destroyResources();

        if (bgfx::isValid(uCloudShadowParams)) bgfx::destroy(uCloudShadowParams);
        if (bgfx::isValid(uCloudShadowWind)) bgfx::destroy(uCloudShadowWind);
        if (bgfx::isValid(uBloomParams)) bgfx::destroy(uBloomParams);
        if (bgfx::isValid(uBloomPass)) bgfx::destroy(uBloomPass);
        if (bgfx::isValid(uBloomMap)) bgfx::destroy(uBloomMap);
        uBloomParams = uBloomPass = uBloomMap = { bgfx::kInvalidHandle };
        if (bgfx::isValid(uBRDFMap)) bgfx::destroy(uBRDFMap);
        if (bgfx::isValid(uBRDFCache)) bgfx::destroy(uBRDFCache);
        uBRDFMap = uBRDFCache = { bgfx::kInvalidHandle };
        if (bgfx::isValid(uPrefilteredMap)) bgfx::destroy(uPrefilteredMap);
        uPrefilteredMap = { bgfx::kInvalidHandle };
        if (bgfx::isValid(uIrradianceMap)) bgfx::destroy(uIrradianceMap);
        if (bgfx::isValid(uTerrainEnvironmentCache)) bgfx::destroy(uTerrainEnvironmentCache);
        uTerrainEnvironmentCache = { bgfx::kInvalidHandle };
        if (bgfx::isValid(uEnvironmentCache)) bgfx::destroy(uEnvironmentCache);
        if (bgfx::isValid(uCubeFace)) bgfx::destroy(uCubeFace);
        uIrradianceMap = uEnvironmentCache = uCubeFace = { bgfx::kInvalidHandle };
        if (bgfx::isValid(uSSRParams)) bgfx::destroy(uSSRParams);
        uSSRParams = { bgfx::kInvalidHandle };
        if (bgfx::isValid(uSSAOParams)) bgfx::destroy(uSSAOParams);
        uSSAOParams = { bgfx::kInvalidHandle };
        if (bgfx::isValid(uPresentation)) bgfx::destroy(uPresentation);
        if (bgfx::isValid(uColorGrade)) bgfx::destroy(uColorGrade);
        uPresentation = uColorGrade = { bgfx::kInvalidHandle };
        if (bgfx::isValid(uCloudLayer)) bgfx::destroy(uCloudLayer);
        uCloudShadowParams = uCloudShadowWind = uCloudLayer = { bgfx::kInvalidHandle };
        if (bgfx::isValid(uDecalBoundary)) bgfx::destroy(uDecalBoundary);
        if (bgfx::isValid(uDecalBoundaryCount)) bgfx::destroy(uDecalBoundaryCount);
        uDecalBoundary = uDecalBoundaryCount = { bgfx::kInvalidHandle };
		bgfx::shutdown();

		while (bgfx::RenderFrame::NoContext != bgfx::renderFrame()) {};

		uiCallback = nullptr;
		renderCallbacks.clear();

		cameras.clear();
	}

	void Renderer::setSize(int w, int h)
	{
		width = w;
		height = h;

		uint32_t flags = BGFX_RESET_MAXANISOTROPY;

		if (Engine::getSingleton()->getSettings()->getVSync())
			flags |= BGFX_RESET_VSYNC;
		
		bgfx::reset(width, height, flags);

		if (backBuffer != nullptr)
			backBuffer->reset(width, height);

		// Editor Scene/Viewport cameras render into window-owned targets whose
		// dimensions are unrelated to the OS backbuffer. Their resize lifecycle
		// is handled by the owning window, across frame boundaries. Rebuilding
		// them here creates a second allocation wave during every dock/OS resize.
		if (Engine::getSingleton()->getIsRuntimeMode())
			resetFrameBuffers();

		std::vector<GameObject*> gameObjects = Engine::getSingleton()->getGameObjects();
		for (auto& obj : gameObjects)
		{
			std::vector<Component*>& components = obj->getComponents();
			for (auto& cc : components)
				cc->onScreenResized(w, h);
		}
	}

	int Renderer::getWidth()
	{
		return width;
	}

	int Renderer::getHeight()
	{
		return height;
	}

	void Renderer::resetFrameBuffers()
	{
		const bool runtime = Engine::getSingleton()->getIsRuntimeMode();
		for (Camera* camera : cameras)
		{
			if (!camera->getAutomaticRendering())
				continue;
			if (runtime && camera->getIsEditorCamera())
				continue;
			if (!camera->getEnabled() || camera->gameObject == nullptr ||
				!camera->gameObject->getActive())
				continue;

			camera->resetFrameBuffers();
			if (!runtime)
				break;
		}
	}

	void Renderer::collectStats()
	{
		//Update stats
		const bgfx::Stats* stats = bgfx::getStats();
		double toMsCpu = 1000.0 / stats->cpuTimerFreq;
		cpuTime += double(stats->cpuTimeEnd - stats->cpuTimeBegin) * toMsCpu;

		double toMsGpu = 1000.0 / stats->gpuTimerFreq;
		gpuTime += double(stats->gpuTimeEnd - stats->gpuTimeBegin) * toMsGpu;

		frameMs += double(stats->cpuTimeFrame) * toMsCpu;
		numDrawCalls += stats->numDraw;
		numTriangles += stats->numPrims[bgfx::Topology::TriList];

		gpuMemUsed = (stats->textureMemoryUsed + stats->rtMemoryUsed) / 1000000;
	}

	std::string Renderer::addRenderCallback(std::function<void(int viewId, int viewLayer, Camera* camera)> callback, int queue)
	{
		RenderCallbackInfo inf;
		inf.cb = callback;
		inf.queue = queue;
		inf.id = GUIDGenerator::genGuid();

		renderCallbacks.push_back(inf);

		std::sort(renderCallbacks.begin(), renderCallbacks.end(), [=](RenderCallbackInfo& cb1, RenderCallbackInfo& cb2) -> bool
			{
				return cb1.queue > cb2.queue;
			}
		);

		return inf.id;
	}

	void Renderer::removeRenderCallback(std::string id)
	{
		auto it = std::find_if(renderCallbacks.begin(), renderCallbacks.end(), [=](RenderCallbackInfo& inf) -> bool
			{
				return inf.id == id;
			}
		);

		if (it != renderCallbacks.end())
			renderCallbacks.erase(it);
	}

	std::string Renderer::addPostRenderCallback(std::function<void()> callback)
	{
		PostRenderCallbackInfo inf;
		inf.cb = callback;
		inf.id = GUIDGenerator::genGuid();

		postRenderCallbacks.push_back(inf);

		return inf.id;
	}

	void Renderer::removePostRenderCallback(std::string id)
	{
		auto it = std::find_if(postRenderCallbacks.begin(), postRenderCallbacks.end(), [=](PostRenderCallbackInfo& inf) -> bool
			{
				return inf.id == id;
			}
		);

		if (it != postRenderCallbacks.end())
			postRenderCallbacks.erase(it);
	}

	void Renderer::uiProcessEvent(const void* event)
	{
		ImGui_ImplSDL2_ProcessEvent((const SDL_Event*)event);
	}

	void splitFrustum(float* _splits, uint8_t _numSplits, float _near, float _far, float _splitWeight = 0.75f)
	{
		const float l = _splitWeight;
		const float ratio = _far / _near;
		const int8_t numSlices = _numSplits * 2;

		// First slice. The values are view-space Z distances (negative for the
		// right-handed camera used by the renderer), so preserve the supplied
		// camera near plane instead of crossing behind the camera.
		_splits[0] = _near;

		for (uint8_t nn = 2, ff = 1; nn < numSlices; nn += 2, ff += 2)
		{
			// nn advances by two because every cascade stores a near/far pair.
			// The old ff/numSlices expression produced 1/8, 3/8, 5/8 for four
			// cascades instead of the required 1/4, 2/4, 3/4 distribution.
			float si = float(nn / 2) / float(_numSplits);

			const float nearp = l * (_near * bx::pow(ratio, si)) + (1 - l) * (_near + (_far - _near) * si);
			_splits[nn] = nearp;          //near
			_splits[ff] = nearp * 1.005f; //far from previous split
		}

		// Last slice.
		_splits[numSlices - 1] = _far;
	}

	void worldSpaceFrustumCorners(float* _corners24f, float _near, float _far, float _projWidth, float _projHeight, const float* _invViewMtx)
	{
		// Define frustum corners in view space.
		const float nw = _near * _projWidth;
		const float nh = _near * _projHeight;
		const float fw = _far * _projWidth;
		const float fh = _far * _projHeight;

		const uint8_t numCorners = 8;
		const bx::Vec3 corners[numCorners] =
		{
			{ -nw,  nh, _near },
			{  nw,  nh, _near },
			{  nw, -nh, _near },
			{ -nw, -nh, _near },
			{ -fw,  fh, _far  },
			{  fw,  fh, _far  },
			{  fw, -fh, _far  },
			{ -fw, -fh, _far  },
		};

		// Convert them to world space.
		float(*out)[3] = (float(*)[3])_corners24f;
		for (uint8_t ii = 0; ii < numCorners; ++ii)
		{
			bx::store(&out[ii], bx::mul(corners[ii], _invViewMtx));
		}
	}

	void Renderer::renderUI(std::vector<GameObject*>& gameObjects, glm::vec2 sizeOffset, bool updateMouse)
	{
		std::vector<Canvas*>& canvases = Renderer::getSingleton()->getCanvases();
		ImGuiWindow* win = ImGui::GetCurrentWindow();
		for (auto canv = canvases.begin(); canv != canvases.end(); ++canv)
		{
			Canvas* canvas = *canv;
			if (!canvas->getEnabled() || !canvas->getGameObject()->getActive())
				continue;

			canvas->setWindow(win);

			glm::vec2 refSize = canvas->getRefScreenSize();
			float delta = canvas->getScreenMatchSide();
			float zoom = 1.0f;

			if (canvas->getMode() == CanvasMode::ScaleToScreenSize)
			{
				zoom = (((win->Size.x + sizeOffset.x) / refSize.x) * (1.0f - delta)) + (((win->Size.y + sizeOffset.y) / refSize.y) * delta);
				canvas->setZoom(zoom);
			}

			std::vector<UIElement*>& uiElements = canvas->getUIElements();
			std::sort(uiElements.begin(), uiElements.end(), [=](UIElement* elem1, UIElement* elem2) -> bool
				{
					auto e1 = std::find(gameObjects.begin(), gameObjects.end(), elem1->getGameObject());
					auto e2 = std::find(gameObjects.begin(), gameObjects.end(), elem2->getGameObject());
					int64_t eidx1 = std::distance(gameObjects.begin(), e1) + (int)elem1->getTransform()->getPosition().z;
					int64_t eidx2 = std::distance(gameObjects.begin(), e2) + (int)elem2->getTransform()->getPosition().z;
					return eidx1 < eidx2;
				}
			);

			for (auto elem = uiElements.begin(); elem != uiElements.end(); ++elem)
			{
				UIElement* element = *elem;

				if (!element->getEnabled() || !element->getGameObject()->getActive())
					continue;

				if (element->getCanvas() == nullptr)
					continue;

				int pushCount = 0;
				Transform* par = element->getTransform()->getParent();
				while (par != nullptr)
				{
					GameObject* ob = par->getGameObject();
					Mask* mask = (Mask*)ob->getComponent(Mask::COMPONENT_TYPE);
					if (mask != nullptr)
					{
						UIElement::Properties& props = mask->getProperties();
						props.isEditor = false;

						++pushCount;
						glm::vec4 rect = mask->getRect();
						win->DrawList->PushClipRect(ImVec2(rect.x, rect.y), ImVec2(rect.z, rect.w), true);
					}

					par = par->getParent();
				}

				element->clearClipRects();
				UIElement::Properties& props = element->getProperties();

				auto& clipRects = props.clipRects;
				auto& clipRects1 = win->DrawList->_ClipRectStack;
				for (auto& clip : clipRects1)
					clipRects.push_back(glm::vec4(clip.x, clip.y, clip.z, clip.w));

				props.isEditor = false;
				element->onUpdate(Time::getDeltaTime());

				element->onRender(win->DrawList);

				while (pushCount > 0)
				{
					--pushCount;
					win->DrawList->PopClipRect();
				}
			}

			if (canvas->getMode() == CanvasMode::ScaleToScreenSize)
			{
				float dimX = ((win->Size.x / refSize.x) / zoom) * delta;
				float dimY = ((win->Size.y / refSize.y) / zoom) * (1.0f - delta);
				canvas->setRealScreenSize(glm::vec2(refSize.x * ((1.0f - delta) + dimX), refSize.y * (delta + dimY)), win);
			}
			else
				canvas->setRealScreenSize(glm::vec2(win->Size.x, win->Size.y), win);
		}
	}

	void Renderer::frame()
	{
	#ifndef FALCO_MODERN_BGFX
		SDL_GL_MakeCurrent((SDL_Window*)sdlWindow, context);
	#endif
		submittedFrameNumber = bgfx::frame();
#ifdef FALCO_MODERN_BGFX
		bgfxDiagnosticCallback.flushScreenshotResults();
#endif
	}

	//Single thread rendering
	void Renderer::renderFrame()
	{
		// Script compilation can request progress frames from a UI/post-render
		// callback. Never start another ImGui frame or GI capture inside this one.
		if (renderFrameInProgress)
			return;
		struct FrameScope
		{
			bool& active;
			explicit FrameScope(bool& flag) : active(flag) { active = true; }
			~FrameScope() { active = false; }
		} frameScope(renderFrameInProgress);

		// Realtime GI capture advances several bgfx frames synchronously.  It
		// must run before ImGui or any camera records commands for this frame;
		// invoking it from an editor callback split live Scene commands and could
		// crash D3D11 when its temporary readback resources were destroyed.
		if (environmentMapUpdatePending)
		{
			environmentMapUpdatePending = false;
			if (giEnabled)
				rebuildEnvironmentMap();
		}

		cpuTime = 0;
		gpuTime = 0;
		frameMs = 0;
		numDrawCalls = 0;
		numTriangles = 0;

		clearTransientRenderables();

		Camera::updateDynamicResolution();

		std::vector<GameObject*> gameObjects = Engine::getSingleton()->getGameObjects();
		std::vector<Camera*>& cameras = Renderer::getSingleton()->getCameras();
		std::vector<Light*>& lights = Renderer::getSingleton()->getLights();

		glm::mat4x4 screenView = glm::identity<glm::mat4x4>();
		glm::mat4x4 screenProj = glm::orthoRH(
			0.0f
			, 1.0f
			, 1.0f
			, 0.0f
			, 0.0f
			, 100.0f
		);

		std::sort(cameras.begin(), cameras.end(), [=](Camera* cam1, Camera* cam2) -> bool
			{
				return cam1->getDepth() < cam2->getDepth();
			}
		);

		//-->Render UI begin
		bgfx::setViewClear(RENDER_UI_PASS_ID, 0 | BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH | BGFX_CLEAR_STENCIL, Color::packRGBA(uiClearColor), 1.0f, 0);
		bgfx::setViewRect(RENDER_UI_PASS_ID, 0, 0, width, height);

		bgfx::setViewFrameBuffer(RENDER_UI_PASS_ID, BGFX_INVALID_HANDLE);

		ImGui_Implbgfx_NewFrame();
		ImGui_ImplSDL2_NewFrame();

		ImGui::NewFrame();

		if (uiCallback != nullptr)
			uiCallback();

		if (Engine::getSingleton()->getIsRuntimeMode())
		{
			bool isEditor = false;
			for (auto& cam : cameras)
			{
				if (cam->getIsEditorCamera())
				{
					isEditor = true;
					break;
				}
			}

			if (!isEditor)
			{
				bool open = true;
				ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
				ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
				ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
				ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
				ImGui::SetNextWindowSize(ImVec2((float)width, (float)height), ImGuiCond_Always);
				ImGui::Begin("BackbufferOutput", &open, ImGuiWindowFlags_NoBackground
					| ImGuiWindowFlags_NoBringToFrontOnFocus
					| ImGuiWindowFlags_NoCollapse
					| ImGuiWindowFlags_NoDecoration
					| ImGuiWindowFlags_NoDocking
					| ImGuiWindowFlags_NoFocusOnAppearing
					| ImGuiWindowFlags_NoInputs
					| ImGuiWindowFlags_NoMouseInputs
					| ImGuiWindowFlags_NoMove
					| ImGuiWindowFlags_NoNav
					| ImGuiWindowFlags_NoNavFocus
					| ImGuiWindowFlags_NoNavInputs
					| ImGuiWindowFlags_NoResize
					| ImGuiWindowFlags_NoSavedSettings
					| ImGuiWindowFlags_NoScrollbar
					| ImGuiWindowFlags_NoTitleBar
				);

				if (backBuffer != nullptr)
					ImGui::Image((void*)backBuffer->getColorTextureHandle().idx, ImVec2((float)width, (float)height), ImVec2(0, 1), ImVec2(1, 0));

				renderUI(gameObjects);

				ImGui::SetCursorPos(ImVec2(0, 0));

				APIManager::getSingleton()->execute("OnGUI");

				ImGui::End();
				ImGui::PopStyleVar(3);
			}
		}

		ImGui::Render();

		ImGui_Implbgfx_RenderDrawLists(ImGui::GetDrawData());

	#ifndef FALCO_MODERN_BGFX
		if (lights.size() > 0 || renderables.size() > 0)
			frame();
	#endif

		//-->Render UI end

		//----------Render point and spot light shadows----------//

		renderPointAndSpotLightShadows();

		//-------------------------------------------------------//

		int renderedCameraIndex = 0;
		const bool runtimeMode = Engine::getSingleton()->getIsRuntimeMode();
		for (Camera* camera : cameras)
		{
			// Scene 2 and Camera Viewport own reserved view groups and submit
			// themselves from their UI windows.  Keeping them out of this loop makes
			// the main Scene selection deterministic and prevents editor cameras from
			// replacing the Game camera in runtime mode.
			if (!camera->getAutomaticRendering())
				continue;
			if (runtimeMode)
			{
				if (camera->getIsEditorCamera())
					continue;
			}
			else if (!camera->getIsEditorCamera())
			{
				// The editor backbuffer belongs to the primary Scene camera.
				// Project cameras can sort ahead of it by Depth, but must only be
				// rendered by Camera Viewport while the editor is not playing.
				continue;
			}

			camera->renderFrame(renderedCameraIndex * CAMERA_VIEW_GROUP_SIZE);
			++renderedCameraIndex;

			if (!runtimeMode)
				break;
		}

		for (auto& obj : gameObjects)
		{
			if (!obj->getActive())
				continue;

			std::vector<Component*>& components = obj->getComponents();
			for (auto& cc : components)
			{
				if (cc->isUiComponent())
					continue;

				if (cc->getEnabled())
					cc->onUpdate(Time::getDeltaTime());
			}
		}

		for (auto& cb : postRenderCallbacks)
			cb.cb();

	#ifdef FALCO_MODERN_BGFX
		// Submit and present exactly once, after every UI, camera and post-render
		// command for this application frame has been recorded.  The legacy path
		// keeps its historical mid-frame/consumer-controlled behavior.
		frame();
	#endif

		lastCpuTime = cpuTime;
		lastGpuTime = gpuTime;
		lastFrameMs = frameMs;
		lastNumDrawCalls = numDrawCalls;
		lastNumTriangles = numTriangles;
	}

	void Renderer::renderObjects(Camera* camera, int viewLayer, int renderQueue, const glm::mat4x4& view, const glm::mat4x4& proj, const glm::mat4x4& skyMtx)
	{
		int objectView = RENDER_FORWARD_PASS_ID + viewLayer;
	#ifdef FALCO_MODERN_BGFX
		if (renderQueue == 1)
			objectView = RENDER_WATER_PASS_ID + viewLayer;
	#endif
		if (bgfx::getCaps()->limits.maxFBAttachments > 1 && bgfx::isValid(combinePH))
		{
			//-----------Render skybox-----------//

			if (renderQueue == 0)
				renderSkybox(RENDER_SKYBOX_PASS_ID + viewLayer, camera, skyMtx);

			//-----------Render forward-----------//

			std::vector<Renderable*> transparentObjects;

			//Render opaque

			for (auto it = renderables.begin(); it != renderables.end(); ++it)
			{
				Renderable* comp = *it;

				if (comp->getSkipRendering() || comp->isDecal())
					continue;

				if (comp->getRenderQueue() != renderQueue)
					continue;

				if (!comp->checkCullingMask(camera->getCullingMask()))
					continue;

				if (!comp->isAlwaysVisible())
				{
					if (!camera->isVisible(comp->getBounds()))
						continue;
				}

				if (!comp->isTransparent())
				{
					comp->onRender(camera, objectView, getRenderState(camera, defaultRenderState), { bgfx::kInvalidHandle }, static_cast<int>(RenderMode::Forward), [=]() {
						setSystemUniforms(camera);
						});
				}
				else
					transparentObjects.push_back(comp);
			}

			//Sort transparent
			glm::vec3 cameraPos = camera->getTransform()->getPosition();
			std::sort(transparentObjects.begin(), transparentObjects.end(), [=](Renderable* rend1, Renderable* rend2) -> bool
				{
					glm::vec3 pos1 = rend1->getBounds().getCenter();
					glm::vec3 pos2 = rend2->getBounds().getCenter();

					float dist1 = glm::distance(pos1, cameraPos);
					float dist2 = glm::distance(pos2, cameraPos);

					return dist1 > dist2;
				}
			);

			//Render transparent
			for (auto it = transparentObjects.begin(); it != transparentObjects.end(); ++it)
			{
				Renderable* comp = *it;

				comp->onRender(camera, objectView, getRenderState(camera, defaultRenderState), { bgfx::kInvalidHandle }, static_cast<int>(RenderMode::Forward), [=]() {
					setSystemUniforms(camera);
					});
			}

			transparentObjects.clear();
		}
	}

	void Renderer::renderRenderCallbacks(Camera* camera, int viewLayer)
	{
		int callbackView = RENDER_FORWARD_PASS_ID + viewLayer;
	#ifdef FALCO_MODERN_BGFX
		// Editor helpers must not enter the background sampled by Water.
		// bgfx executes by view ID, not by the order callbacks are recorded.
		if (camera->getIsEditorCamera())
			callbackView = RENDER_OVERLAY_PASS_ID + viewLayer;
	#endif
		if (bgfx::getCaps()->limits.maxFBAttachments > 1 && bgfx::isValid(combinePH))
		{
			for (auto cb = renderCallbacks.begin(); cb != renderCallbacks.end(); ++cb)
			{
				if (cb->cb != nullptr)
					cb->cb(callbackView, viewLayer, camera);
			}
		}
	}

	void Renderer::renderPointAndSpotLightShadows()
	{
		if (projectSettings != nullptr && projectSettings->getShadowsEnabled() && shadowsEnabled)
		{
			int lightIndex = 0;

			for (auto ll = lights.begin(); ll != lights.end(); ++ll)
			{
				Light* light = *ll;

				if (!light->getCastShadows())
					continue;
				if (light->getLightRenderMode() == LightRenderMode::Baked)
					continue;
				if (!light->getEnabled())
					continue;
				if (!light->getGameObject()->getActive())
					continue;

				int viewLayer = lightIndex * NUM_LIGHT_VIEWS;
				++lightIndex;

				if (light->getLightType() == LightType::Directional)
					continue;

				bgfx::setViewClear(RENDER_SHADOW_1_PASS_ID + viewLayer, 0 | BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, 0x000000ff, 1.0f, 0);
				bgfx::setViewClear(RENDER_SHADOW_2_PASS_ID + viewLayer, 0 | BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, 0x000000ff, 1.0f, 0);
				bgfx::setViewClear(RENDER_SHADOW_3_PASS_ID + viewLayer, 0 | BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, 0x000000ff, 1.0f, 0);
				bgfx::setViewClear(RENDER_SHADOW_4_PASS_ID + viewLayer, 0 | BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, 0x000000ff, 1.0f, 0);

				Transform* lightTransform = light->getGameObject()->getTransform();
				glm::vec3 lightPos = lightTransform->getPosition();

				bool lightVisible = false;
				int camIndex = 0;
				for (auto c = cameras.begin(); c != cameras.end(); c++, ++camIndex)
				{
					if (!Engine::getSingleton()->getIsRuntimeMode())
					{
						if (camIndex > 0)
							break;
					}

					if (!(*c)->getEnabled())
						continue;

					if ((*c)->gameObject == nullptr || !(*c)->gameObject->getActive())
						continue;

					Frustum* camFrustum = (*c)->getFrustum();
					if (camFrustum->sphereInFrustum(lightPos, light->getRadius()))
					{
						lightVisible = true;
						break;
					}
				}

				if (!lightVisible)
					continue;

				int shadowMapSize = light->getShadowMapSize();

				glm::mat4x4 lightView[4];
				glm::mat4x4 lightProj = glm::identity<glm::mat4x4>();
				glm::mat4x4 mtxYpr[4];

				glm::vec3 lightDir = lightTransform->getForward();
				glm::vec3 lightRight = lightTransform->getRight();

				glm::vec3 eye = glm::vec3(0, 0, 0);
				glm::vec3 at = eye + lightDir;
				lightView[0] = glm::lookAtRH(eye, at, glm::vec3(0, 1, 0));

				const int maxNumSplits = 4;
				int numSplits = 1;
				glm::mat4x4 mShadowMtx[4];
				for (int iii = 0; iii < 4; ++iii)
					mShadowMtx[iii] = glm::identity<glm::mat4x4>();

				glm::vec4 splitDistances = glm::vec4(0, 0, 0, 0);

				if (light->getLightType() == LightType::Point)
				{
					numSplits = 4;

					float ypr[4][3] =
					{
						// Keep this Green/Yellow/Blue/Red order synchronized with the
						// tetrahedral face-selection normals in shadows.sh.
						{ bx::toRad(0.0f), bx::toRad(27.36780516f), bx::toRad(0.0f) },
						{ bx::toRad(180.0f), bx::toRad(27.36780516f), bx::toRad(0.0f) },
						{ bx::toRad(-90.0f), bx::toRad(-27.36780516f), bx::toRad(0.0f) },
						{ bx::toRad(90.0f), bx::toRad(-27.36780516f), bx::toRad(0.0f) },
					};

					const float fovx = 143.98570868f + 7.8f + 0; //-20..20
					const float fovy = 125.26438968f + 3.0f + 0; //-20..20
					const float aspect = bx::tan(bx::toRad(fovx * 0.5f)) / bx::tan(bx::toRad(fovy * 0.5f));
					const float zfar = glm::max(light->getRadius(), 0.02f);
					// Point lights are commonly placed close to nearby geometry. A 1%-of-
					// Range near plane becomes 0.5 m at Range 50 and clips different parts
					// of a caster in adjacent tetrahedral faces. Keep a 1000:1 depth ratio
					// while allowing centimetre-scale placement near a surface.
					const float znear = glm::min(glm::max(zfar * 0.001f, 0.01f), zfar * 0.5f);
					const bgfx::Caps* caps = bgfx::getCaps();
					const bool homogeneousDepth = caps != nullptr && caps->homogeneousDepth;
					float projection[16];
					bx::mtxProj(projection, fovy, aspect, znear, zfar,
						homogeneousDepth, bx::Handedness::Left);
					lightProj = glm::make_mat4(projection);

					for (uint8_t ii = 0; ii < 4; ++ii)
					{
						glm::mat4x4 mtxTmp = glm::yawPitchRoll(ypr[ii][0], ypr[ii][1], ypr[ii][2]);

						float tmp[3] =
						{
							-glm::dot(lightPos, glm::make_vec3(&mtxTmp[0].x)),
							-glm::dot(lightPos, glm::make_vec3(&mtxTmp[1].x)),
							-glm::dot(lightPos, glm::make_vec3(&mtxTmp[2].x))
						};

						mtxYpr[ii] = glm::transpose(mtxTmp);

						memcpy(glm::value_ptr(lightView[ii]), glm::value_ptr(mtxYpr[ii]), 12 * sizeof(float));
						lightView[ii][3].x = tmp[0];
						lightView[ii][3].y = tmp[1];
						lightView[ii][3].z = tmp[2];
						lightView[ii][3].w = 1.0f;
					}

					const bool originBottomLeft = caps != nullptr && caps->originBottomLeft;
					const float s = originBottomLeft ? 1.0f : -1.0f;
					const float zscale = homogeneousDepth ? 0.5f : 1.0f;
					const float zadd = homogeneousDepth ? 0.5f : 0.0f;

					const float mtxCropBias[4][16] =
					{
						{ // D3D: Green, OGL: Blue
							0.25f,    0.0f, 0.0f, 0.0f,
							0.0f, s * 0.25f, 0.0f, 0.0f,
							0.0f,    0.0f, zscale, 0.0f,
							0.25f,   0.25f, zadd, 1.0f,
						},
						{ // D3D: Yellow, OGL: Red
							0.25f,    0.0f, 0.0f, 0.0f,
							0.0f, s * 0.25f, 0.0f, 0.0f,
							0.0f,    0.0f, zscale, 0.0f,
							0.75f,   0.25f, zadd, 1.0f,
						},
						{ // D3D: Blue, OGL: Green
							0.25f,    0.0f, 0.0f, 0.0f,
							0.0f, s * 0.25f, 0.0f, 0.0f,
							0.0f,    0.0f, zscale, 0.0f,
							0.25f,   0.75f, zadd, 1.0f,
						},
						{ // D3D: Red, OGL: Yellow
							0.25f,    0.0f, 0.0f, 0.0f,
							0.0f, s * 0.25f, 0.0f, 0.0f,
							0.0f,    0.0f, zscale, 0.0f,
							0.75f,   0.75f, zadd, 1.0f,
						},
					};

					static const uint8_t cropBiasIndices[2][4] =
					{
						{ 0, 1, 2, 3 }, // top-left render-target origin (D3D)
						{ 2, 3, 0, 1 }, // bottom-left render-target origin (OpenGL)
					};

					for (uint8_t ii = 0; ii < 4; ++ii)
					{
						uint8_t biasIndex = cropBiasIndices[originBottomLeft ? 1 : 0][ii];

						float mtxTmp[16];
						bx::mtxMul(mtxTmp, glm::value_ptr(mtxYpr[ii]), glm::value_ptr(lightProj));
						bx::mtxMul(glm::value_ptr(mShadowMtx[ii]), mtxTmp, mtxCropBias[biasIndex]); //mtxYprProjBias
					}
				}

				if (light->getLightType() == LightType::Spot)
				{
					numSplits = 1;

					// The Spot outer angle is the full light cone angle. Keep the shadow
					// camera on the same cone and range so illuminated pixels and shadow
					// coverage cannot drift apart when one editor unit means one metre.
					const float fovyDegrees = glm::clamp(light->getOuterRadius(), 1.0f, 179.0f);
					const float aspect = 1.0f;
					const float zfar = glm::max(light->getRadius(), 0.02f);
					const float znear = glm::min(glm::max(zfar * 0.01f, 0.01f), zfar * 0.5f);
					const bgfx::Caps* caps = bgfx::getCaps();
					const bool homogeneousDepth = caps != nullptr && caps->homogeneousDepth;

					float projection[16];
					bx::mtxProj(projection, fovyDegrees, aspect, znear, zfar,
						homogeneousDepth, bx::Handedness::Right);
					lightProj = glm::make_mat4(projection);

					lightView[0] = glm::lookAtRH(lightPos, lightPos + lightDir, glm::vec3(0, 1, 0));

					const float ymul = caps != nullptr && caps->originBottomLeft ? 0.5f : -0.5f;
					const float zscale = homogeneousDepth ? 0.5f : 1.0f;
					const float zadd = homogeneousDepth ? 0.5f : 0.0f;

					const float mtxBias[16] =
					{
						0.5f, 0.0f, 0.0f, 0.0f,
						0.0f, ymul, 0.0f, 0.0f,
						0.0f, 0.0f, zscale, 0.0f,
						0.5f, 0.5f, zadd, 1.0f,
					};

					float mtxTmp[16];

					bx::mtxMul(mtxTmp, glm::value_ptr(lightProj), mtxBias);
					bx::mtxMul(glm::value_ptr(mShadowMtx[0]), glm::value_ptr(lightView[0]), mtxTmp); //lViewProjCropBias
				}

				int numShadowMaps = light->getNumShadowMaps();
				if (numShadowMaps > 0)
				{
					if (light->getLightType() == LightType::Point)
					{
						const uint16_t h = shadowMapSize / 2; //half size
						bgfx::setViewRect(RENDER_SHADOW_1_PASS_ID + viewLayer, 0, 0, h, h);
						bgfx::setViewRect(RENDER_SHADOW_2_PASS_ID + viewLayer, h, 0, h, h);
						bgfx::setViewRect(RENDER_SHADOW_3_PASS_ID + viewLayer, 0, h, h, h);
						bgfx::setViewRect(RENDER_SHADOW_4_PASS_ID + viewLayer, h, h, h, h);

						for (int split = 0; split < 4; ++split)
						{
							bgfx::setViewTransform(RENDER_SHADOW_1_PASS_ID + split + viewLayer, glm::value_ptr(lightView[split]), glm::value_ptr(lightProj));
							bgfx::setViewFrameBuffer(RENDER_SHADOW_1_PASS_ID + split + viewLayer, light->shadowMapFB[0]);
							bgfx::touch(RENDER_SHADOW_1_PASS_ID + split + viewLayer);
						}
					}

					if (light->getLightType() == LightType::Spot)
					{
						bgfx::setViewRect(RENDER_SHADOW_1_PASS_ID + viewLayer, 0, 0, shadowMapSize, shadowMapSize);
						bgfx::setViewTransform(RENDER_SHADOW_1_PASS_ID + viewLayer, glm::value_ptr(lightView[0]), glm::value_ptr(lightProj));
						bgfx::setViewFrameBuffer(RENDER_SHADOW_1_PASS_ID + viewLayer, light->shadowMapFB[0]);

						bgfx::touch(RENDER_SHADOW_1_PASS_ID + viewLayer);
					}
				}

				light->mShadowMtx[0] = mShadowMtx[0];
				light->mShadowMtx[1] = mShadowMtx[1];
				light->mShadowMtx[2] = mShadowMtx[2];
				light->mShadowMtx[3] = mShadowMtx[3];

				for (int split = 0; split < numSplits; ++split)
					light->mShadowFrustum[split]->calculateFrustum(lightView[split], lightProj);

				for (auto it = renderables.begin(); it != renderables.end(); ++it)
				{
					Renderable* comp = *it;

					if (comp->getSkipRendering() || comp->isDecal())
						continue;

					if (!comp->getCastShadows())
						continue;

					if (light->getLightRenderMode() == LightRenderMode::Mixed)
					{
						if (comp->isStatic())
							continue;
					}

					AxisAlignedBox aab = comp->getBounds();

					if (!Mathf::intersects(lightPos, light->getRadius(), aab))
						continue;

					// A local-light shadow caster must not be culled by the main camera
					// frustum.  An off-screen object can still cast a shadow onto a
					// visible receiver.  The light-volume test above and the shadow-map
					// frustum test below provide the correct spatial bounds.
					bool isVisible = false;
					camIndex = 0;
					for (auto cam = cameras.begin(); cam != cameras.end(); ++cam, ++camIndex)
					{
						Camera* camera = *cam;

						if (!Engine::getSingleton()->getIsRuntimeMode())
						{
							if (camIndex > 0)
								break;
						}

						if (!camera->getEnabled())
							continue;

						if (camera->gameObject == nullptr || !camera->gameObject->getActive())
							continue;

						if (!comp->checkCullingMask(camera->getCullingMask()))
							continue;

						Frustum* camFrustum = camera->getFrustum();
						if (!camFrustum->sphereInFrustum(lightPos, light->getRadius()))
							continue;

						isVisible = true;
						break;

						if (!Engine::getSingleton()->getIsRuntimeMode())
							break;
					}

					if (!isVisible)
						continue;

					for (uint8_t ii = 0; ii < numSplits; ++ii)
					{
						if (!light->mShadowFrustum[ii]->sphereInFrustum(aab.getCenter(), aab.getRadius()))
							continue;

						/*Camera lightCamera;
						lightCamera.injectView = true;
						lightCamera.injectProj = true;
						lightCamera.injectedViewMatrix = lightView[ii];
						lightCamera.injectedProjectionMatrix = lightProj;
						lightCamera.transform = lightTransform;*/

						const uint8_t viewId = RENDER_SHADOW_1_PASS_ID + viewLayer + ii;
						comp->onRender(nullptr, viewId, shadowRenderState, shadowCasterPH, static_cast<int>(RenderMode::Forward), [=]()
							{
								setSystemUniforms(nullptr);
								light->submitUniforms();
							}
						);
					}
				}
			}
		}
	}

	void Renderer::renderDirectionalLightShadows(Camera* camera, const glm::mat4x4& invView, int viewOffset)
	{
		if (projectSettings != nullptr && projectSettings->getShadowsEnabled() && shadowsEnabled)
		{
			const float projHeight = bx::tan(bx::toRad(camera->getFOVy()) * 0.5f);
			const float projWidth = projHeight * camera->getAspectRatio();

			int lightIndex = 0;

			for (auto ll = lights.begin(); ll != lights.end(); ++ll)
			{
				Light* light = *ll;

				if (!light->getCastShadows())
					continue;
				if (light->getLightRenderMode() == LightRenderMode::Baked)
					continue;
				if (!light->getEnabled())
					continue;
				if (!light->getGameObject()->getActive())
					continue;

				int viewLayer = viewOffset + lightIndex * NUM_LIGHT_VIEWS;
				++lightIndex;

				if (light->getLightType() != LightType::Directional)
					continue;

				bgfx::setViewClear(RENDER_SHADOW_1_PASS_ID + viewLayer, 0 | BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, 0x000000ff, 1.0f, 0);
				bgfx::setViewClear(RENDER_SHADOW_2_PASS_ID + viewLayer, 0 | BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, 0x000000ff, 1.0f, 0);
				bgfx::setViewClear(RENDER_SHADOW_3_PASS_ID + viewLayer, 0 | BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, 0x000000ff, 1.0f, 0);
				bgfx::setViewClear(RENDER_SHADOW_4_PASS_ID + viewLayer, 0 | BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, 0x000000ff, 1.0f, 0);

				Transform* lightTransform = light->getGameObject()->getTransform();

				int shadowMapSize = light->getShadowMapSize();

				glm::mat4x4 lightView = glm::identity<glm::mat4x4>();
				glm::mat4x4 lightProj[4];

				glm::vec3 camPos = camera->getTransform()->getPosition();
				glm::vec3 lightDir = lightTransform->getForward();

				glm::vec3 eye = camPos;
				glm::vec3 at = eye + lightDir;
				// Keep the light basis valid when a Directional Light points almost
				// straight up/down. A fixed world-up vector makes lookAt singular in
				// that orientation and turns tiny rotation errors into unstable CSMs.
				const glm::vec3 lightUp = glm::abs(glm::dot(lightDir, glm::vec3(0, 1, 0))) > 0.999f
					? glm::vec3(0, 0, 1)
					: glm::vec3(0, 1, 0);
				lightView = glm::lookAtRH(eye, at, lightUp);

				const int maxNumSplits = 4;
				int numSplits = 1;
				glm::mat4x4 mShadowMtx[4];
				for (int iii = 0; iii < 4; ++iii)
					mShadowMtx[iii] = glm::identity<glm::mat4x4>();
				glm::vec4 splitDistances = glm::vec4(0, 0, 0, 0);

				numSplits = projectSettings->getShadowCascadesCount();
				const float cascadeNear = glm::max(camera->getNear(), 0.01f);
				const float requestedShadowDistance = glm::max(projectSettings->getShadowDistance(), cascadeNear + 0.01f);
				const float cascadeFar = glm::min(requestedShadowDistance, glm::max(camera->getFar(), cascadeNear + 0.01f));
				float splitSlices[maxNumSplits * 2];
				splitFrustum(splitSlices
					, uint8_t(numSplits)
					, -cascadeNear
					, -cascadeFar
					, 0.6f
				);

				// Keep shader split distances at the nominal boundaries. splitSlices
				// stores a small extension in the preceding cascade's far value, so
				// take every intermediate boundary from the next cascade's near value.
				for (uint8_t ii = 0, ff = 1; ii < numSplits; ++ii, ff += 2)
				{
					// splitFrustum stores right-handed view-space Z (negative in
					// front of the camera).  Shaders select cascades using positive
					// depth along the camera forward vector.
					splitDistances[ii] = ii + 1 < numSplits
						? -splitSlices[(ii + 1) * 2]
						: -splitSlices[ff];
				}
				light->mShadowCascadeSplits = splitDistances;

				// Extend each outer cascade towards the camera. The shader blends to
				// that cascade over the final 10% of the preceding depth interval,
				// removing the hard full-screen band/pop at a CSM boundary.
				for (int ii = 1; ii < numSplits; ++ii)
				{
					const float previousSplit = splitDistances[ii - 1];
					const float currentSplit = splitDistances[ii];
					const float transitionWidth = glm::max((currentSplit - previousSplit) * 0.1f, 0.25f);
					splitSlices[ii * 2] = -(previousSplit - transitionWidth);
				}

				glm::mat4x4 mtxProj;
				// Directional maps use D24S8. Start with a local depth span for good
				// precision, then grow it to contain every caster which the render loop
				// below can submit. This matters for Terrain: one large AABB may touch
				// the camera shadow sphere while vertices on its far side lie outside a
				// fixed light-space range. They used to cross the depth clip plane as
				// the camera moved, making a large terrain shadow blink on/off.
				const float shadowDistance = projectSettings->getShadowDistance();
				float shadowDepthExtent = glm::max(shadowDistance * 2.0f, 10.0f);
				for (Renderable* comp : renderables)
				{
					if (comp->getSkipRendering() || comp->isDecal() || !comp->getCastShadows())
						continue;
					if (light->getLightRenderMode() == LightRenderMode::Mixed && comp->isStatic())
						continue;
					if (!comp->checkCullingMask(camera->getCullingMask()))
						continue;

					AxisAlignedBox casterBounds = comp->getBounds();
					if (casterBounds.isNull() || casterBounds.isInfinite())
						continue;
					if (!Mathf::intersects(camPos, shadowDistance, casterBounds))
						continue;

					for (const glm::vec3& corner : casterBounds.getAllCorners())
					{
						const glm::vec4 lightSpaceCorner = lightView * glm::vec4(corner, 1.0f);
						shadowDepthExtent = glm::max(shadowDepthExtent, glm::abs(lightSpaceCorner.z) + 1.0f);
					}
				}

				// Avoid changing the depth projection continuously for sub-unit camera
				// movement. D24 retains ample precision at this one-unit quantization.
				shadowDepthExtent = glm::ceil(shadowDepthExtent);
			#ifdef FALCO_MODERN_BGFX
				const bgfx::Caps* caps = bgfx::getCaps();
				const bool homogeneousDepth = caps != nullptr && caps->homogeneousDepth;
				float projection[16];
				bx::mtxOrtho(
					projection
					, 1.0f
					, -1.0f
					, 1.0f
					, -1.0f
					, -shadowDepthExtent
					, shadowDepthExtent
					, 0.0f
					, homogeneousDepth
					, bx::Handedness::Right
				);
				mtxProj = glm::make_mat4(projection);
			#else
				mtxProj = glm::orthoRH(
					1.0f
					, -1.0f
					, 1.0f
					, -1.0f
					, -shadowDepthExtent
					, shadowDepthExtent
				);
			#endif

				const uint8_t numCorners = 8;
				float frustumCorners[maxNumSplits][numCorners][3];
				for (uint8_t ii = 0, nn = 0, ff = 1; ii < numSplits; ++ii, nn += 2, ff += 2)
				{
					// Compute frustum corners for one split in world space.
					worldSpaceFrustumCorners((float*)frustumCorners[ii], splitSlices[nn], splitSlices[ff], projWidth, projHeight, glm::value_ptr(invView));

					bx::Vec3 min = { SHADOW_AAB_VAL, SHADOW_AAB_VAL, SHADOW_AAB_VAL };
					bx::Vec3 max = { -SHADOW_AAB_VAL, -SHADOW_AAB_VAL, -SHADOW_AAB_VAL };

					for (uint8_t jj = 0; jj < numCorners; ++jj)
					{
						// Transform to light space.
						const bx::Vec3 xyz = bx::mul(bx::load<bx::Vec3>(frustumCorners[ii][jj]), glm::value_ptr(lightView));

						// Update bounding box.
						min = bx::min(min, xyz);
						max = bx::max(max, xyz);
					}

					const bx::Vec3 minproj = bx::mulH(min, glm::value_ptr(mtxProj));
					const bx::Vec3 maxproj = bx::mulH(max, glm::value_ptr(mtxProj));

					float scalex = 2.0f / (maxproj.x - minproj.x);
					float scaley = 2.0f / (maxproj.y - minproj.y);

					//if (m_settings.m_stabilize)
					//{
					const float quantizer = 64.0f;
					scalex = quantizer / bx::ceil(quantizer / scalex);
					scaley = quantizer / bx::ceil(quantizer / scaley);
					//}

					float offsetx = 0.5f * (maxproj.x + minproj.x) * scalex;
					float offsety = 0.5f * (maxproj.y + minproj.y) * scaley;

					//if (m_settings.m_stabilize)
					//{
					const float halfSize = shadowMapSize * 0.5f;
					offsetx = bx::ceil(offsetx * halfSize) / halfSize;
					offsety = bx::ceil(offsety * halfSize) / halfSize;
					//}

					float mtxCrop[16];
					bx::mtxIdentity(mtxCrop);
					mtxCrop[0] = scalex;
					mtxCrop[5] = scaley;
					mtxCrop[12] = offsetx;
					mtxCrop[13] = offsety;

					bx::mtxMul(glm::value_ptr(lightProj[ii]), mtxCrop, glm::value_ptr(mtxProj));
				}

			#ifdef FALCO_MODERN_BGFX
				const float ymul = caps != nullptr && caps->originBottomLeft ? 0.5f : -0.5f;
				const float zscale = homogeneousDepth ? 0.5f : 1.0f;
				const float zadd = homogeneousDepth ? 0.5f : 0.0f;
			#else
				const float ymul = 0.5f;
				const float zscale = 0.5f;
				const float zadd = 0.5f;
			#endif

				const float mtxBias[16] =
				{
					0.5f, 0.0f, 0.0f, 0.0f,
					0.0f, ymul, 0.0f, 0.0f,
					0.0f, 0.0f, zscale, 0.0f,
					0.5f, 0.5f, zadd, 1.0f,
				};

				for (uint8_t ii = 0; ii < numSplits; ++ii)
				{
					float mtxTmp[16];

					bx::mtxMul(mtxTmp, glm::value_ptr(lightProj[ii]), mtxBias);
					bx::mtxMul(glm::value_ptr(mShadowMtx[ii]), glm::value_ptr(lightView), mtxTmp); //lViewProjCropBias
				
					bgfx::setViewRect(RENDER_SHADOW_1_PASS_ID + ii + viewLayer, 0, 0, shadowMapSize, shadowMapSize);
					bgfx::setViewTransform(RENDER_SHADOW_1_PASS_ID + ii + viewLayer, glm::value_ptr(lightView), glm::value_ptr(lightProj[ii]));
					bgfx::setViewFrameBuffer(RENDER_SHADOW_1_PASS_ID + ii + viewLayer, light->shadowMapFB[ii]);
					bgfx::touch(RENDER_SHADOW_1_PASS_ID + ii + viewLayer);
				}

				light->mShadowMtx[0] = mShadowMtx[0];
				light->mShadowMtx[1] = mShadowMtx[1];
				light->mShadowMtx[2] = mShadowMtx[2];
				light->mShadowMtx[3] = mShadowMtx[3];

				for (int split = 0; split < numSplits; ++split)
					light->mShadowFrustum[split]->calculateFrustum(lightView, lightProj[split]);

				for (auto it = renderables.begin(); it != renderables.end(); ++it)
				{
					Renderable* comp = *it;

					if (comp->getSkipRendering() || comp->isDecal())
						continue;

					if (!comp->getCastShadows())
						continue;

					if (light->getLightRenderMode() == LightRenderMode::Mixed)
					{
						if (comp->isStatic())
							continue;
					}

					if (!comp->checkCullingMask(camera->getCullingMask()))
						continue;

					/*if (!comp->isAlwaysVisible())
					{
						if (!camera->isVisible(comp->getBounds(), 2.2f))
							continue;
					}*/

					AxisAlignedBox aab = comp->getBounds();

					if (aab.isNull() || aab.isInfinite())
						continue;

					if (!Mathf::intersects(camPos, projectSettings->getShadowDistance(), aab))
						continue;

					for (uint8_t ii = 0; ii < numSplits; ++ii)
					{
						if (!light->mShadowFrustum[ii]->sphereInFrustum(aab.getCenter(), aab.getRadius()))
							continue;

						/*Camera lightCamera;
						lightCamera.injectView = true;
						lightCamera.injectProj = true;
						lightCamera.injectedViewMatrix = lightView;
						lightCamera.injectedProjectionMatrix = lightProj[ii];
						lightCamera.transform = lightTransform;*/

						const uint8_t viewId = RENDER_SHADOW_1_PASS_ID + viewLayer + ii;
						// Thin walls must cast from either side, as in the baker.
						comp->onRender(nullptr, viewId, shadowRenderState & ~BGFX_STATE_CULL_MASK, shadowCasterPH, static_cast<int>(RenderMode::Forward), [=]()
							{
								setSystemUniforms(camera);
								light->submitUniforms();
							}
						);
					}
				}
			}
		}
	}

	void Renderer::renderSkybox(int skyView, Camera* camera, const glm::mat4x4& skyMtx, std::function<void()> viewUniforms)
	{
		if (camera != nullptr)
		{
			if (camera->getClearFlags() != Camera::ClearFlags::Skybox)
				return;
		}

		//-->Render skybox begin
		if (skyMaterial != nullptr)
		{
			Shader* shader = nullptr;
			if (skyMaterial->isLoaded())
				shader = skyMaterial->getShader();

			if (shader != nullptr && shader->isLoaded())
			{
				int passCount = shader->getPassCount();

				for (int j = 0; j < passCount; ++j)
				{
					Pass* pass = shader->getPass(j);

					ProgramVariant* pv = nullptr;
					if (pass != nullptr)
						pv = pass->getProgramVariant(skyMaterial->getDefinesStringHash());

					uint64_t passState = BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A;

					if (pv != nullptr)
						passState = pv->getRenderState(passState);

					// Pass uniforms
					if (pv != nullptr)
						skyMaterial->submitUniforms(pv, camera);

					//Bind system uniforms
					setSystemUniforms(camera);

					if (viewUniforms)
						viewUniforms();

					// Render
					bgfx::ProgramHandle ph = { bgfx::kInvalidHandle };
					if (pv != nullptr)
						ph = pv->programHandle;

					if (skyModel == SkyModel::Box)
						Primitives::skybox(skyMtx, skyView, passState, ph, camera);
					else
						Primitives::sphere(skyMtx, 48, skyView, passState, ph, camera);
				}
			}
		}
		//-->Render skybox end
	}

	void Renderer::setViewCameraUniforms(const glm::mat4x4& view, const glm::mat4x4& projection, const glm::vec3& position, int targetWidth, int targetHeight)
	{
		glm::mat4x4 inverseView = glm::inverse(view);
		glm::mat4x4 inverseProjection = glm::inverse(projection);
		glm::mat4x4 viewProjection = projection * view;
		glm::mat4x4 inverseVP = glm::inverse(viewProjection);
		glm::vec4 cameraPosition(position, 0.0f);
		glm::vec4 cameraDirection(-glm::vec3(inverseView[2]), 0.0f);
		glm::vec4 screen(static_cast<float>(targetWidth), static_cast<float>(targetHeight), static_cast<float>(width), static_cast<float>(height));
		bgfx::setUniform(uCamPos, glm::value_ptr(cameraPosition));
		bgfx::setUniform(uCamDir, glm::value_ptr(cameraDirection));
		bgfx::setUniform(uCamView, glm::value_ptr(view));
		bgfx::setUniform(uCamProj, glm::value_ptr(projection));
		bgfx::setUniform(uCamInvView, glm::value_ptr(inverseView));
		bgfx::setUniform(uCamInvProj, glm::value_ptr(inverseProjection));
		bgfx::setUniform(u_VP, glm::value_ptr(viewProjection));
		bgfx::setUniform(u_invVP, glm::value_ptr(inverseVP));
		bgfx::setUniform(uScreenParams, glm::value_ptr(screen));
	}

    void Renderer::setPreviewUniforms()
    {
        setSystemUniforms(nullptr);
        // Asset previews have their own neutral studio, independent of the scene.
        const glm::vec4 disabled(0.0f);
        bgfx::setUniform(uGIParams, glm::value_ptr(disabled));
        bgfx::setUniform(uSSAOParams, glm::value_ptr(disabled));
        bgfx::setUniform(uCloudShadowParams, glm::value_ptr(disabled));
        bgfx::setUniform(uFogEnabled, glm::value_ptr(disabled));
        const glm::vec4 ambient(0.6f, 0.6f, 0.6f, 1.0f);
        bgfx::setUniform(uAmbientColor, glm::value_ptr(ambient));
        const auto* caps = bgfx::getCaps();
        const glm::vec4 origin(caps && !caps->originBottomLeft ? 1.0f : 0.0f,
            caps && caps->homogeneousDepth ? 1.0f : 0.0f, 0.0f, 0.0f);
        bgfx::setUniform(uRenderTargetOrigin, glm::value_ptr(origin));
    }

	void Renderer::setSystemUniforms(Camera* camera)
	{
		if (camera != nullptr)
		{
			Transform* cameraTransform = camera->getTransform();

			Color clearColor = camera->getClearColor();
			if (skyMaterial != nullptr)
				clearColor[3] = 0.0f;
			else
				clearColor[3] = 1.0f;

			float w = width;
			float h = height;

			if (camera->sceneBuffer != nullptr)
			{
				w = camera->sceneBuffer->getWidth();
				h = camera->sceneBuffer->getHeight();
			}

			//if (Engine::getSingleton()->getIsRuntimeMode())
			//{
			RenderTexture* rt = camera->getRenderTarget();
			if (rt != nullptr)
			{
				w = rt->getWidth();
				h = rt->getHeight();
			}
			//}

			bgfx::setUniform(uCamPos, glm::value_ptr(glm::vec4(cameraTransform->getPosition(), 0.0f)), 1);
			bgfx::setUniform(uCamDir, glm::value_ptr(glm::vec4(cameraTransform->getForward(), 0.0f)), 1);
			bgfx::setUniform(uCamFov, glm::value_ptr(glm::vec4(camera->getFOVy(), 1.0f / camera->getFOVy(), 0.0f, 0.0f)), 1);
			bgfx::setUniform(uCamView, glm::value_ptr(camera->getViewMatrix()), 1);
			bgfx::setUniform(uCamProj, glm::value_ptr(camera->getProjectionMatrix()), 1);
			bgfx::setUniform(uCamInvView, glm::value_ptr(glm::inverse(camera->getViewMatrix())), 1);
			bgfx::setUniform(uCamInvProj, glm::value_ptr(glm::inverse(camera->getProjectionMatrix())), 1);
			bgfx::setUniform(uCamClearColor, clearColor.toLinear().ptr(), 1);
			bgfx::setUniform(uClipPlane, glm::value_ptr(glm::vec4(camera->getNear(), camera->getFar(), 0.0f, 0.0f)), 1);
			bgfx::setUniform(uScreenParams, glm::value_ptr(glm::vec4(w, h, width, height)), 1);
			const bgfx::Caps* caps = bgfx::getCaps();
			const float flipRenderTargetV = caps != nullptr && !caps->originBottomLeft ? 1.0f : 0.0f;
			const float homogeneousDepth = caps != nullptr && caps->homogeneousDepth ? 1.0f : 0.0f;
			bgfx::setUniform(uRenderTargetOrigin, glm::value_ptr(glm::vec4(flipRenderTargetV, homogeneousDepth, 0.0f, 0.0f)), 1);
		}

        glm::vec4 cloudParams(0.0f);
        glm::vec4 cloudWind(0.0f);
        glm::vec4 cloudLayer(600.0f, 350.0f, 0.003f, 6.0f);
        for (Renderable* renderable : renderables)
        {
            CloudSystem* cloud = dynamic_cast<CloudSystem*>(renderable);
            if (!cloud || cloud->getSkipRendering() || !cloud->getMaterial()
                || !cloud->getMaterial()->isLoaded()) continue;
            if (camera && !cloud->checkCullingMask(camera->getCullingMask())) continue;
            cloudParams = glm::vec4(glm::clamp(cloud->coverage, 0.0f, 1.0f),
                glm::clamp(cloud->density, 0.0f, 1.0f), 1.0f, Time::getTimeSinceLevelStartScaled());
            cloudWind = glm::vec4(cloud->wind, 0.0f);
            cloudLayer = cloud->getLayerParameters();
            break; // One global sky layer; deterministic first active CloudSystem.
        }
        bgfx::setUniform(uCloudShadowParams, glm::value_ptr(cloudParams));
        bgfx::setUniform(uCloudShadowWind, glm::value_ptr(cloudWind));
        bgfx::setUniform(uCloudLayer, glm::value_ptr(cloudLayer));
        const glm::vec4 ssaoParams(projectSettings->getSSAORadius(), projectSettings->getSSAOIntensity(),
            projectSettings->getSSAOBias(), camera != nullptr && projectSettings->getSSAO() ? float(projectSettings->getSSAOSamples()) : 0.0f);
        bgfx::setUniform(uSSAOParams, glm::value_ptr(ssaoParams));
		bgfx::setUniform(uAmbientColor, ambientColor.toLinear().ptr(), 1);
		bgfx::setUniform(uTime, glm::value_ptr(glm::vec4(Time::getTimeSinceLevelStart(), 0.0f, 0.0f, 0.0f)), 1);
		bgfx::setUniform(uScaledTime, glm::value_ptr(glm::vec4(Time::getTimeSinceLevelStartScaled(), 0.0f, 0.0f, 0.0f)), 1);

		bgfx::setUniform(uUseFXAA, glm::value_ptr(glm::vec4(projectSettings->getFXAA() ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f)), 1);
		bgfx::setUniform(uFogEnabled, glm::value_ptr(glm::vec4(fogEnabled ? 1.0f : 0.0f, fogIncludeSkybox ? 1.0f : 0.0f, 0.0f, 0.0f)), 1);
		bgfx::setUniform(uFogParams, glm::value_ptr(glm::vec4(fogStartDistance, fogEndDistance, fogDensity, fogType)), 1);
		bgfx::setUniform(uFogColor, fogColor.toLinear().ptr(), 1);

		if (environmentMap != nullptr)
			bgfx::setTexture(9, uEnvMap, environmentMap->getHandle());
		else
			bgfx::setTexture(9, uEnvMap, Cubemap::getNullCubemap()->getHandle());


		float enbl = giEnabled ? 1.0f : 0.0f;
		float hasEnvironmentMap = environmentMap != nullptr ? 1.0f : 0.0f;
		bgfx::setUniform(uGIParams, glm::value_ptr(glm::vec4(enbl, giIntensity, hasEnvironmentMap, 0.0f)), 1);
	}

    void Renderer::setEnvironmentCacheUniforms(bool terrainLayout)
    {
        // Terrain reserves 0..11 for its layers; only its ambient pass uses 12..15 for IBL.
        if (terrainLayout)
            bgfx::setTexture(15, uEnvMap, environmentMap != nullptr && bgfx::isValid(environmentMap->getHandle())
                ? environmentMap->getHandle() : Cubemap::getNullCubemap()->getHandle());
        const bool brdfReady = bgfx::isValid(environmentBRDF);
        bgfx::setTexture(terrainLayout ? 12 : 6, uBRDFMap, brdfReady ? environmentBRDF : Texture::getNullTexture()->getHandle());
        const glm::vec4 brdfInfo(brdfReady ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f);
        bgfx::setUniform(uBRDFCache, glm::value_ptr(brdfInfo));
        const bool irradianceReady = bgfx::isValid(diffuseIrradiance);
        bgfx::setTexture(terrainLayout ? 13 : 10, uIrradianceMap, irradianceReady ? diffuseIrradiance : Cubemap::getNullCubemap()->getHandle());
        const bool specularReady = bgfx::isValid(specularEnvironment);
        bgfx::setTexture(terrainLayout ? 14 : 11, uPrefilteredMap, specularReady ? specularEnvironment : Cubemap::getNullCubemap()->getHandle());
        const glm::vec4 cacheInfo(irradianceReady ? 1.0f : 0.0f, 32.0f, specularReady ? 1.0f : 0.0f, 6.0f);
        bgfx::setUniform(terrainLayout ? uTerrainEnvironmentCache : uEnvironmentCache, glm::value_ptr(cacheInfo));
    }

	int Renderer::getOutlineViewId()
	{
		return RENDER_OUTLINE_PASS_ID;
	}

	int Renderer::getFinalViewId()
	{
		return RENDER_FINAL_PASS_ID;
	}

	int Renderer::getSceneViewId()
	{
		return RENDER_SCENE_PASS_ID;
	}

	int Renderer::getOverlayViewId()
	{
		return RENDER_OVERLAY_PASS_ID;
	}

	int Renderer::getNumViewsUsed()
	{
		return CAMERA_VIEW_GROUP_SIZE;
	}

	Light* Renderer::getFirstLight()
	{
		Light* light = nullptr;

		//Search directional light first
		for (auto& l : lights)
		{
			if (l->getLightType() != LightType::Directional)
				continue;

			if (l->gameObject->getActive() && l->enabled)
			{
				light = l;
				break;
			}
		}

		//Use other if not found
		if (light == nullptr)
		{
			for (auto& l : lights)
			{
				if (l->getLightType() == LightType::Directional)
					continue;

				if (l->gameObject->getActive() && l->enabled)
				{
					light = l;
					break;
				}
			}
		}

		return light;
	}

	int Renderer::getNumActiveLightsWithShadows()
	{
		int lightsWithShadowsCount = 0;

		for (auto& light : lights)
		{
			if (!light->getCastShadows())
				continue;
			if (light->getLightRenderMode() == LightRenderMode::Baked)
				continue;
			if (!light->getEnabled())
				continue;
			if (!light->getGameObject()->getActive())
				continue;

			++lightsWithShadowsCount;
		}

		return lightsWithShadowsCount;
	}

	void Renderer::setSkyModel(SkyModel value)
	{
		skyModel = value;
		if (giEnabled)
			updateEnvironmentMap();
	}

	void Renderer::setSkyMaterial(Material* value)
	{
		skyMaterial = value;
		if (giEnabled)
			updateEnvironmentMap();
	}

	void Renderer::calculateVisibility(const glm::mat4x4& view, const glm::mat4x4& proj, Frustum* frustum, float maxDistance)
	{
		moc->ClearBuffer();

		std::vector<GameObject*> staticNodes = Engine::getSingleton()->getGameObjects();

		glm::mat4x4 viewProj = proj * view;
		glm::vec3 cameraPos = glm::vec3(view[3][0], view[3][1], view[3][2]);

		for (auto it = staticNodes.begin(); it != staticNodes.end(); ++it)
		{
			GameObject* obj = *it;
			Transform* t = obj->getTransform();

			if (!obj->getActive())
				continue;

			if (!obj->getOcclusionStatic())
				continue;

			MeshRenderer* rend = (MeshRenderer*)obj->getComponent(MeshRenderer::COMPONENT_TYPE);
			if (rend != nullptr && rend->getEnabled())
			{
				if (rend->isSkinned())
					continue;

				if (rend->isTransparent())
					continue;

				if (frustum != nullptr)
				{
					if (!frustum->sphereInFrustum(t->getPosition(), rend->getBounds().getRadius() * 2.2f))
						continue;
				}

				if (glm::distance(t->getPosition(), cameraPos) > maxDistance)
					continue;

				Mesh* mesh = rend->getMesh();

				if (mesh != nullptr)
				{
					glm::mat4x4 mtx = viewProj * t->getTransformMatrix();

					auto& lodVbuf = rend->getLodVertexBuffer();
					auto& lodVbufMtx = rend->getLodVertexBufferMtxCache();

					for (int sm = 0; sm < mesh->getSubMeshCount(); ++sm)
					{
						SubMesh* subMesh = mesh->getSubMesh(sm);

						auto& vbuf = subMesh->getVertexBuffer();
						auto& ibuf = subMesh->getIndexBuffer();

						int maxLod = subMesh->getLodLevelsCount() - 1;

						if (maxLod >= 0)
						{
							auto& lodIbuf = subMesh->getLodIndexBuffer(maxLod);

							for (int i = 0; i < lodVbufMtx[sm].size(); ++i)
								lodVbufMtx[sm][i] = false;

							for (int i = 0; i < lodIbuf.size(); ++i)
							{
								if (!lodVbufMtx[sm][lodIbuf[i]])
								{
									glm::vec4 pos = mtx * glm::vec4(vbuf[lodIbuf[i]].position, 1.0);
									lodVbuf[sm][lodIbuf[i]] = pos;
									lodVbufMtx[sm][lodIbuf[i]] = true;
								}
							}

							moc->RenderTriangles(&lodVbuf[sm][0].x, lodIbuf.data(), lodIbuf.size() / 3);
						}

						if (maxLod < 0)
						{
							for (int i = 0; i < vbuf.size(); ++i)
							{
								glm::vec4 pos = mtx * glm::vec4(vbuf[i].position, 1.0);
								lodVbuf[sm][i] = pos;
							}

							moc->RenderTriangles(&lodVbuf[sm][0].x, ibuf.data(), ibuf.size() / 3);
						}
					}
				}
			}
		}

		//cmoc->Flush();
	}

    void Renderer::createEnvironmentBRDF()
    {
        if (bgfx::isValid(environmentBRDF)) return;
        constexpr uint16_t size = 64;
        constexpr int samples = 256;
        const uint64_t flags = BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP;
        if (!bgfx::isTextureValid(0, false, 1, bgfx::TextureFormat::RG32F, flags)) return;
        std::vector<float> values(size_t(size) * size * 2, 0.0f);
        for (int y = 0; y < size; ++y)
        {
            const double roughness = (y + 0.5) / size;
            const double a = std::max(roughness * roughness, 0.0016);
            const double a2 = a * a;
            for (int x = 0; x < size; ++x)
            {
                const double nv = (x + 0.5) / size;
                const double vx = std::sqrt(1.0 - nv * nv);
                const double gv = 2.0 * nv / (nv + std::sqrt(a2 + (1.0-a2)*nv*nv));
                double scale = 0.0, bias = 0.0;
                for (int i = 0; i < samples; ++i)
                {
                    const double u = (i + 0.5) / samples;
                    const double nh = std::sqrt((1.0-u) / (1.0-u+a2*u));
                    const double hx = std::sqrt(std::max(1.0-nh*nh, 0.0)) * std::cos(i * 2.39996323);
                    const double vh = std::min(1.0, std::max(vx*hx + nv*nh, 0.0));
                    const double nl = std::max(2.0*vh*nh - nv, 0.0);
                    if (nl <= 0.0 || vh <= 0.0) continue;
                    const double gl = 2.0 * nl / (nl + std::sqrt(a2 + (1.0-a2)*nl*nl));
                    const double weight = gv * gl * vh / std::max(nh * nv, 0.00001);
                    const double fc = std::pow(1.0-vh, 5.0);
                    scale += (1.0-fc)*weight;
                    bias += fc*weight;
                }
                const size_t index = (size_t(y)*size+x)*2;
                values[index] = float(scale / samples);
                values[index+1] = float(bias / samples);
            }
        }
        environmentBRDF = bgfx::createTexture2D(size, size, false, 1, bgfx::TextureFormat::RG32F,
            flags, bgfx::copy(values.data(), uint32_t(values.size()*sizeof(float))));
    }

    void Renderer::updateEnvironmentLightingCache()
    {
#ifdef FALCO_MODERN_BGFX
        const bgfx::Caps* caps = bgfx::getCaps();
        const uint64_t flags = BGFX_TEXTURE_RT | BGFX_SAMPLER_UVW_CLAMP;
        if (caps == nullptr || !bgfx::isValid(irradiancePH) || environmentMap == nullptr
            || !bgfx::isValid(environmentMap->getHandle())
            || !bgfx::isTextureValid(0, true, 1, bgfx::TextureFormat::RGBA16F, flags))
            return;
        float projection[16];
        bx::mtxOrtho(projection, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 100.0f,
            0.0f, caps->homogeneousDepth, bx::Handedness::Right);
        const glm::mat4 identity(1.0f);
        const glm::vec4 cacheInfo(0.0f, 128.0f, 0.0f, 0.0f);
        auto bake = [&](uint16_t size, uint8_t mipCount, bool specular, bgfx::TextureHandle& destination)
        {
            if (int(mipCount) * 6 > ENVIRONMENT_FILTER_VIEW_COUNT) return;
            bool submitted = false;
            bgfx::TextureHandle cube = bgfx::createTextureCube(size, mipCount > 1, 1, bgfx::TextureFormat::RGBA16F, flags);
            if (!bgfx::isValid(cube)) return;
            std::vector<bgfx::FrameBufferHandle> faces(size_t(mipCount) * 6);
            for (auto& face : faces) face = BGFX_INVALID_HANDLE;
            bool ready = true;
            for (uint8_t mip = 0; mip < mipCount; ++mip)
            {
                for (uint16_t face = 0; face < 6; ++face)
                {
                    bgfx::Attachment attachment;
                    // Every mip is rendered explicitly; do not request automatic mip generation.
                    attachment.init(cube, bgfx::Access::Write, face, 1, mip, BGFX_RESOLVE_NONE);
                    auto& framebuffer = faces[size_t(mip) * 6 + face];
                    framebuffer = bgfx::createFrameBuffer(1, &attachment, false);
                    ready = ready && bgfx::isValid(framebuffer);
                }
            }
            for (uint8_t mip = 0; ready && mip < mipCount; ++mip)
            {
                const uint16_t mipSize = uint16_t(std::max(1, int(size) >> mip));
                for (uint16_t face = 0; ready && face < 6; ++face)
                {
                    const bgfx::ViewId viewId = bgfx::ViewId(RENDER_FIRST_SERVICE_PASS_ID + int(mip) * 6 + face);
                    bgfx::setViewClear(viewId, BGFX_CLEAR_COLOR, 0, 1.0f, 0);
                    bgfx::setViewRect(viewId, 0, 0, mipSize, mipSize);
                    bgfx::setViewFrameBuffer(viewId, faces[size_t(mip) * 6 + face]);
                    bgfx::setViewTransform(viewId, nullptr, projection);
                    bgfx::setTransform(glm::value_ptr(identity));
                    bgfx::setTexture(6, uBRDFMap, Texture::getNullTexture()->getHandle());
                    const glm::vec4 noBRDFCache(0.0f);
                    bgfx::setUniform(uBRDFCache, glm::value_ptr(noBRDFCache));
                    bgfx::setTexture(9, uEnvMap, environmentMap->getHandle());
                    // Both disabled cache samplers bind a valid source, never the destination.
                    bgfx::setTexture(10, uIrradianceMap, environmentMap->getHandle());
                    bgfx::setTexture(11, uPrefilteredMap, environmentMap->getHandle());
                    bgfx::setUniform(uEnvironmentCache, glm::value_ptr(cacheInfo));
                    const float roughness = mipCount > 1 ? float(mip) / float(mipCount - 1) : 0.0f;
                    const glm::vec4 faceInfo(float(face), caps->originBottomLeft ? 0.0f : 1.0f, roughness, specular ? 1.0f : 0.0f);
                    bgfx::setUniform(uCubeFace, glm::value_ptr(faceInfo));
                    bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A);
                    if (!Primitives::screenSpaceQuad())
                    {
                        bgfx::discard();
                        ready = false;
                        break;
                    }
                    bgfx::submit(viewId, irradiancePH);
                    submitted = true;
                }
            }
            // Distinct views retain every face/mip target until the entire batch
            // is submitted; queue the batch before deferred framebuffer destruction.
            if (submitted) frame();
            for (auto face : faces)
                if (bgfx::isValid(face)) bgfx::destroy(face);
            if (ready)
            {
                if (bgfx::isValid(destination)) bgfx::destroy(destination);
                destination = cube;
            }
            else
            {
                bgfx::destroy(cube);
                Debug::logError("Environment convolution cache unavailable; using integration fallback");
            }
        };
        createEnvironmentBRDF();
        bake(32, 1, false, diffuseIrradiance);
        bake(64, 7, true, specularEnvironment);
#endif
    }

	void Renderer::updateEnvironmentMap()
	{
		environmentMapUpdatePending = true;
	}

	void Renderer::rebuildEnvironmentMap()
	{
		deleteEnvironmentMap();

	#ifdef FALCO_MODERN_BGFX
		// Render directly into cubemap faces.  The former path allocated twelve
		// temporary 2D textures, synchronously read six of them back to the CPU and
		// uploaded the same pixels again.  On D3D11 an invalid/late framebuffer in
		// that sequence fell back to the application backbuffer (the visible small
		// Skybox window) and then crashed inside the driver during readback.
		constexpr uint16_t envSize = 256;
		const uint64_t cubeFlags = BGFX_TEXTURE_RT | BGFX_SAMPLER_UVW_CLAMP;
		if (!bgfx::isTextureValid(0, true, 1, bgfx::TextureFormat::RGBA16F, cubeFlags))
		{
			Debug::log("HDR RGBA16F environment cubemap render target is not supported", Debug::DbgColorRed);
			return;
		}

		bgfx::TextureHandle cubeHandle = bgfx::createTextureCube(
			envSize, false, 1, bgfx::TextureFormat::RGBA16F, cubeFlags);
		if (!bgfx::isValid(cubeHandle))
		{
			Debug::log("Failed to create Realtime GI cubemap", Debug::DbgColorRed);
			return;
		}

		bgfx::FrameBufferHandle faceBuffers[6];
		for (bgfx::FrameBufferHandle& handle : faceBuffers)
			handle = BGFX_INVALID_HANDLE;

		bool frameBuffersValid = true;
		for (uint16_t face = 0; face < 6; ++face)
		{
			bgfx::Attachment attachment;
			// D3D11 does not reliably support auto-generated mip levels for a cubemap
			// that is also used as six independent render targets. Realtime GI uses
			// the captured base level explicitly, so no resolve/mip generation is needed.
			attachment.init(cubeHandle, bgfx::Access::Write, face, 1, 0, BGFX_RESOLVE_NONE);
			faceBuffers[face] = bgfx::createFrameBuffer(1, &attachment, false);
			frameBuffersValid = frameBuffersValid && bgfx::isValid(faceBuffers[face]);
		}

		if (!frameBuffersValid)
		{
			for (bgfx::FrameBufferHandle handle : faceBuffers)
				if (bgfx::isValid(handle))
					bgfx::destroy(handle);
			bgfx::destroy(cubeHandle);
			Debug::log("Failed to create Realtime GI cubemap face buffers", Debug::DbgColorRed);
			return;
		}

		const bgfx::ViewId viewId = RENDER_ENVIRONMENT_PASS_ID;
        // Capture world-space radiance in the same face convention used by
        // Irradiance.h. Legacy opposite-face rotations plus an X mirror made
        // adjacent faces disagree. Sky materials keep their own sampling convention.
        const bgfx::Caps* captureCaps = bgfx::getCaps();
        float captureProjection[16];
        bx::mtxProj(captureProjection, 90.0f, 1.0f, 0.1f, 1000.0f,
            captureCaps->homogeneousDepth, bx::Handedness::Right);
        const glm::mat4 projection = glm::scale(glm::make_mat4(captureProjection),
            glm::vec3(1.0f, captureCaps->originBottomLeft ? 1.0f : -1.0f, 1.0f));
        const glm::vec3 origin(0.0f);
        const glm::mat4 faceViews[6] =
        {
            glm::lookAtRH(origin, glm::vec3( 1, 0, 0), glm::vec3(0,-1, 0)),
            glm::lookAtRH(origin, glm::vec3(-1, 0, 0), glm::vec3(0,-1, 0)),
            glm::lookAtRH(origin, glm::vec3( 0, 1, 0), glm::vec3(0, 0, 1)),
            glm::lookAtRH(origin, glm::vec3( 0,-1, 0), glm::vec3(0, 0,-1)),
            glm::lookAtRH(origin, glm::vec3( 0, 0, 1), glm::vec3(0,-1, 0)),
            glm::lookAtRH(origin, glm::vec3( 0, 0,-1), glm::vec3(0,-1, 0))
        };
		const glm::mat4 skyMatrix = glm::scale(
			glm::identity<glm::mat4>(), glm::vec3(100.0f));

		for (uint16_t face = 0; face < 6; ++face)
		{
			bgfx::setViewClear(viewId, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH,
				Color::packRGBA(ambientColor.toLinear()), 1.0f, 0);
			bgfx::setViewRect(viewId, 0, 0, envSize, envSize);
			bgfx::setViewFrameBuffer(viewId, faceBuffers[face]);
			bgfx::setViewTransform(viewId, glm::value_ptr(faceViews[face]), glm::value_ptr(projection));
			if (skyMaterial != nullptr)
				renderSkybox(viewId, nullptr, skyMatrix);
			else
				bgfx::touch(viewId);
			frame();
		}

		for (bgfx::FrameBufferHandle handle : faceBuffers)
			bgfx::destroy(handle);

		environmentMap = Cubemap::create("system/GI/", "environmentMap");
		environmentMap->adoptTextureHandle(cubeHandle);
        updateEnvironmentLightingCache();
		return;
	#endif

		const bgfx::Caps* caps = bgfx::getCaps();
		if ((BGFX_CAPS_TEXTURE_BLIT | BGFX_CAPS_TEXTURE_READ_BACK) == (caps->supported & (BGFX_CAPS_TEXTURE_BLIT | BGFX_CAPS_TEXTURE_READ_BACK)))
		{
			int envSize = 512;

			uint32_t tflags = BGFX_SAMPLER_MIN_ANISOTROPIC
				| BGFX_SAMPLER_MAG_ANISOTROPIC
				| BGFX_SAMPLER_U_CLAMP
				| BGFX_SAMPLER_V_CLAMP;

			// Capture/readback surfaces have a single mip.  The convenience
			// framebuffer overload requests the default AUTO_GEN_MIPS resolve, which
			// is invalid for this synchronous D3D11 readback path and can crash the
			// driver when Realtime GI is enabled.  Match all other modern render
			// targets: one mip and an explicit no-resolve attachment.
			bgfx::TextureHandle tex0 = bgfx::createTexture2D(envSize, envSize, false, 1, bgfx::TextureFormat::BGRA8, BGFX_TEXTURE_RT | tflags);
			bgfx::TextureHandle tex1 = bgfx::createTexture2D(envSize, envSize, false, 1, bgfx::TextureFormat::BGRA8, BGFX_TEXTURE_RT | tflags);
			bgfx::TextureHandle tex2 = bgfx::createTexture2D(envSize, envSize, false, 1, bgfx::TextureFormat::BGRA8, BGFX_TEXTURE_RT | tflags);
			bgfx::TextureHandle tex3 = bgfx::createTexture2D(envSize, envSize, false, 1, bgfx::TextureFormat::BGRA8, BGFX_TEXTURE_RT | tflags);
			bgfx::TextureHandle tex4 = bgfx::createTexture2D(envSize, envSize, false, 1, bgfx::TextureFormat::BGRA8, BGFX_TEXTURE_RT | tflags);
			bgfx::TextureHandle tex5 = bgfx::createTexture2D(envSize, envSize, false, 1, bgfx::TextureFormat::BGRA8, BGFX_TEXTURE_RT | tflags);

			bgfx::TextureHandle m_rb0 = bgfx::createTexture2D(envSize, envSize, false, 1, bgfx::TextureFormat::BGRA8, BGFX_TEXTURE_BLIT_DST | BGFX_TEXTURE_READ_BACK | tflags);
			bgfx::TextureHandle m_rb1 = bgfx::createTexture2D(envSize, envSize, false, 1, bgfx::TextureFormat::BGRA8, BGFX_TEXTURE_BLIT_DST | BGFX_TEXTURE_READ_BACK | tflags);
			bgfx::TextureHandle m_rb2 = bgfx::createTexture2D(envSize, envSize, false, 1, bgfx::TextureFormat::BGRA8, BGFX_TEXTURE_BLIT_DST | BGFX_TEXTURE_READ_BACK | tflags);
			bgfx::TextureHandle m_rb3 = bgfx::createTexture2D(envSize, envSize, false, 1, bgfx::TextureFormat::BGRA8, BGFX_TEXTURE_BLIT_DST | BGFX_TEXTURE_READ_BACK | tflags);
			bgfx::TextureHandle m_rb4 = bgfx::createTexture2D(envSize, envSize, false, 1, bgfx::TextureFormat::BGRA8, BGFX_TEXTURE_BLIT_DST | BGFX_TEXTURE_READ_BACK | tflags);
			bgfx::TextureHandle m_rb5 = bgfx::createTexture2D(envSize, envSize, false, 1, bgfx::TextureFormat::BGRA8, BGFX_TEXTURE_BLIT_DST | BGFX_TEXTURE_READ_BACK | tflags);

			bgfx::Attachment envAt0; envAt0.init(tex0, bgfx::Access::Write, 0, 1, 0, BGFX_RESOLVE_NONE);
			bgfx::Attachment envAt1; envAt1.init(tex1, bgfx::Access::Write, 0, 1, 0, BGFX_RESOLVE_NONE);
			bgfx::Attachment envAt2; envAt2.init(tex2, bgfx::Access::Write, 0, 1, 0, BGFX_RESOLVE_NONE);
			bgfx::Attachment envAt3; envAt3.init(tex3, bgfx::Access::Write, 0, 1, 0, BGFX_RESOLVE_NONE);
			bgfx::Attachment envAt4; envAt4.init(tex4, bgfx::Access::Write, 0, 1, 0, BGFX_RESOLVE_NONE);
			bgfx::Attachment envAt5; envAt5.init(tex5, bgfx::Access::Write, 0, 1, 0, BGFX_RESOLVE_NONE);
			bgfx::FrameBufferHandle envFb0 = bgfx::createFrameBuffer(1, &envAt0, true);
			bgfx::FrameBufferHandle envFb1 = bgfx::createFrameBuffer(1, &envAt1, true);
			bgfx::FrameBufferHandle envFb2 = bgfx::createFrameBuffer(1, &envAt2, true);
			bgfx::FrameBufferHandle envFb3 = bgfx::createFrameBuffer(1, &envAt3, true);
			bgfx::FrameBufferHandle envFb4 = bgfx::createFrameBuffer(1, &envAt4, true);
			bgfx::FrameBufferHandle envFb5 = bgfx::createFrameBuffer(1, &envAt5, true);

			// Environment capture is a synchronous auxiliary render.  Keep it out of
			// all camera/shadow groups so its persistent view state cannot collide with
			// Scene or Camera Preview passes.
			const int v1 = RENDER_ENVIRONMENT_PASS_ID;

			bgfx::setViewClear(v1, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, Color::packRGBA(ambientColor), 1.0f, 0);
			bgfx::setViewRect(v1, 0, 0, envSize, envSize);

			float aspect = width / height;
			glm::mat4 proj = glm::perspectiveRH(90.0f * Mathf::fDeg2Rad, aspect, 0.1f, 1000.0f);
			proj = glm::scale(proj, glm::vec3(-1, 1, 1));

			glm::mat4x4 view = glm::identity<glm::mat4x4>();

			if (skyMaterial != nullptr)
			{
				glm::mat4x4 skyMtx = glm::identity<glm::mat4x4>();
				skyMtx = glm::scale(skyMtx, glm::vec3(100.0f, 100.0f, 100.0f));

				//+X
				bgfx::setViewFrameBuffer(v1, envFb0);
				view = glm::identity<glm::mat4x4>();
				view *= glm::mat4_cast(glm::angleAxis(-90.0f * Mathf::fDeg2Rad, glm::vec3(0, 1, 0)));
				bgfx::setViewTransform(v1, glm::value_ptr(view), glm::value_ptr(proj));
				renderSkybox(v1, nullptr, skyMtx);
				frame();

				//-X
				bgfx::setViewFrameBuffer(v1, envFb1);
				view = glm::identity<glm::mat4x4>();
				view *= glm::mat4_cast(glm::angleAxis(90.0f * Mathf::fDeg2Rad, glm::vec3(0, 1, 0)));
				bgfx::setViewTransform(v1, glm::value_ptr(view), glm::value_ptr(proj));
				renderSkybox(v1, nullptr, skyMtx);
				frame();

				//+Y
				bgfx::setViewFrameBuffer(v1, envFb2);
				view = glm::identity<glm::mat4x4>();
				view *= glm::mat4_cast(glm::angleAxis(90.0f * Mathf::fDeg2Rad, glm::vec3(1, 0, 0)));
				bgfx::setViewTransform(v1, glm::value_ptr(view), glm::value_ptr(proj));
				renderSkybox(v1, nullptr, skyMtx);
				frame();

				//-Y
				bgfx::setViewFrameBuffer(v1, envFb3);
				view = glm::identity<glm::mat4x4>();
				view *= glm::mat4_cast(glm::angleAxis(-90.0f * Mathf::fDeg2Rad, glm::vec3(1, 0, 0)));
				bgfx::setViewTransform(v1, glm::value_ptr(view), glm::value_ptr(proj));
				renderSkybox(v1, nullptr, skyMtx);
				frame();

				//+Z
				bgfx::setViewFrameBuffer(v1, envFb4);
				view = glm::identity<glm::mat4x4>();
				bgfx::setViewTransform(v1, glm::value_ptr(view), glm::value_ptr(proj));
				renderSkybox(v1, nullptr, skyMtx);
				frame();

				//-Z
				bgfx::setViewFrameBuffer(v1, envFb5);
				view = glm::identity<glm::mat4x4>();
				view *= glm::mat4_cast(glm::angleAxis(180.0f * Mathf::fDeg2Rad, glm::vec3(0, 1, 0)));
				bgfx::setViewTransform(v1, glm::value_ptr(view), glm::value_ptr(proj));
				renderSkybox(v1, nullptr, skyMtx);
				frame();
			}
			else
			{
				bgfx::setViewFrameBuffer(v1, envFb0); bgfx::touch(v1); frame();
				bgfx::setViewFrameBuffer(v1, envFb1); bgfx::touch(v1); frame();
				bgfx::setViewFrameBuffer(v1, envFb2); bgfx::touch(v1); frame();
				bgfx::setViewFrameBuffer(v1, envFb3); bgfx::touch(v1); frame();
				bgfx::setViewFrameBuffer(v1, envFb4); bgfx::touch(v1); frame();
				bgfx::setViewFrameBuffer(v1, envFb5); bgfx::touch(v1); frame();
			}

			int ds = envSize * envSize * 4;

			environmentMap = Cubemap::create("system/GI/", "environmentMap");

			const int v2 = RENDER_ENVIRONMENT_BLIT_PASS_ID;

			uint8_t* data0 = new uint8_t[ds];
			uint8_t* data1 = new uint8_t[ds];
			uint8_t* data2 = new uint8_t[ds];
			uint8_t* data3 = new uint8_t[ds];
			uint8_t* data4 = new uint8_t[ds];
			uint8_t* data5 = new uint8_t[ds];

			bgfx::blit(v2, m_rb0, 0, 0, tex0);
			bgfx::blit(v2, m_rb1, 0, 0, tex1);
			bgfx::blit(v2, m_rb2, 0, 0, tex2);
			bgfx::blit(v2, m_rb3, 0, 0, tex3);
			bgfx::blit(v2, m_rb4, 0, 0, tex4);
			bgfx::blit(v2, m_rb5, 0, 0, tex5);

			uint32_t readyFrame = bgfx::readTexture(m_rb0, data0);
			readyFrame = std::max(readyFrame, bgfx::readTexture(m_rb1, data1));
			readyFrame = std::max(readyFrame, bgfx::readTexture(m_rb2, data2));
			readyFrame = std::max(readyFrame, bgfx::readTexture(m_rb3, data3));
			readyFrame = std::max(readyFrame, bgfx::readTexture(m_rb4, data4));
			readyFrame = std::max(readyFrame, bgfx::readTexture(m_rb5, data5));

			// readTexture explicitly reports the frame in which CPU data becomes
			// available.  A fixed two-frame wait is insufficient on a loaded GPU
			// and allowed the D3D11 readback textures to be freed while in flight.
			while (submittedFrameNumber < readyFrame)
				frame();

			bgfx::destroy(m_rb0);
			bgfx::destroy(m_rb1);
			bgfx::destroy(m_rb2);
			bgfx::destroy(m_rb3);
			bgfx::destroy(m_rb4);
			bgfx::destroy(m_rb5);

			bgfx::destroy(envFb0);
			bgfx::destroy(envFb1);
			bgfx::destroy(envFb2);
			bgfx::destroy(envFb3);
			bgfx::destroy(envFb4);
			bgfx::destroy(envFb5);

			Texture* cubeTex0 = Texture::create("system/GI/", "env_texture0", envSize, envSize, 1, GX::Texture::TextureType::Texture2D, bgfx::TextureFormat::BGRA8, data0, ds, true, true);
			Texture* cubeTex1 = Texture::create("system/GI/", "env_texture1", envSize, envSize, 1, GX::Texture::TextureType::Texture2D, bgfx::TextureFormat::BGRA8, data1, ds, true, true);
			Texture* cubeTex2 = Texture::create("system/GI/", "env_texture2", envSize, envSize, 1, GX::Texture::TextureType::Texture2D, bgfx::TextureFormat::BGRA8, data2, ds, true, true);
			Texture* cubeTex3 = Texture::create("system/GI/", "env_texture3", envSize, envSize, 1, GX::Texture::TextureType::Texture2D, bgfx::TextureFormat::BGRA8, data3, ds, true, true);
			Texture* cubeTex4 = Texture::create("system/GI/", "env_texture4", envSize, envSize, 1, GX::Texture::TextureType::Texture2D, bgfx::TextureFormat::BGRA8, data4, ds, true, true);
			Texture* cubeTex5 = Texture::create("system/GI/", "env_texture5", envSize, envSize, 1, GX::Texture::TextureType::Texture2D, bgfx::TextureFormat::BGRA8, data5, ds, true, true);

			environmentMap->setTexture(0, cubeTex0, false);
			environmentMap->setTexture(1, cubeTex1, false);
			environmentMap->setTexture(2, cubeTex2, false);
			environmentMap->setTexture(3, cubeTex3, false);
			environmentMap->setTexture(4, cubeTex4, false);
			environmentMap->setTexture(5, cubeTex5, false);

			environmentMap->updateCubemap();

			cubeTex0->freeData();
			cubeTex1->freeData();
			cubeTex2->freeData();
			cubeTex3->freeData();
			cubeTex4->freeData();
			cubeTex5->freeData();

			delete cubeTex0;
			delete cubeTex1;
			delete cubeTex2;
			delete cubeTex3;
			delete cubeTex4;
			delete cubeTex5;

			delete[] data0;
			delete[] data1;
			delete[] data2;
			delete[] data3;
			delete[] data4;
			delete[] data5;

			environmentMap->setTexture(0, nullptr, false);
			environmentMap->setTexture(1, nullptr, false);
			environmentMap->setTexture(2, nullptr, false);
			environmentMap->setTexture(3, nullptr, false);
			environmentMap->setTexture(4, nullptr, false);
			environmentMap->setTexture(5, nullptr, false);

			frame();
		}
	}
}
