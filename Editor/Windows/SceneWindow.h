#pragma once

#include <string>
#include <vector>
#include <functional>
#include <map>
#include <chrono>

#include "../Engine/glm/vec2.hpp"
#include "../Engine/glm/vec3.hpp"

namespace GX
{
	class Texture;
	class RenderTexture;
	class Camera;
	class Gizmo;
	class Transform;
	class GameObject;
	class UndoData;
	struct Ray;

	class SceneWindow
	{
		friend class MainWindow;
		friend class NavigationWindow;

	protected:
		RenderTexture* renderTarget = nullptr;

		void onResize();

	private:
		bool opened = true;
		std::string windowTitle = "Scene";
		std::string dndId = "";
		Gizmo* gizmo = nullptr;
		GameObject* cameraObject = nullptr;
		int manualViewOffset = -1;
		bool persistentSettings = true;
		static SceneWindow* inputOwner;

		std::string mouseDownSubscription;
		std::string mouseUpSubscription;
		std::string mouseMoveSubscription;
		std::string mouseWheelSubscription;
		std::string renderSubscription;

		bool lButtonDown = false;
		bool rButtonDown = false;
		bool mButtonDown = false;
		bool ctrlPressed = false;
		bool shiftPressed = false;
		bool mouseOver = false;

		glm::vec2 position = glm::vec2(0, 0);
		glm::vec2 size = glm::vec2(0, 0);
		glm::vec2 oldSize = glm::vec2(0, 0);
		glm::vec2 pendingResizeSize = glm::vec2(0, 0);
		std::chrono::steady_clock::time_point resizeLastChange = std::chrono::steady_clock::now();
		bool resizePending = false;
		bool resizeApplyPending = false;
		bool resizeCreatePending = false;
		bool sceneRenderValidationPending = false;
		glm::vec2 prevMousePos = glm::vec2(0, 0);

		std::map<GameObject*, int> layerMem;

		float cameraSpeed1 = 0.0f;
		float cameraSpeed2 = 0.0f;
		float cameraSpeed3 = 0.0f;
		int cameraSpeedPreset = 0;

		float cameraSpeed = 1.0f;
		float cameraSpeedNormal = 1.0f;
		float cameraSpeedFast = 2.0f;

		float gizmoIconsScale = 0.35f;

		bool focused = false;
		bool hovered = false;
		bool wasHovered = false;

		bool showStats = false;
		bool showNavMesh = false;
		bool showGrid = true;

		Texture * cameraIcon = nullptr;
		Texture * viewIcon = nullptr;
		Texture * shadowsIcon = nullptr;
		Texture * gridIcon = nullptr;
		Texture * navMeshIcon = nullptr;

		Texture * lightPointGizmoIcon = nullptr;
		Texture * lightSpotGizmoIcon = nullptr;
		Texture * lightDirGizmoIcon = nullptr;
		Texture * cameraGizmoIcon = nullptr;

		Camera* renderCamera = nullptr;

		Transform* dragObject = nullptr;

		glm::vec3 cameraPivot = glm::vec3(0.0f);

		void dropObject(GameObject * obj);

		void updateToolbar();

		void saveSettings();
		void loadSettings();

		void onGizmoSelect(std::vector<Transform*>& nodes, std::vector<Transform*>& prevNodes, void* userData);
		void selectObjects(std::vector<Transform*>& nodes, void* userData);

		void mouseDown(int x, int y, int mb);
		void mouseUp(int x, int y, int mb);
		void mouseMove(int x, int y);
		void mouseWheel(int x, int y);

		std::function<void()> onMouseHover = nullptr;
		std::function<void()> onMouseExit = nullptr;

		UndoData* undoData = nullptr;

		void onGizmoStartManipulate(std::vector<Transform*>& objects);
		void onGizmoEndManipulate(std::vector<Transform*>& objects);
		void onGizmoManipulate(std::vector<Transform*>& objects);

	public:
		SceneWindow(std::string title = "Scene", int viewOffset = -1, bool savePersistentSettings = true);
		~SceneWindow();

		void init();
		void update();
		void onSceneLoaded() { sceneRenderValidationPending = true; }
		void show(bool show) { opened = show; }
		bool getVisible() { return opened; }
		const std::string& getTitle() const { return windowTitle; }
		void drawGizmos(int viewId, int viewLayer, Camera * camera);

		glm::vec2 getPosition();
		glm::vec2 getSize();
		bool isFocused();
		bool isHovered();

		float getToolbarHeight();
		Camera* getCamera() { return renderCamera; }
		Ray getCameraToViewportRay(float screenX, float screenY) const;
		float getNavigationSpeed(bool fast) const { return fast ? cameraSpeedFast : cameraSpeedNormal; }
		void setNavigationSpeed(float value);

		Gizmo* getGizmo() { return gizmo; }

		void setShowGrid(bool value);
		void setShowNavMesh(bool value);
	};
}
