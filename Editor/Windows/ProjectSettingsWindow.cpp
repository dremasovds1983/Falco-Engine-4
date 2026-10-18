#include "ProjectSettingsWindow.h"
#include "../Classes/AssetWindowGlyphs.h"
#include "MainWindow.h"
#include "../Serialization/EditorSettings.h"

#include <imgui.h>
#include <dear-imgui/misc/cpp/imgui_stdlib.h>
#include <algorithm>
#include "../Engine/Core/Engine.h"
#include "../Engine/Core/GameObject.h"
#include "../Engine/Serialization/Settings/ProjectSettings.h"
#include "../Engine/Classes/StringConverter.h"
#include "InspectorWindow.h"

#include "../PropertyEditors/ProjectSettingsEditor.h"

namespace GX
{
	ProjectSettingsWindow::ProjectSettingsWindow()
	{
		editor = new ProjectSettingsEditor();
	}

	ProjectSettingsWindow::~ProjectSettingsWindow()
	{
		delete editor;
	}

	void ProjectSettingsWindow::init()
	{
		editor->init();
	}

	void ProjectSettingsWindow::update()
	{
		updateTags();
		if (!opened)
			return;

		if (ImGui::Begin("Project Settings", &opened, ImGuiWindowFlags_NoCollapse))
		{
			if (ImGui::CollapsingHeader("Editor startup", ImGuiTreeNodeFlags_DefaultOpen))
			{
				auto* settings = MainWindow::getSettings();
				if (ImGui::Checkbox("Open project with an empty scene", &settings->openWithEmptyScene))
					settings->save();
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip("Applies next time this project opens. Disable to restore the last scene.");
			}
			ImGui::Separator();
			if (ImGui::GetWindowSize().x >= 200.0f)
				editor->update();
		}

		ImGui::End();
	}

