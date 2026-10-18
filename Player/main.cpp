#include <bx/uint32_t.h>
#include <dear-imgui/imgui.h>

#include "../Engine/Renderer/Window.h"
#include "../Engine/Renderer/Renderer.h"
#include "../Engine/Core/Engine.h"
#include "../Engine/Core/APIManager.h"
#include "../Engine/Core/GameObject.h"
#include "../Engine/Components/Camera.h"
#include "../Engine/Assets/Scene.h"
#include "../Engine/Assets/Texture.h"
#include "../Engine/Classes/Helpers.h"
#include "../Engine/Classes/IO.h"
#include "../Engine/Classes/StringConverter.h"
#include "../Engine/Core/Debug.h"
#include "../Engine/Core/Time.h"
#include "LogoText.c"

#if defined(_WIN32) && defined(FALCO_WITH_STEAM)
#include "../steam/steam_api.h"

//Steam API callbacks
#include "SteamManager.h"
#endif

#include <SDL2/SDL.h>


std::string projectPath = "";
std::string assemblyPath = "";
std::string scenePath = "";
bool useLibrary = false;
int screenW = 1280;
int screenH = 800;
bool fullscreen = false;
bool exclusiveMode = false;
float logoTime = 40.0f;
float logoTimer = 0.0f;
float logoTint = 0.0f;

int main(int argc, char* argv[])
{
	projectPath = GX::Helper::ExePath();

#if defined(_WIN32) && defined(FALCO_MODERN_BGFX)
	// System shaders require the external compiler even when a cache exists.
	// Reject an incomplete runtime before bgfx consumes a null shader result.
	const std::string compilerPath = projectPath + "shadercRelease.exe";
	if (!GX::IO::FileExists(compilerPath))
	{
		const std::string message = "Player cannot start: shadercRelease.exe is missing.\n"
			"Restore this file next to Player.exe and try again.\n\nExpected path:\n" + compilerPath;
		GX::Debug::createLogFile(projectPath + "player.log");
		GX::Debug::logError(message);
		GX::Debug::log("Startup cancelled before renderer initialization; exit code 1.");
		std::cerr << message << std::endl;
		MessageBoxA(nullptr, message.c_str(), "Player - missing shader compiler", MB_OK | MB_ICONERROR);
		return 1;
	}
#endif
	
	if (argc > 1) projectPath = CP_SYS(argv[1]);
	if (argc > 2) assemblyPath = CP_SYS(argv[2]);
	if (argc > 3) scenePath = CP_SYS(argv[3]);
	if (argc > 4) useLibrary = CP_SYS(argv[4]) == "useLibrary";
	if (argc > 5) screenW = std::atoi(argv[5]);
	if (argc > 6) screenH = std::atoi(argv[6]);
	if (argc > 7) fullscreen = CP_SYS(argv[7]) == "true";

#ifdef _WIN32
	if (argc > 8 && argv[8] == CP_SYS("/d"))
	{
		AllocConsole();
		freopen("CON", "w", stdout);
		freopen("CON", "w", stderr);
		freopen("CON", "r", stdin);

		::ShowWindow(::GetConsoleWindow(), SW_SHOW);
	}
#endif

	std::string libDir = "Assets/";
	if (useLibrary) libDir = "Library/";

	std::string exeName = GX::IO::GetFileName(GX::Helper::ExeName());

	GX::Engine::getSingleton()->setAppName(exeName);
	GX::Engine::getSingleton()->setRootPath(projectPath);
	GX::Engine::getSingleton()->setAssetsPath(projectPath + "Assets/");
	GX::Engine::getSingleton()->setLibraryPath(projectPath + libDir);
	GX::Engine::getSingleton()->setSettingsPath(projectPath + "Settings/");
	GX::Engine::getSingleton()->setAssemblyPath(projectPath + assemblyPath);
	GX::Engine::getSingleton()->setBuiltinResourcesPath(GX::Helper::ExePath() + "BuiltinResources/");

	GX::ProjectSettings* settings = GX::Engine::getSingleton()->getSettings();
	settings->load();

#if defined(_WIN32) && defined(FALCO_WITH_STEAM)
	//Steam API init
	if (settings->getEnableSteamAPI())
	{
		if (argc > 1)
		{
			int appId = settings->getSteamAppId();
			GX::IO::WriteText(GX::Helper::ExePath() + "steam_appid.txt", std::to_string(appId));
		}

		if (SteamAPI_RestartAppIfNecessary(k_uAppIdInvalid))
		{
			return 1;
		}

		if (!SteamAPI_Init())
		{
			GX::Debug::logError("Steam must be running to play this game or app id is invalid");
			std::cout << "Fatal Error - Steam must be running to play this game or app id is invalid" << std::endl;
			MessageBoxA(0, "Steam must be running to play this game", "Error", MB_OK | MB_ICONERROR);
			return 1;
		}
	}
	//
#endif

	if (argc < 6)
	{
		if (settings->getUseNativeResolution())
		{
#ifdef _WIN32
			screenW = GetSystemMetrics(SM_CXSCREEN);
			screenH = GetSystemMetrics(SM_CYSCREEN);
#else
			SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER | SDL_INIT_GAMECONTROLLER);

			SDL_DisplayMode DM;
			SDL_GetCurrentDisplayMode(0, &DM);
			screenW = DM.w;
			screenH = DM.h;
#endif
		}
		else
		{
			screenW = settings->getScreenWidth();
			screenH = settings->getScreenHeight();
		}

		fullscreen = settings->getFullScreen();
		exclusiveMode = settings->getExclusiveMode();
	}

	Uint32 flags = SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE;
	if (fullscreen)
	{
		if (exclusiveMode)
			flags |= SDL_WINDOW_FULLSCREEN;
		else
			flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
	}

