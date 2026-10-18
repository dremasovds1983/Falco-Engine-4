#include "../Classes/RenderTargetImageUV.h"
#include "ViewportWindow.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <iomanip>
#include <imgui.h>
#include <sstream>
#include <SDL2/SDL.h>

#include "MainWindow.h"
#include "SceneWindow.h"
#include "InspectorWindow.h"
#include "../Classes/Undo.h"

#include "../Engine/Core/Engine.h"
#include "../Engine/Core/GameObject.h"
#include "../Engine/Core/InputManager.h"
#include "../Engine/Core/Time.h"
#include "../Engine/Components/Camera.h"
#include "../Engine/Components/Transform.h"
#include "../Engine/Renderer/Renderer.h"
#include "../Engine/Renderer/RenderTexture.h"
#include "../Engine/Renderer/Primitives.h"

namespace GX
{
	static const int VIEWPORT_VIEW_OFFSET = CAMERA_VIEW_GROUP_SIZE * 2;
	static_assert(VIEWPORT_VIEW_OFFSET + CAMERA_VIEW_GROUP_SIZE <= RENDER_FIRST_SERVICE_PASS_ID,
		"Viewport camera view range overlaps renderer service views");

	static glm::ivec2 fitViewportRenderSize(float requestedWidth, float requestedHeight)
	{
		constexpr float maxWidth = 1280.0f;
		constexpr float maxHeight = 720.0f;
		const float width = std::max(8.0f, requestedWidth);
		const float height = std::max(8.0f, requestedHeight);
		const float scale = std::min(1.0f, std::min(maxWidth / width, maxHeight / height));
		return glm::ivec2(
			std::max(8, static_cast<int>(width * scale)),
			std::max(8, static_cast<int>(height * scale)));
	}

	ViewportWindow::ViewportWindow()
	{
	}

	ViewportWindow::~ViewportWindow()
	{
		if (cameraObject != nullptr)
		{
			Engine::getSingleton()->destroyGameObject(cameraObject);
			cameraObject = nullptr;
			renderCamera = nullptr;
		}

		delete renderTarget;
		renderTarget = nullptr;
	}

	void ViewportWindow::init()
	{
		const glm::ivec2 initialSize = fitViewportRenderSize(
			static_cast<float>(MainWindow::getSingleton()->getWidth() / 2),
			static_cast<float>(MainWindow::getSingleton()->getHeight() / 2));
		const int width = initialSize.x;
		const int height = initialSize.y;
		renderTarget = new RenderTexture(width, height);

		cameraObject = Engine::getSingleton()->createGameObject();
		cameraObject->setName("Camera Viewport Camera");
		cameraObject->setSerializable(false);
		cameraObject->getTransform()->setPosition(glm::vec3(0.0f, 2.0f, 0.0f));

		renderCamera = new Camera();
		renderCamera->setDepth(10000);
		renderCamera->setAutomaticRendering(false);
		renderCamera->setRenderTarget(renderTarget);
		renderCamera->setClearColor(Color(0.5f, 0.5f, 0.5f, 1.0f));
		renderCamera->setFOVy(75.0f);
		renderCamera->setIsEditorCamera(true);
		cameraObject->addComponent(renderCamera);
		if (renderCamera->getBackBuffer() == nullptr)
			renderCamera->initResources();

	}