    void ProjectSettingsWindow::updateTags()
    {
        if (!tagsOpened) return;
        restoreAssetWindowLayout("\xee\x80\x81 Tags###Tags", "Tags");
        if (ImGui::Begin("\xee\x80\x81 Tags###Tags", &tagsOpened))
        {
            auto* settings = Engine::getSingleton()->getSettings();
            auto& tags = settings->getTags();
            ImGui::TextWrapped("Project tags are shared by scenes, prefabs and scripts.");
            ImGui::Checkbox("Used in current scene", &usedTagsOnly);
            ImGui::SameLine();
            ImGui::Checkbox("Show deleted", &showDeletedTags);
            if (ImGui::Button("Add tag"))
            {
                editTag = -1; tagName.clear(); tagError.clear();
                ImGui::OpenPopup("Edit tag");
            }
            auto* uiSettings = MainWindow::getSettings();
            auto& sets = uiSettings->tagFavoriteSets;
            if (selectedTagSet >= static_cast<int>(sets.size())) selectedTagSet = -1;
            if (ImGui::BeginCombo("Favorite set", selectedTagSet < 0 ? "None" : sets[selectedTagSet].name.c_str()))
            {
                if (ImGui::Selectable("None", selectedTagSet < 0)) selectedTagSet = -1;
                for (int i = 0; i < static_cast<int>(sets.size()); ++i)
                    if (ImGui::Selectable(sets[i].name.c_str(), i == selectedTagSet)) selectedTagSet = i;
                ImGui::EndCombo();
            }
            if (ImGui::Button("New / copy set"))
            {
                tagSetName.clear(); tagSetError.clear(); ImGui::OpenPopup("Save favorite set");
            }
            ImGui::SameLine();
            ImGui::Checkbox("Favorites only", &favoriteTagsOnly);
            if (ImGui::BeginPopupModal("Save favorite set", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
            {
                ImGui::InputText("Set name", &tagSetName);
                ImGui::TextUnformatted("Copies the selected set, or creates an empty set. Tag definitions stay shared.");
                if (ImGui::Button("Save"))
                {
                    bool valid = tagSetName.find_first_not_of(" \t\r\n") != std::string::npos;
                    for (auto& set : sets) if (set.name == tagSetName) valid = false;
                    if (!valid) tagSetError = "Enter a unique, non-empty set name.";
                    else
                    {
                        TagFavoriteSet set;
                        set.name = tagSetName;
                        if (selectedTagSet >= 0) set.tags = sets[selectedTagSet].tags;
                        sets.push_back(set); selectedTagSet = static_cast<int>(sets.size()) - 1;
                        uiSettings->save(); ImGui::CloseCurrentPopup();
                    }
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
                ImGui::TextUnformatted(tagSetError.c_str());
                ImGui::EndPopup();
            }
            ImGui::Separator();
            ImGui::TextDisabled("0: Untagged (built in)");
            for (int i = 0; i < static_cast<int>(tags.size()); ++i)
            {
                int uses = 0;
                for (auto* object : Engine::getSingleton()->getGameObjects())
                    if (object->getTag() == i + 1) ++uses;
                bool favorite = false;
                if (selectedTagSet >= 0)
                {
                    auto& ids = sets[selectedTagSet].tags;
                    favorite = std::find(ids.begin(), ids.end(), i + 1) != ids.end();
                }
                if ((favoriteTagsOnly && !favorite) || (usedTagsOnly && uses == 0) || (!showDeletedTags && tags[i].empty())) continue;
                ImGui::PushID(i);
                if (selectedTagSet >= 0)
                {
                    if (ImGui::Checkbox("##favorite", &favorite))
                    {
                        auto& ids = sets[selectedTagSet].tags;
                        ids.erase(std::remove(ids.begin(), ids.end(), i + 1), ids.end());
                        if (favorite) ids.push_back(i + 1);
                        uiSettings->save();
                    }
                    ImGui::SameLine();
                }
                ImGui::Text("%d: %s (%d in scene)", i + 1,
                    tags[i].empty() ? "<deleted>" : CP_UNI(tags[i]).c_str(), uses);
                ImGui::SameLine();
                if (ImGui::SmallButton(tags[i].empty() ? "Restore" : "Rename"))
                {
                    editTag = i; tagName = CP_UNI(tags[i]); tagError.clear();
                    ImGui::PopID(); ImGui::OpenPopup("Edit tag"); ImGui::PushID(i);
                }
                if (!tags[i].empty())
                {
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Delete"))
                    {
                        editTag = i;
                        ImGui::PopID(); ImGui::OpenPopup("Delete tag"); ImGui::PushID(i);
                    }
                }
                ImGui::PopID();
            }
            if (ImGui::BeginPopupModal("Edit tag", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
            {
                ImGui::InputText("Name", &tagName);
                ImGui::TextUnformatted("Renaming keeps scene/prefab IDs. Update tag names in scripts yourself.");
                if (ImGui::Button("Save"))
                {
                    std::string name = CP_SYS(tagName);
                    const auto first = name.find_first_not_of(" \t\r\n");
                    name = first == std::string::npos ? "" : name.substr(first, name.find_last_not_of(" \t\r\n") - first + 1);
                    bool duplicate = name == "Untagged";
                    for (int i = 0; i < static_cast<int>(tags.size()); ++i)
                        if (i != editTag && tags[i] == name) duplicate = true;
                    if (name.empty() || duplicate) tagError = "Enter a non-empty, unique name.";
                    else
                    {
                        if (editTag < 0) tags.push_back(name);
                        else if (editTag < static_cast<int>(tags.size())) tags[editTag] = name;
                        settings->save();
                        MainWindow::getInspectorWindow()->updateCurrentEditor();
                        ImGui::CloseCurrentPopup();
                    }
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
                ImGui::TextUnformatted(tagError.c_str());
                ImGui::EndPopup();
            }
            if (ImGui::BeginPopupModal("Delete tag", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
            {
                ImGui::TextUnformatted("Retire this name? Scene/prefab IDs remain reserved and can be restored.");
                ImGui::TextUnformatted("Scripts using the old name must be updated. Other tag IDs will not move.");
                if (ImGui::Button("Delete"))
                {
                    if (editTag >= 0 && editTag < static_cast<int>(tags.size())) tags[editTag].clear();
                    settings->save();
                    MainWindow::getInspectorWindow()->updateCurrentEditor();
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
            }
        }
        ImGui::End();
    }

	void ProjectSettingsWindow::show(bool show)
	{
		opened = show;
	}
}
