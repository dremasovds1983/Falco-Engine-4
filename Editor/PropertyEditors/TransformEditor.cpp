#include "TransformEditor.h"

#include <cmath>

#include "../Windows/MainWindow.h"
#include "../Windows/SceneWindow.h"
#include "../Windows/InspectorWindow.h"

#include "../Engine/Core/GameObject.h"
#include "../Engine/Renderer/BatchedGeometry.h"
#include "../Engine/Renderer/CSGGeometry.h"
#include "../Engine/Components/Transform.h"
#include "../Engine/Components/CSGBrush.h"
#include "../Engine/Assets/Texture.h"

#include "../Engine/Classes/Helpers.h"
#include "../Engine/Math/Mathf.h"
#include "../Engine/Gizmo/Gizmo.h"

#include "Property.h"
#include "PropVector3.h"

#include "../Classes/Undo.h"

namespace
{
	glm::vec3 cleanEulerForEditor(glm::vec3 value)
	{
		for (int axis = 0; axis < 3; ++axis)
		{
			const float nearestInteger = std::round(value[axis]);
			if (std::abs(value[axis] - nearestInteger) < 0.0001f)
				value[axis] = nearestInteger;
		}

		return value;
	}
}

namespace GX
{
	TransformEditor::TransformEditor()
	{
		setEditorName("TransformEditor");
	}

	TransformEditor::~TransformEditor()
	{
	}

	void TransformEditor::init(std::vector<Component*> comps)
	{
		ComponentEditor::init(comps);

		Transform* currentTransform = (Transform*)comps[0];

		glm::vec3 eulerAngles = cleanEulerForEditor(Mathf::toEuler(currentTransform->getLocalRotation()));

		position = new PropVector3(this, "Position", currentTransform->getLocalPosition());
		rotation = new PropVector3(this, "Rotation", eulerAngles);
		scale = new PropVector3(this, "Scale", currentTransform->getLocalScale());

		position->setShowBadge(true);
		rotation->setShowBadge(true);
		scale->setShowBadge(true);

		rotation->setMaskX("°");
		rotation->setMaskY("°");
		rotation->setMaskZ("°");

		position->setOnChangeCallback([=](Property* prop, glm::vec3 val) { onChangePosition(prop, val); });
		rotation->setOnChangeCallback([=](Property* prop, glm::vec3 val) { onChangeRotation(prop, val); });
		scale->setOnChangeCallback([=](Property* prop, glm::vec3 val) { onChangeScale(prop, val); });

		addProperty(position);
		addProperty(rotation);
		addProperty(scale);
	}

