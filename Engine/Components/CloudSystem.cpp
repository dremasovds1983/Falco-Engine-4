#include "CloudSystem.h"
#include "../Core/GameObject.h"
#include "../Components/Transform.h"
#include "../Components/Camera.h"
#include "../Components/Light.h"
#include "../Renderer/Renderer.h"
#include "../Renderer/Primitives.h"
#include "../Assets/Material.h"
#include "../Assets/Texture.h"
#include "../Assets/Shader.h"
#include "../Core/Engine.h"
#include "../Core/Debug.h"
#include "../Classes/GUIDGenerator.h"
#include "../glm/gtc/matrix_transform.hpp"
#include "../glm/gtc/type_ptr.hpp"

namespace GX
{
    std::string CloudSystem::COMPONENT_TYPE = "CloudSystem";
    CloudSystem::CloudSystem() : Component(nullptr), Renderable()
    {
        castShadows = false;
        layerUniform = bgfx::createUniform("u_cloudLayer", bgfx::UniformType::Vec4);
    }
    CloudSystem::~CloudSystem()
    {
        if (ownsMaterial) delete material;
        if (bgfx::isValid(layerUniform)) bgfx::destroy(layerUniform);
    }
    glm::vec4 CloudSystem::getLayerParameters() const
    {
        return glm::vec4(baseHeight, glm::clamp(thickness, 1.0f, 10000.0f),
            glm::clamp(spatialScale, 0.00001f, 1.0f), glm::clamp(animationSpeed, 0.0f, 1000.0f));
    }

    void CloudSystem::setMaterial(Material* value)
    {
        if (material == value) return;
        if (ownsMaterial) delete material;
        material = value;
        ownsMaterial = false;
    }

    AxisAlignedBox CloudSystem::getBounds(bool world) { return bounds; }
    bool CloudSystem::getSkipRendering()
    {
        return !isAttached() || !getEnabled() || !gameObject || !gameObject->getActive();
    }

    void CloudSystem::onAttach()
    {
        Component::onAttach();
        attach();
        // Preserve cloned/custom materials and materials retained by Undo detach.
        if (material) return;
        const std::string basePath = Engine::getSingleton()->getBuiltinResourcesPath();
        Shader* shader = Shader::load(basePath, "Shaders/StandardClouds.shader");
        if (!shader || !shader->isLoaded())
        {
            Debug::log("CloudSystem: could not load StandardClouds.shader");
            return;
        }
        material = new Material();
        ownsMaterial = true;
        material->setLocation(basePath);
        material->setName(GUIDGenerator::genGuid());
        material->setShader(shader);
        material->load();
    }

    void CloudSystem::onDetach() { detach(); Component::onDetach(); }
    Component* CloudSystem::onClone()
    {
        CloudSystem* c = new CloudSystem();
        c->coverage = coverage;
        c->density = density;
        c->wind = wind;
        c->baseHeight = baseHeight;
        c->thickness = thickness;
        c->spatialScale = spatialScale;
        c->animationSpeed = animationSpeed;
        c->setEnabled(getEnabled());
        c->enableLogging = enableLogging;
        if (material)
        {
            c->material = material->clone(GUIDGenerator::genGuid());
            c->ownsMaterial = true;
        }
        return c;
    }
    void CloudSystem::onScreenResized(int w, int h) {}
    bool CloudSystem::checkCullingMask(LayerMask& mask)
    {
        return !getSkipRendering() && mask.getLayer(gameObject->getLayer());
    }

    void CloudSystem::onRender(Camera* camera, int view, uint64_t state,
        bgfx::ProgramHandle program, int renderMode, std::function<void()> preRenderCallback)
    {
        if (bgfx::isValid(program)) return; // Sky must not enter object-mask/shadow override passes.
        if (getSkipRendering() || !camera || !material || !material->isLoaded()
            || renderMode != static_cast<int>(RenderMode::Forward)) return;
        Shader* shader = material->getShader();
        if (!shader || !shader->isLoaded() || shader->getPassCount() == 0) return;
        Pass* pass = shader->getPass(0);
        ProgramVariant* pv = pass ? pass->getProgramVariant(material->getDefinesStringHash()) : nullptr;
        if (!pv || !bgfx::isValid(pv->programHandle)) return;
        material->setUniform<float>("cloudCoverage", glm::clamp(coverage, 0.0f, 1.0f), false);
        material->setUniform<float>("cloudDensity", glm::clamp(density, 0.0f, 1.0f), false);
        material->setUniform<glm::vec3>("cloudWind", wind, false);
        glm::vec3 sun(0.3f, 0.8f, 0.2f);
        for (Light* light : Renderer::getSingleton()->getLights())
        {
            if (light->getLightType() == LightType::Directional && light->getEnabled()
                && light->getGameObject() && light->getGameObject()->getActive())
            {
                sun = -light->getGameObject()->getTransform()->getForward();
                break;
            }
        }
        material->setUniform<glm::vec3>("cloudSunDirection", sun, false);
        material->submitUniforms(pv, camera);
        if (preRenderCallback) preRenderCallback();
        const glm::vec4 layer = getLayerParameters();
        bgfx::setUniform(layerUniform, glm::value_ptr(layer));
        // Clip-space quad uses this view's inverse VP (Scene, Play or reflection).
        // Keep its framebuffer/viewport/projection: changing a shared view here
        // would also change previously queued geometry in bgfx.
        const glm::mat4 model(1.0f);
        const uint64_t cloudState = BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A
            | BGFX_STATE_BLEND_ALPHA | BGFX_STATE_DEPTH_TEST_LEQUAL;
        bgfx::setTransform(glm::value_ptr(model));
        bgfx::setState(cloudState);
        if (Primitives::screenSpaceQuad())
            bgfx::submit(view, pv->programHandle);
    }
}