	void ViewportWindow::update()
	{
		if (!opened)
			return;

		// Apply a settled dock resize at the beginning of a fresh frame. ImGui and
		// the manually rendered camera will therefore use the same texture handle.
		if (resizeCreatePending)
		{
			requestResize();
			resizeCreatePending = false;
		}
		else if (resizeApplyPending)
		{
			const glm::ivec2 renderSize =
				fitViewportRenderSize(pendingResizeSize.x, pendingResizeSize.y);
			RenderTexture* cameraBack = renderCamera->getBackBuffer();
			const bool targetMatches =
				renderTarget->getWidth() == renderSize.x
				&& renderTarget->getHeight() == renderSize.y;
			const bool cameraMatches = cameraBack != nullptr
				&& cameraBack->getWidth() == renderSize.x
				&& cameraBack->getHeight() == renderSize.y;

			if (!targetMatches || !cameraMatches)
			{
				renderCamera->setEnabled(false);
				renderCamera->destroyResources();
				renderTarget->release();
				resizeCreatePending = true;
			}
			resizeApplyPending = false;
		}

		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
		const bool begin = ImGui::Begin("Camera Viewport", &opened,
			ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
		ImGui::PopStyleVar();

		if (begin)
		{
			focused = ImGui::IsWindowFocused();
			position = glm::vec2(ImGui::GetWindowPos().x, ImGui::GetWindowPos().y);

			updateToolbar();
			updateCameraSource();

			const ImVec2 available = ImGui::GetContentRegionAvail();
			const ImVec2 imageSize(std::max(1.0f, available.x), std::max(1.0f, available.y));
			size = glm::vec2(imageSize.x, imageSize.y);
			if (size != oldSize && imageSize.x > 0.0f && imageSize.y > 0.0f)
			{
				oldSize = size;
				pendingResizeSize = size;
				resizeLastChange = std::chrono::steady_clock::now();
				resizePending = true;
			}
			else if (resizePending &&
				std::chrono::steady_clock::now() - resizeLastChange >= std::chrono::milliseconds(100))
			{
				resizePending = false;
				resizeApplyPending = true;
			}

			if (sourceCamera != nullptr && bgfx::isValid(renderTarget->getColorTextureHandle()))
				ImGui::Image((void*)renderTarget->getColorTextureHandle().idx, imageSize, renderTargetImageUV0(), renderTargetImageUV1());
			else
			{
				ImGui::InvisibleButton("##camera_viewport_empty", imageSize);
				const ImVec2 imageMin = ImGui::GetItemRectMin();
				const ImVec2 imageMax = ImGui::GetItemRectMax();
				ImGui::GetWindowDrawList()->AddRectFilled(imageMin, imageMax, IM_COL32(64, 64, 64, 255));
				const char* message = "No Camera";
				const ImVec2 textSize = ImGui::CalcTextSize(message);
				ImGui::GetWindowDrawList()->AddText(
					ImVec2((imageMin.x + imageMax.x - textSize.x) * 0.5f,
						(imageMin.y + imageMax.y - textSize.y) * 0.5f),
					IM_COL32(190, 190, 190, 255), message);
			}
			hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
			if (hovered && ImGui::IsMouseDown(ImGuiMouseButton_Right))
			{
				ImGui::SetWindowFocus();
				focused = true;
			}
			updateNavigation();
			// Navigation changes the source camera itself. Refresh the render copy
			// in the same frame so Camera Viewport responds without a frame of lag.
			updateCameraSource();
			if (showStats)
				drawStats();

			// In play mode Renderer owns all registered cameras and assigns each one
			// a distinct view group. Outside play mode Viewport renders explicitly.
			if (sourceCamera != nullptr && !Engine::getSingleton()->getIsRuntimeMode()
				&& renderCamera->getBackBuffer() != nullptr)
				renderCamera->renderFrame(VIEWPORT_VIEW_OFFSET);
		}
		else
		{
			hovered = false;
			focused = false;
		}

		ImGui::End();
	}

	void ViewportWindow::updateNavigation()
	{
		if (!navigating && hovered && sourceCamera != nullptr
			&& ImGui::IsMouseClicked(ImGuiMouseButton_Right))
		{
			navigating = true;
			navigationCamera = sourceCamera;
			Transform* transform = navigationCamera->getTransform();
			navigationStartPosition = transform->getPosition();
			navigationStartRotation = transform->getRotation();
		}

		if (!navigating)
			return;

		if (navigationCamera == nullptr || navigationCamera->getGameObject() == nullptr
			|| navigationCamera != sourceCamera)
		{
			navigating = false;
			navigationCamera = nullptr;
			return;
		}

		Transform* transform = navigationCamera->getTransform();
		if (ImGui::IsMouseDown(ImGuiMouseButton_Right))
		{
			const ImVec2 mouseDelta = ImGui::GetIO().MouseDelta;
			if (mouseDelta.x != 0.0f || mouseDelta.y != 0.0f)
			{
				transform->yaw(-mouseDelta.x * 0.15f, true);
				transform->pitch(mouseDelta.y * 0.15f, false);
			}

			if (!ImGui::GetIO().WantTextInput)
			{
				const bool fast = InputManager::getSingleton()->getKey(SDL_SCANCODE_LSHIFT);
				const float speed = MainWindow::getSceneWindow()->getNavigationSpeed(fast);
				const float distance = speed * Time::getDeltaTime();
				if (InputManager::getSingleton()->getKey(SDL_SCANCODE_W))
					transform->translate(glm::vec3(0, 0, 1) * distance);
				if (InputManager::getSingleton()->getKey(SDL_SCANCODE_S))
					transform->translate(-glm::vec3(0, 0, 1) * distance);
				if (InputManager::getSingleton()->getKey(SDL_SCANCODE_A))
					transform->translate(glm::vec3(1, 0, 0) * distance);
				if (InputManager::getSingleton()->getKey(SDL_SCANCODE_D))
					transform->translate(-glm::vec3(1, 0, 0) * distance);
				if (InputManager::getSingleton()->getKey(SDL_SCANCODE_Q))
					transform->translate(-glm::vec3(0, 1, 0) * distance);
				if (InputManager::getSingleton()->getKey(SDL_SCANCODE_E))
					transform->translate(glm::vec3(0, 1, 0) * distance);
			}
		}

		if (ImGui::IsMouseReleased(ImGuiMouseButton_Right))
			finishNavigation();
	}

	void ViewportWindow::finishNavigation()
	{
		if (!navigating || navigationCamera == nullptr || navigationCamera->getGameObject() == nullptr)
		{
			navigating = false;
			navigationCamera = nullptr;
			return;
		}

		Transform* transform = navigationCamera->getTransform();
		const glm::vec3 endPosition = transform->getPosition();
		const glm::quat endRotation = transform->getRotation();
		const bool positionChanged = glm::length(endPosition - navigationStartPosition) > 0.000001f;
		const bool rotationChanged = std::abs(glm::dot(endRotation, navigationStartRotation)) < 0.999999f;
		if (positionChanged || rotationChanged)
		{
			UndoData* undo = Undo::addUndo("Navigate camera in Camera Viewport");
			undo->vec3Data.resize(2);
			undo->quatData.resize(2);
			undo->vec3Data[0][transform] = navigationStartPosition;
			undo->vec3Data[1][transform] = endPosition;
			undo->quatData[0][transform] = navigationStartRotation;
			undo->quatData[1][transform] = endRotation;
			undo->undoAction = [](UndoData* data)
			{
				for (const auto& value : data->vec3Data[0])
				{
					Transform* target = static_cast<Transform*>(value.first);
					target->setPosition(value.second);
					target->setRotation(data->quatData[0][target]);
				}
				MainWindow::getInspectorWindow()->updateObjectEditorTransform();
			};
			undo->redoAction = [](UndoData* data)
			{
				for (const auto& value : data->vec3Data[1])
				{
					Transform* target = static_cast<Transform*>(value.first);
					target->setPosition(value.second);
					target->setRotation(data->quatData[1][target]);
				}
				MainWindow::getInspectorWindow()->updateObjectEditorTransform();
			};
		}
		MainWindow::getInspectorWindow()->updateObjectEditorTransform();

		navigating = false;
		navigationCamera = nullptr;
	}

	void ViewportWindow::updateToolbar()
	{
		ImGui::SetCursorPos(ImVec2(5.0f, 22.0f));
		std::vector<Camera*> cameras;
		for (Camera* camera : Renderer::getSingleton()->getCameras())
		{
			if (camera != nullptr && camera != renderCamera && !camera->getIsEditorCamera() && camera->getGameObject() != nullptr)
				cameras.push_back(camera);
		}

		if (std::find(cameras.begin(), cameras.end(), sourceCamera) == cameras.end())
			sourceCamera = cameras.empty() ? nullptr : cameras.front();

		ImGui::SetNextItemWidth(180.0f);
		const std::string currentName = sourceCamera != nullptr ? sourceCamera->getGameObject()->getName() : "No Camera";
		if (ImGui::BeginCombo("##viewport_camera", currentName.c_str()))
		{
			for (Camera* camera : cameras)
			{
				const bool selected = camera == sourceCamera;
				if (ImGui::Selectable(camera->getGameObject()->getName().c_str(), selected))
					sourceCamera = camera;
				if (selected) ImGui::SetItemDefaultFocus();
			}
			ImGui::EndCombo();
		}

		ImGui::SameLine();
		updateCameraSettings(sourceCamera, Engine::getSingleton()->getIsRuntimeMode());
	}

	void ViewportWindow::updateCameraSettings(Camera* camera, bool readOnly)
	{
		ImGui::SetNextItemWidth(140.0f);
		if (!ImGui::BeginCombo("##viewport_camera_settings", "Camera settings", ImGuiComboFlags_HeightLarge))
			return;

		if (camera == nullptr)
		{
			ImGui::TextDisabled("No camera selected");
			ImGui::EndCombo();
			return;
		}

		if (readOnly) ImGui::BeginDisabled();
		float cameraSpeed = MainWindow::getSceneWindow()->getNavigationSpeed(false);
		ImGui::Text("Camera speed");
		ImGui::SetNextItemWidth(-1.0f);
		if (ImGui::DragFloat("##viewport_camera_speed", &cameraSpeed, 0.05f, 0.05f, FLT_MAX, "%.3f", ImGuiSliderFlags_AlwaysClamp))
			MainWindow::getSceneWindow()->setNavigationSpeed(cameraSpeed);

		float farPlane = camera->getFar();
		ImGui::Separator();
		ImGui::Text("Clipping plane distance");
		ImGui::SetNextItemWidth(-1.0f);
		if (ImGui::DragFloat("##viewport_camera_far", &farPlane, 0.1f, camera->getNear() + 0.001f, FLT_MAX, "%.3f", ImGuiSliderFlags_AlwaysClamp))
			camera->setFar(farPlane);

		const bool perspective = camera->getProjectionType() == ProjectionType::Perspective;
		ImGui::Separator();
		ImGui::Text("Projection");
		ImGui::SetNextItemWidth(-1.0f);
		if (ImGui::BeginCombo("##viewport_projection", perspective ? "Perspective" : "Orthographic"))
		{
			if (ImGui::Selectable("Perspective", perspective)) camera->setProjectionType(ProjectionType::Perspective);
			if (ImGui::Selectable("Orthographic", !perspective)) camera->setProjectionType(ProjectionType::Orthographic);
			ImGui::EndCombo();
		}

		if (perspective)
		{
			float fov = camera->getFOVy();
			ImGui::Text("Field of view");
			ImGui::SetNextItemWidth(-1.0f);
			if (ImGui::DragFloat("##viewport_camera_fov", &fov, 0.1f, 1.0f, 179.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp))
				camera->setFOVy(fov);
		}
		else
		{
			float orthographicSize = camera->getOrthographicSize();
			ImGui::Text("Size");
			ImGui::SetNextItemWidth(-1.0f);
			if (ImGui::DragFloat("##viewport_camera_ortho", &orthographicSize) && orthographicSize > 0.0f && orthographicSize < FLT_MAX)
				camera->setOrthographicSize(orthographicSize);
		}
		if (readOnly) ImGui::EndDisabled();
		ImGui::EndCombo();
	}

	void ViewportWindow::updateViewSettings(Camera* camera, bool readOnly)
	{
		ImGui::SetNextItemWidth(130.0f);
		if (!ImGui::BeginCombo("##viewport_view_settings", "View settings", ImGuiComboFlags_HeightLarge))
			return;

		ImGui::Checkbox("Show stats", &showStats);
		ImGui::Separator();
		if (readOnly) ImGui::BeginDisabled();
		bool occlusion = camera != nullptr && camera->getOcclusionCulling();
		if (ImGui::Checkbox("Occlusion culling", &occlusion) && camera != nullptr)
			camera->setOcclusionCulling(occlusion);
		if (readOnly) ImGui::EndDisabled();
		ImGui::Separator();
		bool shadows = Renderer::getSingleton()->getShadowsEnabled();
		if (ImGui::Checkbox("Shadows", &shadows)) Renderer::getSingleton()->setShadowsEnabled(shadows);
		ImGui::EndCombo();
	}

	void ViewportWindow::drawStats()
	{
		Renderer* renderer = Renderer::getSingleton();
		const int fps = Time::getFramesPerSecond();
		const float frameTime = renderer->getFrameTime();
		const float cpuTime = renderer->getCpuTime();
		const float gpuTime = renderer->getGpuTime();
		const uint32_t numDrawCalls = renderer->getNumDrawCalls();
		const uint32_t numTriangles = renderer->getNumTriangles();
		const int64_t gpuMemUsed = renderer->getGpuMemoryUsed();

		const std::string frameTimeValue = std::to_string(frameTime);
		const std::string cpuTimeValue = std::to_string(cpuTime);
		const std::string gpuTimeValue = std::to_string(gpuTime);
		const std::string frameTimeStr = frameTimeValue.substr(0, frameTimeValue.find(".") + 4);
		const std::string cpuTimeStr = cpuTimeValue.substr(0, cpuTimeValue.find(".") + 4);
		const std::string gpuTimeStr = gpuTimeValue.substr(0, gpuTimeValue.find(".") + 4);

		ImVec4 fpsColor(0, 1, 0, 1);
		if (fps >= 0 && fps < 30) fpsColor = ImVec4(1, 0, 0, 1);
		if (fps >= 30 && fps < 55) fpsColor = ImVec4(1, 1, 0, 1);

		int height = 133;
		if (Camera::getDynamicResolution() < 1.0f)
			height += 20;

		ImGui::SetCursorPos(ImVec2(5, 52));
		ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.15f, 0.15f, 0.15f, 0.8f));
		ImGui::BeginChild("Stats", ImVec2(200, static_cast<float>(height)), true);
		ImGui::PopStyleColor();
		ImGui::TextColored(fpsColor, ("FPS: " + std::to_string(fps)).c_str());
		ImGui::Text(("Frame time (ms): " + frameTimeStr).c_str());
		ImGui::Separator();
		ImGui::Text(("CPU time (ms): " + cpuTimeStr).c_str());
		ImGui::Text(("GPU time (ms): " + gpuTimeStr).c_str());
		ImGui::Separator();
		ImGui::Text(("Draw calls: " + std::to_string(numDrawCalls)).c_str());
		ImGui::Text(("Triangles: " + std::to_string(numTriangles)).c_str());
		ImGui::Separator();
		ImGui::Text(("GPU memory used: " + std::to_string(gpuMemUsed) + "MB").c_str());
		if (Camera::getDynamicResolution() < 1.0f)
		{
			ImGui::Separator();
			std::ostringstream stream;
			stream << std::fixed << std::setprecision(2) << Camera::getDynamicResolution();
			ImGui::Text(("Dynamic resolution: " + stream.str()).c_str());
		}
		ImGui::EndChild();
	}

	void ViewportWindow::updateCameraSource()
	{
		if (sourceCamera == nullptr || sourceCamera->getGameObject() == nullptr)
		{
			if (renderCamera->getEnabled())
				renderCamera->setEnabled(false);
			renderCamera->setIsEditorCamera(true);
			return;
		}

		if (!renderCamera->getEnabled())
			renderCamera->setEnabled(true);
		renderCamera->setIsEditorCamera(false);

		Transform* sourceTransform = sourceCamera->getTransform();
		Transform* targetTransform = renderCamera->getTransform();
		targetTransform->setPosition(sourceTransform->getPosition());
		targetTransform->setRotation(sourceTransform->getRotation());

		renderCamera->setProjectionType(sourceCamera->getProjectionType());
		renderCamera->setFOVy(sourceCamera->getFOVy());
		renderCamera->setNear(sourceCamera->getNear());
		renderCamera->setFar(sourceCamera->getFar());
		renderCamera->setOrthographicSize(sourceCamera->getOrthographicSize());
		renderCamera->setClearColor(sourceCamera->getClearColor());
		renderCamera->setClearFlags(sourceCamera->getClearFlags());
		renderCamera->setScreenOffsetLeft(sourceCamera->getScreenOffsetLeft());
		renderCamera->setScreenOffsetTop(sourceCamera->getScreenOffsetTop());
		renderCamera->setFlipHorizontal(sourceCamera->getFlipHorizontal());
		renderCamera->setFlipVertical(sourceCamera->getFlipVertical());
		renderCamera->setInvertCulling(sourceCamera->getInvertCulling());
		renderCamera->setOcclusionCulling(sourceCamera->getOcclusionCulling());
		renderCamera->getCullingMask() = sourceCamera->getCullingMask();
	}

	void ViewportWindow::requestResize()
	{
		const glm::ivec2 renderSize = fitViewportRenderSize(pendingResizeSize.x, pendingResizeSize.y);
		const int width = renderSize.x;
		const int height = renderSize.y;
		const bool targetMatches =
			bgfx::isValid(renderTarget->getFrameBufferHandle())
			&& bgfx::isValid(renderTarget->getColorTextureHandle())
			&& renderTarget->getWidth() == width && renderTarget->getHeight() == height;
		RenderTexture* cameraBack = renderCamera->getBackBuffer();
		const bool cameraMatches = cameraBack != nullptr
			&& cameraBack->getWidth() == width && cameraBack->getHeight() == height;
		if (targetMatches && cameraMatches)
			return;

		if (!targetMatches)
			renderTarget->reset(width, height);
		renderCamera->initResources();
		renderCamera->setEnabled(sourceCamera != nullptr);
	}
}
