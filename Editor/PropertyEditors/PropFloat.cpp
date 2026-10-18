#include "PropFloat.h"

#include <algorithm>

#include <imgui.h>
#include <imgui_internal.h>

namespace GX
{
	PropFloat::PropFloat(PropertyEditor* ed, std::string name, float val) : Property(ed, name)
	{
		setValue(val);

		flags = ImGuiTreeNodeFlags_AllowItemOverlap;
		fullSpaceWidth = false;

		guid1 = "##" + genGuid();
	}

	PropFloat::~PropFloat()
	{
	}

	void PropFloat::update(bool opened)
	{
		Property::update(opened);

		//ImGui::SameLine();
		ImGui::PushItemWidth(-1);
		if (isDraggable)
		{
			const bool hasRange = minVal != maxVal;
			ImGui::DragFloat(guid1.c_str(), &value, 0.05f,
				hasRange ? minVal : 0.0f,
				hasRange ? maxVal : 0.0f,
				"%.6f", hasRange ? ImGuiSliderFlags_AlwaysClamp : 0);
			if (ImGui::IsItemDeactivatedAfterEdit())
			{
				if (onChangeCallback != nullptr)
				{
					onChangeCallback(this, value);
				}
			}
		}
		else
		{
			ImGui::InputFloat(guid1.c_str(), &value);
			if (ImGui::IsItemDeactivatedAfterEdit())
			{
				if (minVal != maxVal)
				{
					value = std::max(minVal, std::min(maxVal, value));
				}

				if (onChangeCallback != nullptr)
					onChangeCallback(this, value);
			}
		}
		ImGui::PopItemWidth();
	}

	void PropFloat::setValue(float val)
	{
		value = val;
	}
}