	void TransformEditor::onChangePosition(Property* prop, glm::vec3 val)
	{
		//Undo
		UndoData* undoData = Undo::addUndo("Change object position");
		undoData->vec3Data.resize(2);

		for (auto& comp : components)
		{
			undoData->vec3Data[0][comp] = ((Transform*)comp)->getLocalPosition();
			undoData->vec3Data[1][comp] = val;
		}

		undoData->undoAction = [=](UndoData* data)
		{
			bool updBatches = false;
			bool updCsg = false;

			std::vector<Component*> comps;

			for (auto& d : data->vec3Data[0])
			{
				Transform* t = (Transform*)d.first;
				t->setLocalPosition(d.second);

				// Collect every selected subtree.
				{
					t->iterateChildren([&updBatches, &updCsg, &comps](Transform* ct) -> bool
						{
							if (ct->getGameObject()->getBatchingStatic())
								updBatches = true;

							CSGBrush* csgBrush = (CSGBrush*)ct->getGameObject()->getComponent(CSGBrush::COMPONENT_TYPE);
							if (csgBrush != nullptr)
							{
								comps.push_back(csgBrush);

								csgBrush->rebuild();
								updCsg = true;
							}

							return true;
						}
					);
				}
			}

			MainWindow::getInspectorWindow()->updateObjectEditorTransform();
			MainWindow::getSceneWindow()->getGizmo()->updatePosition();

			if (updBatches)
				MainWindow::addOnEndUpdateCallback([=]() { BatchedGeometry::getSingleton()->rebuild(); });

			if (updCsg)
				MainWindow::addOnEndUpdateCallback([=]() { CSGGeometry::getSingleton()->rebuild(comps); });
		};

		undoData->redoAction = [=](UndoData* data)
		{
			bool updBatches = false;
			bool updCsg = false;

			std::vector<Component*> comps;

			for (auto& d : data->vec3Data[1])
			{
				Transform* t = (Transform*)d.first;
				t->setLocalPosition(d.second);

				// Collect every selected subtree.
				{
					t->iterateChildren([&updBatches, &updCsg, &comps](Transform* ct) -> bool
						{
							if (ct->getGameObject()->getBatchingStatic())
								updBatches = true;

							CSGBrush* csgBrush = (CSGBrush*)ct->getGameObject()->getComponent(CSGBrush::COMPONENT_TYPE);
							if (csgBrush != nullptr)
							{
								comps.push_back(csgBrush);

								csgBrush->rebuild();
								updCsg = true;
							}

							return true;
						}
					);
				}
			}

			MainWindow::getInspectorWindow()->updateObjectEditorTransform();
			MainWindow::getSceneWindow()->getGizmo()->updatePosition();

			if (updBatches)
				MainWindow::addOnEndUpdateCallback([=]() { BatchedGeometry::getSingleton()->rebuild(); });

			if (updCsg)
				MainWindow::addOnEndUpdateCallback([=]() { CSGGeometry::getSingleton()->rebuild(comps); });
		};
		//

		bool updBatches = false;
		bool updCsg = false;
		std::vector<Component*> csgBrushes;

		for (auto it = components.begin(); it != components.end(); ++it)
		{
			Transform* t = (Transform*)*it;
			t->setLocalPosition(val);

			// Collect every selected subtree.
			{
				t->iterateChildren([&updBatches, &updCsg, &csgBrushes](Transform* ct) -> bool
					{
						if (ct->getGameObject()->getBatchingStatic())
							updBatches = true;

						CSGBrush* csgBrush = (CSGBrush*)ct->getGameObject()->getComponent(CSGBrush::COMPONENT_TYPE);
						if (csgBrush != nullptr)
						{
							csgBrushes.push_back(csgBrush);

							csgBrush->rebuild();
							updCsg = true;
						}

						return true;
					}
				);
			}
		}

		MainWindow::getSceneWindow()->getGizmo()->updatePosition();

		if (updBatches)
			MainWindow::addOnEndUpdateCallback([=]() { BatchedGeometry::getSingleton()->rebuild(); });

		if (updCsg)
			MainWindow::addOnEndUpdateCallback([csgBrushes]() { CSGGeometry::getSingleton()->rebuild(csgBrushes); });
	}

