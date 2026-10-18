#pragma once

#include "Component.h"
#include "Renderable.h"

#include "../glm/glm.hpp"

namespace GX
{
    class Material;
    class Transform;

    class CloudSystem : public Renderable, public Component
    {
    public:
        CloudSystem();
        virtual ~CloudSystem();

        static std::string COMPONENT_TYPE;
        virtual std::string getComponentType() { return COMPONENT_TYPE; }

        // Renderable overrides
        virtual AxisAlignedBox getBounds(bool world = true) override;
        // Submit the background before ordinary alpha-blended objects.
        virtual bool isTransparent() override { return false; }
        virtual bool isAlwaysVisible() override { return true; }
        virtual bool getSkipRendering() override;
        virtual void onRender(Camera* camera, int view, uint64_t state, bgfx::ProgramHandle program, int renderMode, std::function<void()> preRenderCallback) override;
        virtual bool checkCullingMask(LayerMask& mask) override;

        // Component overrides
        virtual void onAttach() override;
        virtual void onDetach() override;
        virtual Component* onClone() override;
        virtual void onScreenResized(int w, int h) override;

        // Cloud-specific
        Material* getMaterial() { return material; }
        void setMaterial(Material* value);

        // Parameters (examples)
        float coverage = 0.5f;
        float density = 1.0f;
        glm::vec3 wind = glm::vec3(1.0f, 0.0f, 0.0f);
        float baseHeight = 600.0f;
        float thickness = 350.0f;
        float spatialScale = 0.003f;
        float animationSpeed = 6.0f;
        glm::vec4 getLayerParameters() const;

        //  t800 Comments: enable/disable internal Debug::log output
        bool enableLogging = false;

    private:
        AxisAlignedBox bounds = AxisAlignedBox::BOX_NULL;

        Material* material = nullptr;
        bool ownsMaterial = false;
        bgfx::UniformHandle layerUniform = { bgfx::kInvalidHandle };
    };
}
