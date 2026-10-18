#include "PropertyEditor.h"
#include "PropVector3.h"

#include <imgui.h>
#include <imgui_internal.h>

#include "../../Engine/glm/gtc/type_ptr.hpp"
#include "../../Engine/UI/ImGUIWidgets.h"

namespace GX
{
	PropVector3::PropVector3(PropertyEditor* ed, std::string name, glm::vec3 val) : Property(ed, name)
	{
		setValue(val);

		flags = ImGuiTreeNodeFlags_AllowItemOverlap;
		fullSpaceWidth = false;
	}

	PropVector3::~PropVector3()
	{
		
	}

	void PropVector3::update(bool opened)
	{
		Property::update(opened);

		std::string _maskX = "%.7g";
		std::string _maskY = "%.7g";
		std::string _maskZ = "%.7g";

		if (showBadge)
		{
			if (!maskX.empty())
				_maskX = _maskX + " " + maskX;

			if (!maskY.empty())
				_maskY = _maskY + " " + maskY;

			if (!maskZ.empty())
				_maskZ = _maskZ + " " + maskZ;
		}

		bool changed = false;

		if (showBadge)
		{
			float w = ImGui::GetContentRegionAvail().x;
			bool wide = w > 160.0f;

			float inputW = w / 3.0f - 4.0f * 3.0f;
			if (wide)
				inputW = w / 3.0f - 6.5f * 3.0f;

			inputW = std::max(0.0f, inputW);

			//X
			ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8f, 0.05f, 0, 1));
			ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.8f, 0.05f, 0, 1));
			ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.8f, 0.05f, 0, 1));
			ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2, -1));
			ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 2.0f);
			ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 2));

			if (!wide)
				ImGui::Button("##X", ImVec2(4, 0));
			else
				ImGui::Button("X", ImVec2(12, 0));
			ImGui::PopStyleColor(3);
			ImGui::SameLine();
			ImGui::PopStyleVar(3);
			ImGui::SetNextItemWidth(inputW);
			if (isDraggable)
				ImGui::DragFloat(("##X_" + guid1).c_str(), &value.x, 0.05f, 0, 0, _maskX.c_str(), 0);
			else
				ImGui::InputFloat(("##X_" + guid1).c_str(), &value.x, 0.05f, 0.5f, _maskX.c_str());
			ImGui::SameLine();

			if (!changed)
				changed = ImGui::IsItemDeactivatedAfterEdit();

			//Y
			ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.7f, 0, 1));
			ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.2f, 0.7f, 0, 1));
			ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.2f, 0.7f, 0, 1));
			ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2, -1));
			ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 2.0f);
			ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 2));

			if (!wide)
				ImGui::Button("##Y", ImVec2(4, 0));
			else
				ImGui::Button("Y", ImVec2(12, 0));
			ImGui::PopStyleColor(3);
			ImGui::SameLine();
			ImGui::PopStyleVar(3);
			ImGui::SetNextItemWidth(inputW);
			if (isDraggable)
				ImGui::DragFloat(("##Y_" + guid1).c_str(), &value.y, 0.05f, 0, 0, _maskY.c_str(), 0);
			else
				ImGui::InputFloat(("##Y_" + guid1).c_str(), &value.y, 0.05f, 0.5f, _maskY.c_str());
			ImGui::SameLine();

			if (!changed)
				changed = ImGui::IsItemDeactivatedAfterEdit();

			//Z
			ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.41f, 0.92f, 1));
			ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.20f, 0.41f, 0.92f, 1));
			ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.20f, 0.41f, 0.92f, 1));
			ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2, -1));
			ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 2.0f);
			ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 2));

			if (!wide)
				ImGui::Button("##Z", ImVec2(4, 0));
			else
				ImGui::Button("Z", ImVec2(12, 0));

			ImGui::PopStyleColor(3);
			ImGui::SameLine();
			ImGui::PopStyleVar(3);
			ImGui::SetNextItemWidth(inputW);
			if (isDraggable)
				ImGui::DragFloat(("##Z" + guid1).c_str(), &value.z, 0.05f, 0, 0, _maskZ.c_str(), 0);
			else
				ImGui::InputFloat(("##Z_" + guid1).c_str(), &value.z, 0.05f, 0.5f, _maskZ.c_str());

			if (!changed)
				changed = ImGui::IsItemDeactivatedAfterEdit();
		}
		else
		{
			ImGui::PushItemWidth(-1);
			if (isDraggable)
				ImGui::DragFloat3(("##" + guid1).c_str(), glm::value_ptr(value), 0.05f, 0, 0, _maskX.c_str(), 0);
			else
				ImGui::InputFloat3(("##" + guid1).c_str(), glm::value_ptr(value), _maskX.c_str());
			ImGui::PopItemWidth();

			changed = ImGui::IsItemDeactivatedAfterEdit();
		}

		if (changed)
		{
			if (onChangeCallback != nullptr)
			{
				onChangeCallback(this, value);
			}
		}
	}

	void PropVector3::setValue(glm::vec3 val)
	{
		value = val;
	}
}
