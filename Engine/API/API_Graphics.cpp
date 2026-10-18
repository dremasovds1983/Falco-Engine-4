#include "API_Graphics.h"

#include "../Renderer/Renderer.h"
#include "../Core/APIManager.h"
#include "../Core/Debug.h"
#include <bx/math.h>
#include "../Renderer/Primitives.h"
#include "../Assets/Material.h"
#include "../Assets/Shader.h"
#include "../Renderer/RenderTexture.h"
#include "../Components/Camera.h"

#include "../glm/glm.hpp"
#include "../glm/gtc/type_ptr.hpp"

namespace GX
{
    bool API_Graphics::captureScreenshot(MonoString* path)
    {
#ifdef FALCO_MODERN_BGFX
        if (path == nullptr) return false;
        char* utf8 = mono_string_to_utf8(path);
        if (utf8 == nullptr) return false;
        // bgfx copies the path into its command buffer. Invalid handle selects
        // the main native window backbuffer, including the final UI composite.
        bgfx::requestScreenShot(BGFX_INVALID_HANDLE, utf8);
        mono_free(utf8);
        return true;
#else
        return false;
#endif
    }

    void API_Graphics::blitRt(MonoObject* material, MonoObject* renderTarget, bool copyToBackBuffer)
    {
        Camera* camera = Camera::getApiCamera();
        if (camera == nullptr)
        {
            Debug::logError("Graphics.Blit requires an active OnRenderImage callback");
            return;
        }
        if (material == nullptr || renderTarget == nullptr) return;
        RenderTexture* rt = nullptr;
        mono_field_get_value(renderTarget, APIManager::getSingleton()->object_ptr_field, reinterpret_cast<void*>(&rt));
        Material* mat = nullptr;
        mono_field_get_value(material, APIManager::getSingleton()->asset_ptr_field, reinterpret_cast<void*>(&mat));
        if (rt == nullptr || !bgfx::isValid(rt->getFrameBufferHandle()) || mat == nullptr || !mat->isLoaded()) return;
        Shader* shader = mat->getShader();
        if (shader == nullptr || !shader->isLoaded()) return;
        int viewId = Camera::acquireApiViewId();
        if (viewId < 0)
        {
            Debug::logError("Graphics.Blit exceeded the 16 calls per camera frame limit");
            return;
        }
        const int width = rt->getWidth(), height = rt->getHeight();
        glm::mat4 projection;
#ifdef FALCO_MODERN_BGFX
        float ortho[16];
        bx::mtxOrtho(ortho, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 100.0f, 0.0f,
            bgfx::getCaps()->homogeneousDepth, bx::Handedness::Right);
        projection = glm::make_mat4(ortho);
#else
        projection = glm::orthoRH(0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 100.0f);
#endif
        bgfx::setViewClear(viewId, BGFX_CLEAR_COLOR | (bgfx::isValid(rt->getDepthTextureHandle()) ? BGFX_CLEAR_DEPTH : 0), 0, 1.0f, 0);
        bgfx::setViewRect(viewId, 0, 0, width, height);
        bgfx::setViewTransform(viewId, nullptr, glm::value_ptr(projection));
        bgfx::setViewFrameBuffer(viewId, rt->getFrameBufferHandle());
        bgfx::setViewMode(viewId, bgfx::ViewMode::Sequential);
        const glm::mat4 identity(1.0f);
        bool submitted = false;
        for (int j = 0; j < shader->getPassCount(); ++j)
        {
            Pass* pass = shader->getPass(j);
            ProgramVariant* pv = pass != nullptr ? pass->getProgramVariant(mat->getDefinesStringHash()) : nullptr;
            if (pv == nullptr || !bgfx::isValid(pv->programHandle)) continue;
            Renderer::getSingleton()->setSystemUniforms(camera);
            mat->submitUniforms(pv, camera);
            bgfx::setTransform(glm::value_ptr(identity));
            // A post-effect writes its complete result, preserving RGB and alpha.
            // Alpha blending against the cleared target would multiply RGB again.
            bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A);
            if (!Primitives::screenSpaceQuad()) { bgfx::discard(); break; }
            bgfx::submit(viewId, pv->programHandle);
            submitted = true;
        }
        // Keep all camera producers/consumers in the same bgfx frame. Each Blit
        // owns a view, so later calls cannot retarget previously queued draws.
        if (submitted && copyToBackBuffer) camera->postProcessRt = rt;
    }
}