	void TransformEditor::onChangeRotation(Property* prop, glm::vec3 val)
	{
		
		// --- crutch: protection from crash at X = 90 (Directional Light) ---
		if (fabs(val.x - 90.0f) < 1e-4f)
			val.x = 89.999f;

		glm::highp_quat rotation = Mathf::toQuaternion(val);

		//Undo
		UndoData* undoData = Undo::addUndo("Change object rotation");
		undoData->quatData.resize(2);

		for (auto& comp : components)
		{
			undoData->quatData[0][comp] = ((Transform*)comp)->getLocalRotation();
			undoData->quatData[1][comp] = rotation;
		}

		undoData->undoAction = [=](UndoData* data)
		{
			bool updBatches = false;
			bool updCsg = false;

			std::vector<Component*> comps;

			for (auto& d : data->quatData[0])
			{
				Transform* t = (Transform*)d.first;
				t->setLocalRotation(d.second);

				// Collect every selected subtree.
				{
					t->iterateChildren([&updBatches, &updCsg, &comps](Transform* ct) -> bool
						{
							if (ct->getGameObject()->getBatchingStatic())
								updBatches = true;

							CSGBrush* csgBrush = (CSGBrush*)ct->getGameObject()->getComponent(CSGBrush::COMPONENT_TYPE);
							if (csgBrush != nullptr)
							{
								comps.push_back(csgBrush);

								csgBrush->rebuild();
								updCsg = true;
							}

							return true;
						}
					);
				}
			}

			MainWindow::getInspectorWindow()->updateObjectEditorTransform();
			MainWindow::getSceneWindow()->getGizmo()->updatePosition();

			if (updBatches)
				MainWindow::addOnEndUpdateCallback([=]() { BatchedGeometry::getSingleton()->rebuild(); });

			if (updCsg)
				MainWindow::addOnEndUpdateCallback([=]() { CSGGeometry::getSingleton()->rebuild(comps); });
		};

		undoData->redoAction = [=](UndoData* data)
		{
			bool updBatches = false;
			bool updCsg = false;

			std::vector<Component*> comps;

			for (auto& d : data->quatData[1])
			{
				Transform* t = (Transform*)d.first;
				t->setLocalRotation(d.second);

				// Collect every selected subtree.
				{
					t->iterateChildren([&updBatches, &updCsg, &comps](Transform* ct) -> bool
						{
							if (ct->getGameObject()->getBatchingStatic())
								updBatches = true;

							CSGBrush* csgBrush = (CSGBrush*)ct->getGameObject()->getComponent(CSGBrush::COMPONENT_TYPE);
							if (csgBrush != nullptr)
							{
								comps.push_back(csgBrush);

								csgBrush->rebuild();
								updCsg = true;
							}

							return true;
						}
					);
				}
			}

			MainWindow::getInspectorWindow()->updateObjectEditorTransform();
			MainWindow::getSceneWindow()->getGizmo()->updatePosition();

			if (updBatches)
				MainWindow::addOnEndUpdateCallback([=]() { BatchedGeometry::getSingleton()->rebuild(); });

			if (updCsg)
				MainWindow::addOnEndUpdateCallback([=]() { CSGGeometry::getSingleton()->rebuild(comps); });
		};
		//

		bool updBatches = false;
		bool updCsg = false;
		std::vector<Component*> csgBrushes;

		for (auto it = components.begin(); it != components.end(); ++it)
		{
			Transform* t = (Transform*)*it;
			t->setLocalRotation(rotation);

			// Collect every selected subtree.
			{
				t->iterateChildren([&updBatches, &updCsg, &csgBrushes](Transform* ct) -> bool
					{
						if (ct->getGameObject()->getBatchingStatic())
							updBatches = true;

						CSGBrush* csgBrush = (CSGBrush*)ct->getGameObject()->getComponent(CSGBrush::COMPONENT_TYPE);
						if (csgBrush != nullptr)
						{
							csgBrushes.push_back(csgBrush);

							csgBrush->rebuild();
							updCsg = true;
						}

						return true;
					}
				);
			}
		}

		MainWindow::getSceneWindow()->getGizmo()->updatePosition();

		if (updBatches)
			MainWindow::addOnEndUpdateCallback([=]() { BatchedGeometry::getSingleton()->rebuild(); });

		if (updCsg)
			MainWindow::addOnEndUpdateCallback([csgBrushes]() { CSGGeometry::getSingleton()->rebuild(csgBrushes); });
	}

