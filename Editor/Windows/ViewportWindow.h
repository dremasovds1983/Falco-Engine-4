#pragma once

#include <string>
#include <chrono>

#include "../Engine/glm/vec2.hpp"
#include "../Engine/glm/vec3.hpp"
#include "../Engine/glm/gtc/quaternion.hpp"

namespace GX
{
	class Camera;
	class GameObject;
	class RenderTexture;

	class ViewportWindow
	{
	private:
		bool opened = false;
		bool hovered = false;
		bool focused = false;
		bool showStats = false;
		glm::vec2 position = glm::vec2(0.0f);
		glm::vec2 size = glm::vec2(0.0f);
		glm::vec2 oldSize = glm::vec2(0.0f);
		glm::vec2 pendingResizeSize = glm::vec2(0.0f);
		std::chrono::steady_clock::time_point resizeLastChange = std::chrono::steady_clock::now();
		bool resizePending = false;
		bool resizeApplyPending = false;
		bool resizeCreatePending = false;

		Camera* sourceCamera = nullptr;
		Camera* navigationCamera = nullptr;
		glm::vec3 navigationStartPosition = glm::vec3(0.0f);
		glm::quat navigationStartRotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
		bool navigating = false;
		GameObject* cameraObject = nullptr;
		Camera* renderCamera = nullptr;
		RenderTexture* renderTarget = nullptr;

		void updateToolbar();
		void updateCameraSettings(Camera* camera, bool readOnly);
		void updateViewSettings(Camera* camera, bool readOnly);
		void drawStats();
		void updateCameraSource();
		void updateNavigation();
		void finishNavigation();
		void requestResize();

	public:
		ViewportWindow();
		~ViewportWindow();

		void init();
		void update();
		void show(bool value) { opened = value; }
		bool getVisible() const { return opened; }
		Camera* getCamera() const { return renderCamera; }
	};
}
