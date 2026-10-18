#include "DecalRenderer.h"
#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <unordered_set>
#include "../Core/Engine.h"
#include "../Core/APIManager.h"

#include "../glm/glm.hpp"
#include "../glm/gtc/type_ptr.hpp"

#include "../Math/Mathf.h"

#include "../Core/GameObject.h"

#include "../Assets/Shader.h"
#include "../Assets/Texture.h"
#include "../Assets/Material.h"

#include "../Components/Transform.h"
#include "../Components/Camera.h"
#include "../Renderer/Renderer.h"
#include "../Renderer/Primitives.h"

namespace GX
{
	std::string DecalRenderer::COMPONENT_TYPE = "DecalRenderer";

	DecalRenderer::DecalRenderer() : Component(nullptr), Renderable()
	{
	}

	DecalRenderer::~DecalRenderer()
	{
	}

    void DecalRenderer::onAttach()
    {
        Component::onAttach();

        transform = getGameObject()->getTransform();

        attach();
    }

    void DecalRenderer::onDetach()
    {
        Component::onDetach();

        detach();
    }

	bool DecalRenderer::isTransparent()
	{
		return false;
	}

	AxisAlignedBox DecalRenderer::getBounds(bool world)
	{
        glm::vec3 scale = glm::vec3(1.0f);

        if (world)
        {
            glm::bvec4 _nan = glm::isnan(transform->rotation);
            if (_nan.x || _nan.y || _nan.z || _nan.w)
                transform->setRotation(glm::identity<glm::highp_quat>());

            glm::mat4x4 mtx = transform->getTransformMatrix();

            _nan = glm::isnan(mtx[3]);
            if (_nan.x || _nan.y || _nan.z || _nan.w)
                mtx[3] = glm::vec4(0.0f);

            if (mtx != prevTransform)
            {
                glm::vec3 _min = -scale;
                glm::vec3 _max = scale;

                cachedAAB = AxisAlignedBox(_min, _max);
                cachedAAB.transform(mtx);

                prevTransform = mtx;

                return cachedAAB;
            }
            else
                return cachedAAB;
        }
        else
        {
            scale = transform->getScale();

            glm::vec3 _min = -scale;
            glm::vec3 _max = scale;

            return AxisAlignedBox(_min, _max);
        }
	}

	Component* DecalRenderer::onClone()
    {
        DecalRenderer* newComponent = new DecalRenderer();
        newComponent->enabled = enabled;
        newComponent->material = material;
        newComponent->runtimeSpawned = Engine::getSingleton()->getIsRuntimeMode();

		return newComponent;
	}

    void DecalRenderer::markRuntimeSpawn(GameObject* root)
    {
        if (!root || !Engine::getSingleton()->getIsRuntimeMode()) return;
        std::vector<Transform*> stack = {root->getTransform()};
        while (!stack.empty())
        {
            auto* current = stack.back(); stack.pop_back();
            for (auto* component : current->getGameObject()->getComponents())
                if (component->getComponentType() == COMPONENT_TYPE)
                {
                    auto* decal = static_cast<DecalRenderer*>(component);
                    decal->runtimeSpawned = true;
                    decal->ageSeconds = 0.0;
                }
            const auto& children = current->getChildren();
            stack.insert(stack.end(), children.begin(), children.end());
        }
    }

