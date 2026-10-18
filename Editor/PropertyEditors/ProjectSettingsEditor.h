#pragma once

#include "PropertyEditor.h"

namespace GX
{
	class TreeNode;
	class Property;
	class DialogProgress;
    class PropComboBox;

	class ProjectSettingsEditor : public PropertyEditor
	{
	public:
		ProjectSettingsEditor();
		~ProjectSettingsEditor();

		virtual void init();
		virtual void update();
		virtual void updateEditor();

	private:
		DialogProgress* progressDialog = nullptr;
        PropComboBox* graphicsPreset = nullptr;

		void onChangeUseNativeResolution(Property* prop, bool val);
		void onChangeExclusiveMode(Property* prop, bool val);
		void onChangeFullScreen(Property* prop, bool val);
		void onChangeScreenWidth(Property* prop, int val);
		void onChangeScreenHeight(Property* prop, int val);

		void onChangeShadowsEnabled(Property* prop, bool val);
		void onChangeDirShadows(Property* prop, std::string val);
		void onChangeSpotShadows(Property* prop, std::string val);
		void onChangePointShadows(Property* prop, std::string val);
		void onChangeCascades(Property* prop, std::string val);
		void onChangeSamples(Property* prop, std::string val);
		void onChangeCascadesBlending(Property* prop, bool val);
		void onChangeShadowDistance(Property* prop, float val);
		void onChangeVSync(Property* prop, bool val);
		void onChangeFXAA(Property* prop, bool val);
		void onChangeUseDynamicResolution(Property* prop, bool val);
		void onChangeAutoDynamicResolution(Property* prop, bool val);
		void onChangeAutoDynamicResolutionTargetFPS(Property* prop, float val);
		void onChangeDynamicResolution(Property* prop, float val);

		void onChangeTextureCompression(Property* prop, int val);
		void onChangeTextureCompressionQuality(Property* prop, int val);
		void onChangeTextureMaxResolution(Property* prop, int val);

		void onChangeEnableSteamAPI(Property* prop, bool val);
		void onChangeSteamAppID(Property* prop, int val);


		void onChangeLayer(Property* prop, std::string val);
		void onAddLayer(TreeNode* prop);
		void onLayerPopup(TreeNode* node, int val);

		void onChangeCollisionMask(int i, int j, bool value);
	};
}