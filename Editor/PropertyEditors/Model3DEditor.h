#pragma once

#include "../Engine/glm/vec2.hpp"
#include "../Engine/glm/vec3.hpp"
#include "../Engine/glm/gtc/quaternion.hpp"

#include "PropertyEditor.h"
#include <memory>
#include "../Engine/Assets/Model3DLoader.h"

#include <bgfx/bgfx.h>

namespace GX
{
	class TreeNode;
    class SScene;
	class Material;
	class RenderTexture;
	class Mesh;
	class SModel3DMeta;
	class PropButton;
	class PropFloat;
	class PropInt;
	class PropBool;

	class Model3DEditor : public PropertyEditor
	{
	private:
        struct PrefabContents
        {
            std::string path;
            std::shared_ptr<SScene> scene;
            std::vector<std::vector<size_t>> children;
            std::vector<size_t> roots;
        };
        std::vector<PrefabContents> prefabContents;
        void updatePrefabContents();
		std::vector<std::string> modelPaths;
		std::vector<SModel3DMeta*> metas;
		RenderTexture* rt = nullptr;
		PropButton* saveMeta = nullptr;
		PropFloat* maxSmoothingAngle = nullptr;

		PropInt* lodLevels = nullptr;
		PropFloat* lodError = nullptr;
		PropBool* lodPreserveTopology = nullptr;

		static int lodLevel;

		bool controlsHovered = false;

	public:
		Model3DEditor();
		~Model3DEditor();

		virtual void init(std::vector<std::string> paths);
		virtual void update();
		virtual void updateEditor();
        void setDraftPreview(const std::vector<Model3DLoader::ModelMeshData>& data, bool frame, glm::vec3 fallbackCenter, float fallbackExtent, glm::vec3 origin);
        void drawDraftPreview(float height, const std::function<void(const glm::mat4&, const glm::mat4&, const glm::mat4&, float, float, float, float)>& overlay);


	private:
		struct ModelMeshData
		{
		public:
			Mesh* mesh = nullptr;
			std::vector<Material*> materials;
			glm::mat4x4 transform = glm::identity<glm::mat4x4>();
		};

        bool draftPreviewMode = false;
        glm::vec3 draftOrbitCenter = glm::vec3(0.0f);
        glm::vec2 draftPan = glm::vec2(0.0f);
        float draftYaw = 0.0f, draftPitch = 0.0f, draftDistance = 1.0f;
        float draftPreviewHeight = 280.0f;
        std::function<void(const glm::mat4&, const glm::mat4&, const glm::mat4&, float, float, float, float)> draftOverlay;
		static glm::vec2 prevSize;
		bool mousePressed = false;
		bool mouseMPressed = false;
		glm::vec2 prevMousePos = glm::vec2(0.0f);
		glm::vec3 camPos = glm::vec3(-5, 0, 0);
		glm::highp_quat rotX = glm::identity<glm::highp_quat>();
		glm::highp_quat rotY = glm::identity<glm::highp_quat>();
		glm::mat4x4 meshTransform = glm::identity<glm::mat4x4>();
		float modelScale = 1.0f;

		bgfx::FrameBufferHandle gbuffer = { bgfx::kInvalidHandle };
		bgfx::FrameBufferHandle lightBuffer = { bgfx::kInvalidHandle };

		bgfx::TextureHandle gbufferTex[5] = { bgfx::kInvalidHandle, bgfx::kInvalidHandle, bgfx::kInvalidHandle, bgfx::kInvalidHandle, bgfx::kInvalidHandle };
		bgfx::TextureHandle lightBufferTex = { bgfx::kInvalidHandle };

		std::vector<ModelMeshData> meshes;

		void onUpdatePreview();
		void submitLightUniforms();
		void resetFrameBuffers(float w, float h);

		//Props
		void reloadModel();

		void onChangeScale(Property* prop, float value);
		void onChangeOptimizeMeshes(Property* prop, bool value);
		void onChangeCalculateNormals(Property* prop, bool value);
		void onChangeMaxSmoothingAngle(Property* prop, float value);
		void onChangeGenerateLightmapUVs(Property* prop, bool value);
		void onChangeLodGenerate(Property* prop, bool value);
		void onChangeLodLevels(Property* prop, int value);
		void onChangeLodError(Property* prop, float value);
		void onChangeLodPreserveTopology(Property* prop, bool value);

		void onClickApply(Property* prop);
	};
}