	void TransformEditor::onChangeScale(Property* prop, glm::vec3 val)
	{
		//Undo
		UndoData* undoData = Undo::addUndo("Change object scale");
		undoData->vec3Data.resize(2);

		for (auto& comp : components)
		{
			undoData->vec3Data[0][comp] = ((Transform*)comp)->getLocalScale();
			undoData->vec3Data[1][comp] = val;
		}

		undoData->undoAction = [=](UndoData* data)
		{
			bool updBatches = false;
			bool updCsg = false;

			std::vector<Component*> comps;

			for (auto& d : data->vec3Data[0])
			{
				Transform* t = (Transform*)d.first;
				t->setLocalScale(d.second);

				// Collect every selected subtree.
				{
					t->iterateChildren([&updBatches, &updCsg, &comps](Transform* ct) -> bool
						{
							if (ct->getGameObject()->getBatchingStatic())
								updBatches = true;

							CSGBrush* csgBrush = (CSGBrush*)ct->getGameObject()->getComponent(CSGBrush::COMPONENT_TYPE);
							if (csgBrush != nullptr)
							{
								comps.push_back(csgBrush);

								csgBrush->rebuild();
								updCsg = true;
							}

							return true;
						}
					);
				}
			}

			MainWindow::getInspectorWindow()->updateObjectEditorTransform();
			MainWindow::getSceneWindow()->getGizmo()->updatePosition();

			if (updBatches)
				MainWindow::addOnEndUpdateCallback([=]() { BatchedGeometry::getSingleton()->rebuild(); });

			if (updCsg)
				MainWindow::addOnEndUpdateCallback([=]() { CSGGeometry::getSingleton()->rebuild(comps); });
		};

		undoData->redoAction = [=](UndoData* data)
		{
			bool updBatches = false;
			bool updCsg = false;

			std::vector<Component*> comps;

			for (auto& d : data->vec3Data[1])
			{
				Transform* t = (Transform*)d.first;
				t->setLocalScale(d.second);

				// Collect every selected subtree.
				{
					t->iterateChildren([&updBatches, &updCsg, &comps](Transform* ct) -> bool
						{
							if (ct->getGameObject()->getBatchingStatic())
								updBatches = true;

							CSGBrush* csgBrush = (CSGBrush*)ct->getGameObject()->getComponent(CSGBrush::COMPONENT_TYPE);
							if (csgBrush != nullptr)
							{
								comps.push_back(csgBrush);

								csgBrush->rebuild();
								updCsg = true;
							}

							return true;
						}
					);
				}
			}

			MainWindow::getInspectorWindow()->updateObjectEditorTransform();
			MainWindow::getSceneWindow()->getGizmo()->updatePosition();

			if (updBatches)
				MainWindow::addOnEndUpdateCallback([=]() { BatchedGeometry::getSingleton()->rebuild(); });

			if (updCsg)
				MainWindow::addOnEndUpdateCallback([=]() { CSGGeometry::getSingleton()->rebuild(comps); });
		};
		//

		bool updBatches = false;
		bool updCsg = false;
		std::vector<Component*> csgBrushes;

		for (auto it = components.begin(); it != components.end(); ++it)
		{
			Transform* t = (Transform*)*it;
			t->setLocalScale(val);

			// Collect every selected subtree.
			{
				t->iterateChildren([&updBatches, &updCsg, &csgBrushes](Transform* ct) -> bool
					{
						if (ct->getGameObject()->getBatchingStatic())
							updBatches = true;

						CSGBrush* csgBrush = (CSGBrush*)ct->getGameObject()->getComponent(CSGBrush::COMPONENT_TYPE);
						if (csgBrush != nullptr)
						{
							csgBrushes.push_back(csgBrush);

							csgBrush->rebuild();
							updCsg = true;
						}

						return true;
					}
				);
			}
		}

		MainWindow::getSceneWindow()->getGizmo()->updatePosition();

		if (updBatches)
			MainWindow::addOnEndUpdateCallback([=]() { BatchedGeometry::getSingleton()->rebuild(); });

		if (updCsg)
			MainWindow::addOnEndUpdateCallback([csgBrushes]() { CSGGeometry::getSingleton()->rebuild(csgBrushes); });
	}

	void TransformEditor::updateTransform()
	{
		Transform* currentNode = (Transform*)components[0];
		position->setValue(currentNode->getLocalPosition());
		rotation->setValue(cleanEulerForEditor(Mathf::toEuler(currentNode->getLocalRotation())));
		scale->setValue(currentNode->getLocalScale());
	}
}
