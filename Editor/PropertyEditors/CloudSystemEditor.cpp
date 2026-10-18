#include "CloudSystemEditor.h"
#include "../../Engine/Components/CloudSystem.h"
#include "PropFloat.h"
#include "PropVector3.h"
#include "../Classes/Undo.h"
#include "../Windows/MainWindow.h"
#include "../Windows/InspectorWindow.h"

namespace GX
{
    void CloudSystemEditor::init(std::vector<Component*> comps)
    {
        ComponentEditor::init(comps);
        CloudSystem* cloud = static_cast<CloudSystem*>(comps[0]);
        auto addParameter = [this, cloud](const char* label, float CloudSystem::* member, float minimum, float maximum)
        {
            PropFloat* prop = new PropFloat(this, label, cloud->*member);
            prop->setMinValue(minimum);
            prop->setMaxValue(maximum);
            prop->setOnChangeCallback([this, member, label, minimum, maximum](Property*, float value)
                { setParameter(member, glm::clamp(value, minimum, maximum), label); });
            addProperty(prop);
        };
        addParameter("Coverage", &CloudSystem::coverage, 0.0f, 1.0f);
        addParameter("Density", &CloudSystem::density, 0.0f, 1.0f);
        addParameter("Layer height", &CloudSystem::baseHeight, -10000.0f, 10000.0f);
        addParameter("Layer thickness", &CloudSystem::thickness, 1.0f, 10000.0f);
        addParameter("Spatial scale", &CloudSystem::spatialScale, 0.00001f, 1.0f);
        addParameter("Animation speed", &CloudSystem::animationSpeed, 0.0f, 1000.0f);
        PropVector3* wind = new PropVector3(this, "Wind", cloud->wind);
        wind->setOnChangeCallback([this](Property*, glm::vec3 value)
        {
            UndoData* undo = Undo::addUndo("Change clouds Wind");
            undo->vec3Data.resize(2);
            undo->undoAction = [](UndoData* data)
            {
                for (const auto& entry : data->vec3Data[0])
                    static_cast<CloudSystem*>(entry.first)->wind = entry.second;
                MainWindow::getInspectorWindow()->updateCurrentEditor();
            };
            undo->redoAction = [](UndoData* data)
            {
                for (const auto& entry : data->vec3Data[1])
                    static_cast<CloudSystem*>(entry.first)->wind = entry.second;
                MainWindow::getInspectorWindow()->updateCurrentEditor();
            };
            for (Component* comp : components)
            {
                CloudSystem* target = static_cast<CloudSystem*>(comp);
                undo->vec3Data[0][target] = target->wind;
                undo->vec3Data[1][target] = value;
                target->wind = value;
            }
        });
        addProperty(wind);
    }

    void CloudSystemEditor::setParameter(float CloudSystem::* member, float value, const char* label)
    {
        UndoData* undo = Undo::addUndo(std::string("Change clouds ") + label);
        undo->floatData.resize(2);
        undo->undoAction = [member](UndoData* data)
        {
            for (auto& entry : data->floatData[0])
                static_cast<CloudSystem*>(entry.first)->*member = entry.second;
            MainWindow::getInspectorWindow()->updateCurrentEditor();
        };
        undo->redoAction = [member](UndoData* data)
        {
            for (auto& entry : data->floatData[1])
                static_cast<CloudSystem*>(entry.first)->*member = entry.second;
            MainWindow::getInspectorWindow()->updateCurrentEditor();
        };
        for (Component* comp : components)
        {
            CloudSystem* cloud = static_cast<CloudSystem*>(comp);
            undo->floatData[0][cloud] = cloud->*member;
            undo->floatData[1][cloud] = value;
            cloud->*member = value;
        }
    }
}