#if defined(_WIN32) && defined(FALCO_WITH_STEAM)
	GX::SteamManager* steamMgr = nullptr;
#endif

	GX::Window* window = new GX::Window();
	if (!window->init(exeName, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, screenW, screenH, flags))
		return 1;

#if defined(_WIN32) && defined(FALCO_WITH_STEAM)
	if (settings->getEnableSteamAPI())
	{
		steamMgr = new GX::SteamManager();

		window->setUpdateCallback([=]
			{
				SteamAPI_RunCallbacks();
			}
		);
	}
#endif

	int realWidth = window->getWidth();
	int realHeight = window->getHeight();

	GX::Texture* logoTexture = GX::Texture::loadFromByteArray("system/", "StartupLogo", logoRawData, sizeof(logoRawData) / sizeof(char));

	if (argc > 1)
		logoTimer = logoTime;

	#ifndef FALCO_MODERN_BGFX
	std::string cbId = GX::Renderer::getSingleton()->addPostRenderCallback([=]() { GX::Renderer::getSingleton()->frame(); });
	#endif

	GX::Renderer::getSingleton()->setUIClearColor(GX::Color::Black);
	GX::Renderer::getSingleton()->setUICallback([=]() 
		{
			bool open = true;
			ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
			ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 1));
			ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
			ImGui::SetNextWindowSize(ImVec2(realWidth, realHeight), ImGuiCond_Always);
			ImGui::Begin("__Logo__", &open, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar);
			ImGui::PopStyleColor();
			ImGui::PopStyleVar();
			ImGui::SetCursorPos(ImVec2(realWidth / 2 - 256, realHeight / 2 - 256));
			ImGui::Image((void*)logoTexture->getHandle().idx, ImVec2(512, 512), ImVec2(0, 1), ImVec2(1, 0), ImVec4(1, 1, 1, logoTint));
			ImGui::End();

			if (logoTimer < logoTime)
			{
				logoTimer += GX::Time::getDeltaTime() * 10.0f;
				if (logoTimer <= 3)
					logoTint = 1.0f / 3.0f * logoTimer;
				if (logoTimer >= logoTime - 3.0f)
					logoTint = 1.0f / 3.0f * ((logoTime - 1.0f) - logoTimer);
			}
			else
			{
				GX::Renderer::getSingleton()->setUICallback(nullptr);
			#ifndef FALCO_MODERN_BGFX
				GX::Renderer::getSingleton()->removePostRenderCallback(cbId);
			#endif

				if (!scenePath.empty())
					GX::Scene::load(GX::Engine::getSingleton()->getAssetsPath(), scenePath);
				else
				{
					std::string startupScene = settings->getStartupScene();
					if (!startupScene.empty())
						GX::Scene::load(GX::Engine::getSingleton()->getAssetsPath(), startupScene);
					else
						GX::Debug::logWarning("Startup scene is not set");
				}
			}
		}
	);
	
	window->run();

#if defined(_WIN32) && defined(FALCO_WITH_STEAM)
	//Steam API shutdown
	if (settings->getEnableSteamAPI())
	{
		if (steamMgr != nullptr)
			delete steamMgr;

		SteamAPI_Shutdown();
	}
#endif
	return 0;
}
