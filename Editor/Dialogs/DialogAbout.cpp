#include "DialogAbout.h"

#include <imgui.h>
#include <imgui_internal.h>
#include <SDL2/SDL_scancode.h>

#include "../Windows/MainWindow.h"
#include "../Engine/Classes/Helpers.h"
#include "../Engine/Classes/StringConverter.h"
#include "../Engine/Assets/Texture.h"

#ifdef _WIN32
#include <shellapi.h>
#endif

namespace GX
{
	ImVec4 linkColor = ImVec4(0.4f, 0.4f, 1.0f, 1.0f);
	std::string version = "";

	DialogAbout::DialogAbout()
	{
		
	}

	DialogAbout::~DialogAbout()
	{
	}

	void DialogAbout::show()
	{
		showDreamOwlEdition = false;
		logoTexture = Texture::load(Helper::ExePath(), "Editor/Logo.png", false, Texture::CompressionMethod::None, true);
		visible = true;

		version = Helper::getVersion();
	}

	void DialogAbout::update()
	{
		if (!visible)
			return;

		ImGui::SetNextWindowSize(ImVec2(320.0f, showDreamOwlEdition ? 270.0f : 245.0f));
		ImGui::SetNextWindowPos(ImVec2(MainWindow::getSingleton()->getWidth() / 2.0f, MainWindow::getSingleton()->getHeight() / 2.0f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
		ImGui::Begin("About Falco Engine", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoDocking);

		if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)
			&& ImGui::IsKeyPressed(SDL_SCANCODE_D, false))
			showDreamOwlEdition = true;

		ImGui::Image((void*)logoTexture->getHandle().idx, ImVec2(64, 64), ImVec2(0, 1), ImVec2(1, 0));
		ImGui::Text("Falco Engine, version"); ImGui::SameLine(); ImGui::TextColored(ImVec4(0.9f, 0.9f, 0.0f, 1.0f), "%s", version.c_str());
		ImGui::Dummy(ImVec2(100, 10));
		/*ImGui::Text("Authors:");
		ImGui::Text("Alexander Gerashchenko (tracer0707)");
		ImGui::Text("Valeriy Sokolov (wsokolov)");
		ImGui::Dummy(ImVec2(100, 10));*/
		ImGui::TextUnformatted("Copyright: Falco Software Company");
		ImGui::TextColored(linkColor, "https://www.falco3d.com");
		if (ImGui::IsItemHovered())
			linkColor = ImVec4(0.7f, 0.7f, 1.0f, 1.0f);
		else
			linkColor = ImVec4(0.4f, 0.4f, 1.0f, 1.0f);

		if (ImGui::IsItemClicked())
		{
	#ifdef _WIN32
			ShellExecute(0, 0, u8"https://www.falco3d.com", 0, 0, SW_SHOW);
	#endif
		}

		ImGui::Dummy(ImVec2(100, 10));

		ImGui::TextUnformatted("(C) 2018 - 2026");
		if (showDreamOwlEdition)
			ImGui::TextUnformatted("DreamOwl Edition DremasovDS");

		ImGui::Dummy(ImVec2(0.0f, 10.0f));

		if (ImGui::Button("Close"))
		{
			visible = false;
		}

		ImGui::End();
	}
}