    void DecalRenderer::updateTemporaryDecals(float dt)
    {
        if (!Engine::getSingleton()->getIsRuntimeMode()) return;
        auto* renderer = Renderer::getSingleton();
        // Attachment order is creation order. No distance-dependent sorting for FIFO.
        std::unordered_map<Material*, std::vector<DecalRenderer*>> groups;
        std::unordered_set<GameObject*> expired;
        for (auto* renderable : renderer->getRenderables())
        {
            if (!renderable->isDecal()) continue;
            auto* decal = static_cast<DecalRenderer*>(renderable);
            if (!decal->runtimeSpawned || !decal->gameObject) continue;
            decal->ageSeconds += std::isfinite(dt) ? std::max(0.0f, dt) : 0.0f;
            auto* mat = decal->material;
            if (!mat || !mat->isLoaded() || !mat->hasUniform("TEMPORARY_DECAL") ||
                !mat->getUniform<bool>("TEMPORARY_DECAL")) continue;
            float lifetime = mat->getUniform<float>("decalLifetime");
            if (!std::isfinite(lifetime)) lifetime = 60.0f;
            bool visible = false, hasGameCamera = false;
            if (decal->ageSeconds >= std::max(0.0f, lifetime))
            {
                for (auto* camera : renderer->getCameras())
                {
                    if (camera->getIsEditorCamera() || !camera->getAutomaticRendering() ||
                        !camera->getEnabled() || !camera->getGameObject()->getActive()) continue;
                    hasGameCamera = true;
                    if (decal->checkCullingMask(camera->getCullingMask()) && camera->isVisible(decal->getBounds()))
                        visible = true;
                }
                if (hasGameCamera && !visible) expired.insert(decal->gameObject);
            }
            groups[mat].push_back(decal);
        }
        for (auto& group : groups)
        {
            const int limit = std::max(1, std::min(200, group.first->getUniform<int>("decalMaxCount")));
            auto& decals = group.second;
            size_t survivors = 0;
            for (auto* decal : decals) if (!expired.count(decal->gameObject)) ++survivors;
            for (auto* decal : decals)
            {
                if (survivors <= static_cast<size_t>(limit)) break;
                if (expired.insert(decal->gameObject).second) --survivors;
            }
        }
        // Destroy only the highest scheduled ancestor to avoid queuing a child twice.
        for (auto* object : expired)
        {
            bool parentExpired = false;
            for (auto* parent = object->getTransform()->getParent(); parent; parent = parent->getParent())
                if (expired.count(parent->getGameObject())) { parentExpired = true; break; }
            if (!parentExpired) APIManager::getSingleton()->addDestroyObject(object);
        }
    }

	bool DecalRenderer::canRenderDeferred()
	{
		if (gameObject == nullptr || !gameObject->getActive() || !getEnabled())
			return false;

		if (transform == nullptr || material == nullptr || !material->isLoaded())
			return false;

		Shader* shader = material->getShader();
		return shader != nullptr && shader->isLoaded();
	}

	void DecalRenderer::onRender(Camera* camera, int view, uint64_t state, bgfx::ProgramHandle program, int renderMode, std::function<void()> preRenderCallback)
	{
		if (!canRenderDeferred() ||
			program.idx != bgfx::kInvalidHandle ||
			renderMode != static_cast<int>(RenderMode::Deferred))
			return;

        Shader* shader = Renderer::getDefaultShader();

        if (material != nullptr && material->isLoaded() && material->getShader() != nullptr)
            shader = material->getShader();

        if (shader == nullptr || !shader->isLoaded())
            return;

		glm::vec3 scale = transform->getScale();

        glm::mat4x4 m_projector_view = glm::lookAt(transform->getPosition(), transform->getPosition() - transform->getForward(), glm::cross(transform->getRight(), transform->getForward()));
        glm::mat4x4 m_projector_proj = glm::ortho(-scale.x, scale.x, -scale.y, scale.y, 0.1f, scale.z);
        glm::mat4x4 m_projector_view_proj = m_projector_proj * m_projector_view;

		int passCount = 1;

        if (shader != nullptr && shader->isLoaded())
            passCount = shader->getPassCount();

        for (int j = 0; j < passCount; ++j)
        {
            Pass* pass = nullptr;

            if (shader != nullptr && shader->isLoaded())
                pass = shader->getPass(j);

            ProgramVariant* pv = nullptr;
            if (material != nullptr && material->isLoaded() && pass != nullptr)
                pv = pass->getProgramVariant(material->getDefinesStringHash());

            uint64_t passState = state;

            if (pv != nullptr)
                passState = pv->getRenderState(state);

			// Each decal pass rewrites the complete destination G-buffer. Pixels
			// outside the projector are copied through by the decal shader, which
			// lets the camera safely ping-pong overlapping decals in draw order.
			Primitives::screenSpaceQuad();

			// Set render states
			bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A);

            // Pass uniforms
            if (pv != nullptr)
                material->submitUniforms(pv, camera);

            //Bind system uniforms
            bgfx::setUniform(Renderer::getSingleton()->getViewProjUniform(), glm::value_ptr(m_projector_view_proj), 1);
            bgfx::setUniform(Renderer::getSingleton()->getInvViewProjUniform(), glm::value_ptr(glm::inverse(m_projector_view_proj)), 1);

            if (preRenderCallback != nullptr)
                preRenderCallback();

            // Render
            bgfx::ProgramHandle ph = { bgfx::kInvalidHandle };

            if (pv != nullptr)
                ph = pv->programHandle;

            bgfx::submit(view, ph);
        }
    }
}
