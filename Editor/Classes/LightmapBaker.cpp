#include "LightmapBaker.h"
#include "BakeProgress.h"
#include "BakeOutputTransaction.h"
#include "BakeBlur.h"
#include "BakeUV.h"
#include "BakeOcclusion.h"
#include "../Engine/Classes/md5.h"
#include "../Engine/Components/Terrain.h"
#include <memory>
#include <stdexcept>

#include <algorithm>

#include "../Engine/Core/Engine.h"
#include "../Engine/Core/GameObject.h"
#include "../Engine/Math/Mathf.h"
#include "../Engine/Renderer/Color.h"
#include "../Engine/Renderer/Renderer.h"
#include "../Engine/Math/Raycast.h"
#include "../Engine/Classes/IO.h"

#include "../Engine/Components/Transform.h"
#include "../Engine/Components/MeshRenderer.h"
#include "../Engine/Components/Light.h"

#include "../Engine/Assets/Mesh.h"
#include "../Engine/Assets/Scene.h"
#include "../Engine/Assets/Material.h"
#include "../Engine/Assets/Texture.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <process.h>
#endif

#define FREEIMAGE_LIB
#include <FreeImage.h>
#undef FREEIMAGE_LIB

namespace GX
{
	static thread_local BakeProgress* bakeMonitor = nullptr;
    static thread_local BakeOcclusion* bakeOcclusion = nullptr;
    static BakeOcclusion::Point shadowPoint(const glm::vec3& p) { return {p.x,p.y,p.z}; }
    static bool bakeShadow(const BakeOcclusion::SurfacePoint& receiver, const glm::vec3& vector, bool directional)
    {
        if (!bakeOcclusion) throw std::runtime_error("Bake shadow acceleration structure is missing");
        const bool hit = directional
            ? bakeOcclusion->occludedFromSurface(receiver, shadowPoint(vector), std::numeric_limits<double>::infinity())
            : bakeOcclusion->occludedToLight(receiver, shadowPoint(vector));
        if (bakeMonitor) bakeMonitor->ray();
        return hit;
    }
	std::vector<std::pair<int, int>> LightmapBaker::m_SearchPattern;
	std::vector<LightmapID*> LightmapBaker::m_lightMaps;
	std::vector<LightmapBaker::TextureCache> LightmapBaker::textureCache;
	int LightmapBaker::lightmapSize = 256;
	bool LightmapBaker::giBake = true;
	int LightmapBaker::giBounces = 1;
	float LightmapBaker::giIntensity = 0.8f;
	GIQuality LightmapBaker::giQuality = GIQuality::Normal;

	LightmapBaker::LightmapBaker()
	{
		
	}

	LightmapBaker::~LightmapBaker()
	{

	}

	struct ArraySliceLightMap
	{
	public:
		ArraySliceLightMap() {}
		~ArraySliceLightMap() {}

		int pBegin = 0;
		int pEnd = 0;
		int texSize = 0;
		LightmapBaker::PixelInfo* lightMapDirect = nullptr;
		LightmapBaker::PixelInfo* lightMapIndirect = nullptr;
		std::vector<unsigned int>* indexBuffer = nullptr;
		std::vector<VertexBuffer>* vertexBuffer = nullptr;
		MeshRenderer* renderer = nullptr;
		BatchedGeometry::Batch* geometryBatch = nullptr;
		CSGGeometry::SubMesh* csgSubMesh = nullptr;
		Material* material = nullptr;
		std::vector<Light*>* lights = nullptr;
	};

	void bakeThread(void* param)
	{
		ArraySliceLightMap* slice = (ArraySliceLightMap*)param;
		if (!slice) return;

		MeshRenderer* renderer = slice->renderer;
		BatchedGeometry::Batch* batch = slice->geometryBatch;
		CSGGeometry::SubMesh* csgSubMesh = slice->csgSubMesh;

		int texSize = slice->texSize;

		std::vector<Light*> lights;

		for (auto& light : *slice->lights)
		{
			if (!light->getEnabled())
				continue;

			if (!light->getGameObject()->getActive())
				continue;

			if (light->getLightRenderMode() == LightRenderMode::Realtime)
				continue;

			if (light->getLightType() != LightType::Directional)
			{
				AxisAlignedBox aab = AxisAlignedBox::BOX_NULL;
				if (renderer != nullptr)
					aab = renderer->getBounds();
				else if (batch != nullptr)
					aab = batch->getBounds();
				else if (csgSubMesh != nullptr)
					aab = csgSubMesh->getBounds();

				GameObject* lightObj = light->getGameObject();
				Transform* lightTrans = lightObj->getTransform();

				if (!Mathf::intersects(lightTrans->getPosition(), light->getRadius(), aab))
					continue;
			}

			lights.push_back(light);
		}

		for (int i = slice->pBegin; i < slice->pEnd; i += 3)
		{
			VertexBuffer& vb0 = (*slice->vertexBuffer)[(*slice->indexBuffer)[i]];
			VertexBuffer& vb1 = (*slice->vertexBuffer)[(*slice->indexBuffer)[i + 1]];
			VertexBuffer& vb2 = (*slice->vertexBuffer)[(*slice->indexBuffer)[i + 2]];

			LightmapBaker::directTriangle(vb0.position, vb1.position, vb2.position,
				vb0.normal, vb1.normal, vb2.normal,
				vb0.texcoord1, vb1.texcoord1, vb2.texcoord1,
				vb0.texcoord0, vb1.texcoord0, vb2.texcoord0,
				texSize, slice->lightMapDirect, slice->lightMapIndirect, lights, slice->material);
            if (bakeMonitor) bakeMonitor->advance();
		}

		lights.clear();

		//ExitThread(0);
	}

	LightmapID* LightmapBaker::getLightmapID(MeshRenderer* renderer, int subMeshIndex)
	{
		auto it = std::find_if(m_lightMaps.begin(), m_lightMaps.end(), [=](LightmapID* lm) -> bool
			{
				return lm->renderer == renderer && lm->subMeshIndex == subMeshIndex;
			}
		);

		if (it != m_lightMaps.end())
			return *it;

		return nullptr;
	}

	LightmapID* LightmapBaker::getLightmapID(BatchedGeometry::Batch* batch)
	{
		auto it = std::find_if(m_lightMaps.begin(), m_lightMaps.end(), [=](LightmapID* lm) -> bool
			{
				return lm->batch == batch;
			}
		);

		if (it != m_lightMaps.end())
			return *it;

		return nullptr;
	}

	LightmapID* LightmapBaker::getLightmapID(CSGGeometry::SubMesh* csgSubMesh)
	{
		auto it = std::find_if(m_lightMaps.begin(), m_lightMaps.end(), [=](LightmapID* lm) -> bool
			{
				return lm->csgSubMesh == csgSubMesh;
			}
		);

		if (it != m_lightMaps.end())
			return *it;

		return nullptr;
	}

	int LightmapBaker::getLightmapSize(int value)
	{
		int texSize = lightmapSize;

		switch (value)
		{
		case 0:
			texSize = lightmapSize;
			break;
		case 1:
			texSize = 64;
			break;
		case 2:
			texSize = 128;
			break;
		case 3:
			texSize = 256;
			break;
		case 4:
			texSize = 512;
			break;
		case 5:
			texSize = 1024;
			break;
		case 6:
			texSize = 2048;
			break;
		case 7:
			texSize = 4096;
			break;
		default:
			texSize = lightmapSize;
			break;
		}

		return texSize;
	}

	void LightmapBaker::bakeAll(BakeProgress& monitor)
	{
        BakeOcclusion occlusion;
        bakeOcclusion = &occlusion;
        bakeMonitor = &monitor;
        // Covers cancellation, allocation failures and output errors as well as success.
        auto cleanup = std::unique_ptr<void, std::function<void(void*)>>((void*)1, [](void*) {
            for (auto& cached : textureCache) if (cached.handle) FreeImage_Unload((FIBITMAP*)cached.handle);
            textureCache.clear();
            for (auto* map : m_lightMaps) { delete[] map->directData; delete[] map->indirectData; delete map; }
            m_lightMaps.clear();
            bakeMonitor = nullptr;
            bakeOcclusion = nullptr;
        });
        const std::string loadedScene = Scene::getLoadedScene();
        if (loadedScene.empty()) throw std::runtime_error("Save the scene before baking lighting");
        const std::string outputPath = Engine::getSingleton()->getAssetsPath() + IO::GetFilePath(loadedScene) + IO::GetFileName(loadedScene) + "/Lightmaps";
        BakeOutputTransaction output(std::filesystem::u8path(outputPath));
        monitor.note("scene\t" + loadedScene + "\nlightmap_default\t" + std::to_string(lightmapSize) +
            "\ngi_enabled\t" + std::to_string(giBake) + "\ngi_bounces\t" + std::to_string(giBounces) +
            "\ngi_quality\t" + std::to_string((int)giQuality) + "\ngi_intensity\t" + std::to_string(giIntensity));
        monitor.note("lightmap_storage\tLossless PNG, uncompressed GPU texture, no atlas mipmaps");
        monitor.stage("Preparing geometry");

		buildSearchPattern();

		auto gameObjects = Engine::getSingleton()->getGameObjects();
        auto& lights = Renderer::getSingleton()->getLights();
        for (auto* light : lights)
            monitor.note("light\t" + light->getGameObject()->getName() + "\ttype=" + std::to_string((int)light->getLightType()) +
                "\tmode=" + std::to_string((int)light->getLightRenderMode()) + "\tshadow_map_size=" + std::to_string(light->getShadowMapSize()) +
                "\tshadow_maps=" + std::to_string(light->getNumShadowMaps()));

		BatchedGeometry::getSingleton()->rebuild(true);
        monitor.checkpoint();

		for (auto& mdl : CSGGeometry::getSingleton()->getModels())
		{
			if (mdl->_needRebuild)
				CSGGeometry::getSingleton()->rebuild(mdl->component, true);
            monitor.checkpoint();
		}

		auto& batches = BatchedGeometry::getSingleton()->getBatches();
		auto& csgModels = CSGGeometry::getSingleton()->getModels();

        monitor.stage("Validating lightmap UVs");
        auto validateUVs = [&](const auto& vertices, const auto& indices, const std::string& name) {
            size_t usable = 0, collapsed = 0;
            if (indices.empty() || indices.size() % 3 != 0)
                throw std::runtime_error("Invalid triangle indices for lightmap: " + name);
            for (size_t i = 0; i < indices.size(); i += 3)
            {
                if (indices[i] >= vertices.size() || indices[i+1] >= vertices.size() || indices[i+2] >= vertices.size())
                    throw std::runtime_error("Lightmap vertex index out of bounds: " + name);
                const auto& a = vertices[indices[i]].texcoord1;
                const auto& b = vertices[indices[i+1]].texcoord1;
                const auto& c = vertices[indices[i+2]].texcoord1;
                if (!bakeUvInRange(a) || !bakeUvInRange(b) || !bakeUvInRange(c))
                    throw std::runtime_error("Lightmap UVs must be finite and within 0..1: " + name);
                if (bakeUvArea(a,b,c) != 0) ++usable; else ++collapsed;
                if ((i % 3072) == 0) monitor.checkpoint();
            }
            monitor.note("uv_check\t" + name + "\tusable=" + std::to_string(usable) + "\tcollapsed=" + std::to_string(collapsed));
            if (!usable)
                throw std::runtime_error("No usable lightmap UVs: " + name + ". Enable Generate lightmap UVs in model import settings and reimport.");
        };
        for (auto* object : gameObjects)
        {
            if (!object->getActive() || !object->getLightingStatic() || object->getBatchingStatic()) continue;
            auto* renderer = (MeshRenderer*)object->getComponent(MeshRenderer::COMPONENT_TYPE);
            if (!renderer || !renderer->getMesh()) continue;
            for (int i = 0; i < renderer->getMesh()->getSubMeshCount(); ++i)
            {
                auto* mesh = renderer->getMesh()->getSubMesh(i);
                validateUVs(mesh->getVertexBuffer(), mesh->getIndexBuffer(), object->getName() + " submesh " + std::to_string(i));
            }
        }
        for (auto* batch : batches)
            if (batch->getLightingStatic()) validateUVs(batch->getVertexBuffer(), batch->getIndexBuffer(), "batch " + batch->getGuid());
        for (auto* model : csgModels)
            for (auto* mesh : model->subMeshes) validateUVs(mesh->getVertexBuffer(), mesh->getIndexBuffer(), "CSG " + mesh->getGuid());

        monitor.stage("Building shadow acceleration");
        auto addGeometry = [&](const auto* vertices, size_t vertexCount, const auto* indices, size_t indexCount, const glm::mat4& transform) {
            if (indexCount % 3 != 0) throw std::runtime_error("Invalid shadow triangle index count");
            for (size_t i=0; i<indexCount; i+=3)
            {
                if (indices[i]>=vertexCount || indices[i+1]>=vertexCount || indices[i+2]>=vertexCount)
                    throw std::runtime_error("Shadow triangle vertex index out of bounds");
                const glm::vec3 a=transform*glm::vec4(vertices[indices[i]].position,1);
                const glm::vec3 b=transform*glm::vec4(vertices[indices[i+1]].position,1);
                const glm::vec3 c=transform*glm::vec4(vertices[indices[i+2]].position,1);
                occlusion.add(shadowPoint(a),shadowPoint(b),shadowPoint(c));
                if ((i%3072)==0) monitor.checkpoint();
            }
        };
        for (auto* object : gameObjects)
        {
            if (!object->getActive() || !object->getLightingStatic() || object->getBatchingStatic()) continue;
            auto* renderer=(MeshRenderer*)object->getComponent(MeshRenderer::COMPONENT_TYPE);
            auto* terrain=(Terrain*)object->getComponent(Terrain::COMPONENT_TYPE);
            const auto transform=object->getTransform()->getTransformMatrix();
            if (renderer && renderer->getEnabled() && renderer->getCastShadows() && renderer->getMesh())
            {
                for (int i=0;i<renderer->getMesh()->getSubMeshCount();++i)
                {
                    auto* mesh=renderer->getMesh()->getSubMesh(i);
                    addGeometry(mesh->getVertexBuffer().data(),mesh->getVertexBuffer().size(),mesh->getIndexBuffer().data(),mesh->getIndexBuffer().size(),transform);
                }
            }
            else if (!renderer && terrain && terrain->getEnabled() && terrain->getCastShadows() && !terrain->getIsDirty())
                addGeometry(terrain->getVertexBuffer(),terrain->getVertexCount(),terrain->getIndexBuffer(),terrain->getIndexCount(),transform);
        }
        for (auto* batch : batches)
            if (batch->getLightingStatic() && batch->getCastShadows())
                addGeometry(batch->getVertexBuffer().data(),batch->getVertexBuffer().size(),batch->getIndexBuffer().data(),batch->getIndexBuffer().size(),glm::mat4(1));
        for (auto* model : csgModels)
            for (auto* mesh : model->subMeshes)
                if (mesh->getCastShadows())
                    addGeometry(mesh->getVertexBuffer().data(),mesh->getVertexBuffer().size(),mesh->getIndexBuffer().data(),mesh->getIndexBuffer().size(),glm::mat4(1));
        occlusion.build([&] { monitor.checkpoint(); });
        monitor.note("shadow_query\tBVH any-hit, two-sided opaque triangles, double surface rays with source-triangle exclusion\nshadow_triangles\t" + std::to_string(occlusion.triangleCount()));

		//Create lightmaps
        monitor.stage("Allocating lightmaps");
		for (auto& gameObject : gameObjects)
		{
			if (!gameObject->getActive())
				continue;

			if (!gameObject->getLightingStatic() || gameObject->getBatchingStatic())
				continue;

			MeshRenderer* renderer = (MeshRenderer*)gameObject->getComponent(MeshRenderer::COMPONENT_TYPE);
			if (renderer == nullptr)
				continue;

			Mesh* mesh = renderer->getMesh();
			if (mesh == nullptr)
				continue;

			int texSize = getLightmapSize(renderer->getLightmapSize());

			for (int i = 0; i < mesh->getSubMeshCount(); ++i)
			{
				SubMesh* subMesh = mesh->getSubMesh(i);

				monitor.checkpoint();
				auto ownedId = std::make_unique<LightmapID>();
				LightmapID* id = ownedId.get();
				m_lightMaps.push_back(id);
				ownedId.release();
				id->directData = new PixelInfo[size_t(texSize) * texSize];
				id->indirectData = new PixelInfo[size_t(texSize) * texSize];
				id->texSize = texSize;
				id->renderer = renderer;
				id->subMeshIndex = i;
				id->guid = gameObject->getGuid();

				monitor.allocation(texSize);
			}
		}

		for (auto& batch : batches)
		{
			if (!batch->getLightingStatic())
				continue;

			int texSize = getLightmapSize(batch->getLightmapSize());

			monitor.checkpoint();
			auto ownedId = std::make_unique<LightmapID>();
			LightmapID* id = ownedId.get();
			m_lightMaps.push_back(id);
			ownedId.release();
			id->directData = new PixelInfo[size_t(texSize) * texSize];
			id->indirectData = new PixelInfo[size_t(texSize) * texSize];
			id->texSize = texSize;
			id->batch = batch;
			id->guid = batch->getGuid();

			monitor.allocation(texSize);
		}

		for (auto& csgModel : csgModels)
		{
			for (auto& csgSubMesh : csgModel->subMeshes)
			{
				int texSize = getLightmapSize(0);

				monitor.checkpoint();
				auto ownedId = std::make_unique<LightmapID>();
				LightmapID* id = ownedId.get();
				m_lightMaps.push_back(id);
				ownedId.release();
				id->directData = new PixelInfo[size_t(texSize) * texSize];
				id->indirectData = new PixelInfo[size_t(texSize) * texSize];
				id->texSize = texSize;
				id->csgSubMesh = csgSubMesh;
				id->guid = csgSubMesh->getGuid();

				monitor.allocation(texSize);
			}
		}

		//Bake direct lighting
        size_t mapNumber = 0;



		for (auto& gameObject : gameObjects)
		{
			if (!gameObject->getActive())
				continue;

			if (!gameObject->getLightingStatic() || gameObject->getBatchingStatic())
				continue;

			MeshRenderer* renderer = (MeshRenderer*)gameObject->getComponent(MeshRenderer::COMPONENT_TYPE);
			if (renderer == nullptr)
				continue;

			Mesh* mesh = renderer->getMesh();
			if (mesh == nullptr)
				continue;

			int texSize = getLightmapSize(renderer->getLightmapSize());

			glm::mat4x4 transform = gameObject->getTransform()->getTransformMatrix();
			glm::highp_quat rotation = gameObject->getTransform()->getRotation();

			for (int i = 0; i < mesh->getSubMeshCount(); ++i)
			{
				SubMesh* subMesh = mesh->getSubMesh(i);

				auto& indices = subMesh->getIndexBuffer();
				auto vertices = subMesh->getVertexBuffer();

				for (auto& v : vertices)
				{
					v.position = transform * glm::vec4(v.position, 1.0);
					v.normal = glm::normalize(rotation * v.normal);
					v.texcoord1 = Mathf::rotateUV(v.texcoord1, Mathf::fDeg2Rad * 90.0f);
					v.texcoord1.y = 1.0f - v.texcoord1.y;
				}

				int arrayLength = indices.size();

				LightmapID* lightmap = getLightmapID(renderer, i);

				auto ownedSlice = std::make_unique<ArraySliceLightMap>();
                ArraySliceLightMap* slice = ownedSlice.get();
				slice->pBegin = 0;
				slice->pEnd = arrayLength;
				slice->texSize = texSize;
				slice->lightMapDirect = lightmap->directData;
				slice->lightMapIndirect = lightmap->indirectData;
				slice->indexBuffer = &indices;
				slice->vertexBuffer = &vertices;
				slice->renderer = renderer;
				slice->geometryBatch = nullptr;
				slice->csgSubMesh = nullptr;
				slice->lights = &lights;
				slice->material = renderer->getMaterial(i);

				monitor.stage("Lighting", "Map " + std::to_string(++mapNumber) + "/" + std::to_string(m_lightMaps.size()) + ": " + gameObject->getName() + " [" + std::to_string(texSize) + "]", arrayLength / 3, "triangles");
                bakeThread((void*)slice);

				vertices.clear();
			}


		}

		for (auto& batch : batches)
		{
			if (!batch->getLightingStatic())
				continue;

			int texSize = getLightmapSize(batch->getLightmapSize());

			auto& indices = batch->getIndexBuffer();
			auto vertices = batch->getVertexBuffer();

			for (auto& v : vertices)
			{
				v.texcoord1 = Mathf::rotateUV(v.texcoord1, Mathf::fDeg2Rad * 90.0f);
				v.texcoord1.y = 1.0f - v.texcoord1.y;
			}

			int arrayLength = indices.size();
			
			LightmapID* lightmap = getLightmapID(batch);

			auto ownedSlice = std::make_unique<ArraySliceLightMap>();
                ArraySliceLightMap* slice = ownedSlice.get();
			slice->pBegin = 0;
			slice->pEnd = arrayLength;
			slice->texSize = texSize;
			slice->lightMapDirect = lightmap->directData;
			slice->lightMapIndirect = lightmap->indirectData;
			slice->indexBuffer = &indices;
			slice->vertexBuffer = &vertices;
			slice->renderer = nullptr;
			slice->geometryBatch = batch;
			slice->lights = &lights;
			slice->material = batch->getMaterial();

			monitor.stage("Lighting", "Map " + std::to_string(++mapNumber) + "/" + std::to_string(m_lightMaps.size()) + ": static batch [" + std::to_string(texSize) + "]", arrayLength / 3, "triangles");
            bakeThread((void*)slice);
			
			vertices.clear();


		}

		for (auto& csgModel : csgModels)
		{
			for (auto& csgSubMesh : csgModel->subMeshes)
			{
				int texSize = getLightmapSize(0);

				auto& indices = csgSubMesh->getIndexBuffer();
				auto vertices = csgSubMesh->getVertexBuffer();

				for (auto& v : vertices)
				{
					v.texcoord1 = Mathf::rotateUV(v.texcoord1, Mathf::fDeg2Rad * 90.0f);
					v.texcoord1.y = 1.0f - v.texcoord1.y;
				}

				int arrayLength = indices.size();

				LightmapID* lightmap = getLightmapID(csgSubMesh);

				auto ownedSlice = std::make_unique<ArraySliceLightMap>();
                ArraySliceLightMap* slice = ownedSlice.get();
				slice->pBegin = 0;
				slice->pEnd = arrayLength;
				slice->texSize = texSize;
				slice->lightMapDirect = lightmap->directData;
				slice->lightMapIndirect = lightmap->indirectData;
				slice->indexBuffer = &indices;
				slice->vertexBuffer = &vertices;
				slice->renderer = nullptr;
				slice->csgSubMesh = csgSubMesh;
				slice->lights = &lights;
				slice->material = csgSubMesh->getMaterial();

				monitor.stage("Lighting", "Map " + std::to_string(++mapNumber) + "/" + std::to_string(m_lightMaps.size()) + ": CSG [" + std::to_string(texSize) + "]", arrayLength / 3, "triangles");
                bakeThread((void*)slice);

				vertices.clear();


			}
		}

		//Unload cache
		for (auto it : textureCache)
			FreeImage_Unload((FIBITMAP*)it.handle);

		textureCache.clear();

		//Save lightmaps
        mapNumber = 0;
		for (auto& lm : m_lightMaps)
		{
			std::string guid_num = "";

			MeshRenderer* renderer = lm->renderer;
			BatchedGeometry::Batch* batch = lm->batch;
			CSGGeometry::SubMesh* csgSubMesh = lm->csgSubMesh;
			if (renderer != nullptr)
			{
				guid_num = "_" + std::to_string(lm->subMeshIndex);
			}

			int texSize = lm->texSize;

			//fillInvalidPixels(texSize, lm->directData);
			//fillInvalidPixels(texSize, lm->indirectData);

			//Create and save texture
			size_t texDataSize = texSize * texSize * 3;
			std::vector<BYTE> pixelStorage(texDataSize);
            BYTE* pixelData = pixelStorage.data();
			int pixCount = texSize * texSize;

			int r = giBake ? texSize / 64 : 0;
            const std::string mapLabel = "Map " + std::to_string(++mapNumber) + "/" + std::to_string(m_lightMaps.size()) + " [" + std::to_string(texSize) + "]";
            monitor.stage("Filtering", mapLabel, uint64_t(r) * texSize * 3, "rows");
			for (int i = 0; i < r; ++i)
			{
				blurLightmap(lm->indirectData, texSize, 2.0f);

				for (int row = 0; row < texSize; ++row)
                {
                    for (int col = 0; col < texSize; ++col)
                    { int pixel = row * texSize + col; lm->indirectData[pixel] *= lm->directData[pixel].a; }
                    monitor.advance();
                }
			}

            monitor.stage("Encoding lightmap", mapLabel, texSize, "rows");
			for (int i = 0; i < pixCount; ++i)
			{
				pixelData[i * 3]	 = Mathf::Clamp01(lm->indirectData[i].b + lm->directData[i].b) * 255;
				pixelData[i * 3 + 1] = Mathf::Clamp01(lm->indirectData[i].g + lm->directData[i].g) * 255;
				pixelData[i * 3 + 2] = Mathf::Clamp01(lm->indirectData[i].r + lm->directData[i].r) * 255;
                if ((i + 1) % texSize == 0) monitor.advance();
			}

            monitor.stage("Writing lightmap", mapLabel);
            std::unique_ptr<FIBITMAP, decltype(&FreeImage_Unload)> image(
                FreeImage_ConvertFromRawBits(pixelData, texSize, texSize, texSize * 3, 24, 0xFF0000, 0x00FF00, 0x0000FF, false), &FreeImage_Unload);
            if (!image) throw std::runtime_error("Cannot create lightmap image");
            const auto fileName = output.directory() / (lm->guid + guid_num + ".lightmap.png");
#ifdef _WIN32
            const bool saved = FreeImage_SaveU(FIF_PNG, image.get(), fileName.c_str(), PNG_DEFAULT) != FALSE;
#else
            const bool saved = FreeImage_Save(FIF_PNG, image.get(), fileName.c_str(), PNG_DEFAULT) != 0;
#endif
            if (!saved) throw std::runtime_error("Cannot save lightmap image");
            monitor.checkpoint(true);
        }

        monitor.beginCommit();
        monitor.stage("Publishing lightmaps");
        output.commit();
        // Invalidate only the published maps' derived cache. A previous bake's
        // cache must not mask the newly written source pixels on reload.
        for (auto* lm : m_lightMaps)
        {
            const std::string suffix = lm->renderer ? "_" + std::to_string(lm->subMeshIndex) : "";
            const std::string name = IO::GetFilePath(loadedScene) + IO::GetFileName(loadedScene) + "/Lightmaps/" + lm->guid + suffix + ".lightmap.png";
            const std::string cache = Engine::getSingleton()->getLibraryPath() + IO::GetFilePath(name) + md5(name) + ".texture";
            if (IO::FileExists(cache)) IO::FileDelete(cache);
        }
        monitor.stage("Reloading lightmaps", {}, m_lightMaps.size(), "maps");
        for (auto* lm : m_lightMaps)
        {
            if (lm->renderer) lm->renderer->reloadLightmaps();
            else if (lm->batch) lm->batch->reloadLightmap();
            else if (lm->csgSubMesh) lm->csgSubMesh->reloadLightmap();
            monitor.advance();
        }

		BatchedGeometry::getSingleton()->reloadLightmaps();
		CSGGeometry::getSingleton()->reloadLightmaps();
	}

	float LightmapBaker::getTriangleArea(const glm::vec3& P1, const glm::vec3& P2, const glm::vec3& P3)
	{
		return 0.5f * glm::length(glm::cross(P2 - P1, P3 - P1));
	}

	void LightmapBaker::buildSearchPattern()
	{
		m_SearchPattern.clear();
		const int iSize = 5;
		int i, j;
		for (i = -iSize; i <= iSize; ++i)
		{
			for (j = -iSize; j <= iSize; ++j)
			{
				if (i == 0 && j == 0)
					continue;
				m_SearchPattern.push_back(std::make_pair(i, j));
			}
		}
		std::sort(m_SearchPattern.begin(), m_SearchPattern.end(), [=](std::pair<int, int>& left, std::pair<int, int>& right) -> bool
			{
				return (left.first * left.first + left.second * left.second) < (right.first * right.first + right.second * right.second);
			}
		);
	}

	int LightmapBaker::getPixelCoordinate(float textureCoord, int texSize)
	{
		int iPixel = textureCoord * (float)texSize;
		if (iPixel < 0)
			iPixel = 0;
		if (iPixel >= texSize)
			iPixel = texSize - 1;

		return iPixel;
	}

	float LightmapBaker::getTextureCoordinate(int iPixelCoord, int texSize)
	{
		return ((float)iPixelCoord + 0.5f) / (float)texSize;
	}

	glm::vec2 LightmapBaker::worldToUV(const glm::vec3& p1, const glm::vec3& p2, const glm::vec3& p3, const glm::vec3& p, const glm::vec2& t1, const glm::vec2& t2, const glm::vec2& t3)
	{
		// calculate vectors from point p to vertices p1, p2 and p3:
		glm::vec3 f1 = p1 - p;
		glm::vec3 f2 = p2 - p;
		glm::vec3 f3 = p3 - p;

		// calculate the areas and factors (order of parameters doesn't matter):
		float a = glm::length(glm::cross(p1 - p2, p1 - p3)); // main triangle area a
		float a1 = glm::length(glm::cross(f2, f3)) / a; // p1's triangle area / a
		float a2 = glm::length(glm::cross(f3, f1)) / a; // p2's triangle area / a 
		float a3 = glm::length(glm::cross(f1, f2)) / a; // p3's triangle area / a

		// find the uv corresponding to point p (uv1/uv2/uv3 are associated to p1/p2/p3):
		glm::vec2 uv = t1 * a1 + t2 * a2 + t3 * a3;

		return uv;
	}

	glm::vec3 LightmapBaker::getBarycentricCoordinates(const glm::vec2& P1, const glm::vec2& P2, const glm::vec2& P3, const glm::vec2& P)
	{
        const auto weights = bakeBarycentric(P1, P2, P3, P);
        return glm::vec3(weights[0], weights[1], weights[2]);
	}

	void LightmapBaker::fillInvalidPixels(int textureSize, PixelInfo* m_LightMap)
	{
		int i, j;
		int x, y;
		std::vector<std::pair<int, int> >::iterator itSearchPattern;
		for (i = 0; i < textureSize; ++i)
		{
			for (j = 0; j < textureSize; ++j)
			{
				if (m_LightMap[i * textureSize + j].a == 0)
				{
					for (itSearchPattern = m_SearchPattern.begin(); itSearchPattern != m_SearchPattern.end(); ++itSearchPattern)
					{
						x = i + itSearchPattern->first;
						y = j + itSearchPattern->second;
						if (x < 0 || x >= textureSize) continue;
						if (y < 0 || y >= textureSize) continue;
						
						if (m_LightMap[x * textureSize + y].a == 1)
						{
							PixelInfo _col = m_LightMap[x * textureSize + y];
							_col.a = 1;

							m_LightMap[i * textureSize + j] = _col;
							break;
						}
					}
				}
			}
		}
	}

	LightmapBaker::PixelInfo LightmapBaker::fetchColor(Texture* tex, int tx, int ty)
	{
		auto it = std::find_if(textureCache.begin(), textureCache.end(), [=](TextureCache& cache) -> bool
			{
				return cache.texture == tex;
			}
		);

		PixelInfo out = PixelInfo(1, 1, 1, 1);
		FIBITMAP* imagen = nullptr;

		if (it != textureCache.end())
		{
			imagen = (FIBITMAP*)it->handle;
		}
		else
		{
			std::string fileName = tex->getOrigin();
			FREE_IMAGE_FORMAT formato = FreeImage_GetFileType(fileName.c_str(), 0);
			imagen = FreeImage_Load(formato, fileName.c_str());
			textureCache.push_back({ tex, imagen });
		}

		if (imagen != nullptr)
		{
			RGBQUAD color;
			FreeImage_GetPixelColor(imagen, tx, ty, &color);

			out = PixelInfo((float)color.rgbRed / 255.0f, (float)color.rgbGreen / 255.0f, (float)color.rgbBlue / 255.0f, 1.0f);
		}

		return out;
	}

	void LightmapBaker::directTriangle(const glm::vec3& P1, const glm::vec3& P2, const glm::vec3& P3,
		const glm::vec3& N1, const glm::vec3& N2, const glm::vec3& N3,
		const glm::vec2& T1, const glm::vec2& T2, const glm::vec2& T3,
		const glm::vec2& T11, const glm::vec2& T22, const glm::vec2& T33,
		int textureSize, PixelInfo* m_LightMapDirect, PixelInfo* m_LightMapIndirect, std::vector<Light*>& lights, Material* material)
	{
        const double uvArea = bakeUvArea(T1, T2, T3);
        if (!std::isfinite(uvArea) || uvArea == 0) return;
		glm::vec2 TMin = T1;
		glm::vec2 TMax = T1;
		
		Mathf::makeFloor(TMin, T2);
		Mathf::makeFloor(TMin, T3);
		Mathf::makeCeil(TMax, T2);
		Mathf::makeCeil(TMax, T3);
		
		int iMinX = getPixelCoordinate(TMin.x, textureSize);
		int iMinY = getPixelCoordinate(TMin.y, textureSize);
		int iMaxX = getPixelCoordinate(TMax.x, textureSize);
		int iMaxY = getPixelCoordinate(TMax.y, textureSize);
		
		glm::vec2 TextureCoord = glm::vec2(0, 0);
		glm::vec3 BarycentricCoords = glm::vec3(0, 0, 0);
		glm::vec3 Pos = glm::vec3(0, 0, 0);
		glm::vec3 Normal = glm::vec3(0, 0, 0);

		Texture* tex = nullptr;
		Color col = Color::White;

		if (giBake)
		{
			if (material != nullptr && material->isLoaded())
			{
				tex = material->getUniform<Sampler2DDef>("albedoMap").second;
				col = material->getUniform<Color>("vColor");

				float emission = material->getUniform<float>("emissionVal") + 1.0f;
				col = Color(col[0] * emission, col[1] * emission, col[2] * emission, 0.0f);
			}
		}

		for (int i = iMinX; i <= iMaxX; ++i)
		{
            if (bakeMonitor) bakeMonitor->checkpoint();
			for (int j = iMinY; j <= iMaxY; ++j)
			{
				TextureCoord.x = getTextureCoordinate(i, textureSize);
				TextureCoord.y = getTextureCoordinate(j, textureSize);
				BarycentricCoords = getBarycentricCoordinates(T1, T2, T3, TextureCoord);


				if (m_LightMapDirect[i * textureSize + j].a == 1.0f || BarycentricCoords.x < 0.0f || BarycentricCoords.y < 0.0f || BarycentricCoords.z < 0.0f)
					continue;

				Pos = BarycentricCoords.x * P1 + BarycentricCoords.y * P2 + BarycentricCoords.z * P3;
				Normal = BarycentricCoords.x * N1 + BarycentricCoords.y * N2 + BarycentricCoords.z * N3;
				Normal = glm::normalize(Normal);

				PixelInfo pixColor = PixelInfo(1, 1, 1, 0);
				if (giBake)
				{
					if (tex != nullptr)
					{
						glm::vec2 tuv = worldToUV(P1, P2, P3, Pos, T11, T22, T33);
						int ii = getPixelCoordinate(tuv.x, tex->getOriginalWidth());
						int jj = getPixelCoordinate(tuv.y, tex->getOriginalHeight());
						pixColor = fetchColor(tex, ii, jj) * PixelInfo(col[0], col[1], col[2], 0.0f);
					}
				}

				const BakeOcclusion::SurfacePoint receiver(shadowPoint(P1), shadowPoint(P2), shadowPoint(P3),
                    BarycentricCoords.y, BarycentricCoords.z);
                PixelInfo c = getDirectIntensity(Pos, Normal, lights, textureSize, material, pixColor, receiver);
				c.a = 1;

				m_LightMapDirect[i * textureSize + j] = c;

				clampColor(m_LightMapDirect[i * textureSize + j]);
                if (bakeMonitor) bakeMonitor->sample();
			}
		}
	}

	LightmapBaker::PixelInfo LightmapBaker::getDirectIntensity(const glm::vec3& Position,
		const glm::vec3& Normal,
		std::vector<Light*>& lights,
		int textureSize,
		Material* material,
		PixelInfo pixelColor, const BakeOcclusion::SurfacePoint& receiver)
	{

		PixelInfo outColor;

		for (auto& light : lights)
		{
			Transform* lightNode = light->getGameObject()->getTransform();

			if (light->getLightType() == LightType::Directional)
			{

				glm::vec3 LightDirection = glm::normalize(lightNode->getForward());

				float _Intensity = -glm::dot(LightDirection, Normal) * light->getIntensity();

				if (_Intensity <= 0.0f)
					continue;

				PixelInfo Intensity = PixelInfo(_Intensity, _Intensity, _Intensity, 0.0f);


				const bool hasHit = light->getCastShadows() && bakeShadow(receiver, -LightDirection, true);

				Color lc = light->getColor();
				PixelInfo _color = PixelInfo(lc.r(), lc.g(), lc.b(), 0.0f) * Intensity;

				if (!hasHit)
				{
					outColor += _color;
					clampColor(outColor);
				}


				//GI
				if (giBake && !hasHit)
				{
					indirectLight(Position, Normal, LightDirection, _color * pixelColor, material);
				}
			}

			if (light->getLightType() == LightType::Point)
			{
				glm::vec3 Origin = lightNode->getPosition();
				float Distance = glm::distance(Origin, Position);

				glm::vec3 lightVector = glm::normalize(Origin - Position);

				float attenuation = Mathf::smoothstep(light->getRadius(), 0.0, Distance) * light->getIntensity();

				float _Intensity = Mathf::clamp(glm::dot(lightVector, Normal) * attenuation, 0.0f, 1.0f);

				if (_Intensity <= 0.0f)
					continue;

				PixelInfo Intensity = PixelInfo(_Intensity, _Intensity, _Intensity, 0.0f);

				const bool hasHit = light->getCastShadows() && bakeShadow(receiver, Origin, false);

				Color lc = light->getColor();
				PixelInfo _color = PixelInfo(lc.r(), lc.g(), lc.b(), 0.0f) * Intensity;

				if (!hasHit)
				{
					outColor += _color;
					clampColor(outColor);
				}


				//GI
				if (giBake && !hasHit)
				{
					indirectLight(Position, Normal, -lightVector, _color * pixelColor, material);
				}
			}

			if (light->getLightType() == LightType::Spot)
			{
				glm::vec3 Origin = lightNode->getPosition();
				float Distance = glm::distance(Origin, Position);

				glm::vec3 lightVector = glm::normalize(Origin - Position);
				glm::vec3 LightDirection = lightNode->getForward();

				float len_sq = glm::dot(lightVector, lightVector);
				float len = sqrt(len_sq);
				float attenuation = Mathf::clamp(1.0f - Distance / (light->getRadius() * 1.2f), 0.0f, len) * light->getIntensity();

				float spotlightAngle = Mathf::clamp(glm::dot(lightVector, -LightDirection), 0.0f, 1.0f);
				float spotParamX = std::cos(Mathf::fDeg2Rad * light->getInnerRadius());
				float spotParamY = std::cos(Mathf::fDeg2Rad * light->getOuterRadius());
				float spotFalloff = Mathf::clamp((spotlightAngle - spotParamX) / (spotParamY - spotParamX), 0.0f, 1.0f);
				float spot = (1.0 - spotFalloff);

				float _Intensity = Mathf::clamp(glm::dot(lightVector, Normal) * attenuation * spot, 0.0f, 1.0f);

				if (_Intensity <= 0.0f)
					continue;

				PixelInfo Intensity = PixelInfo(_Intensity, _Intensity, _Intensity, 0.0f);

				const bool hasHit = light->getCastShadows() && bakeShadow(receiver, Origin, false);

				Color lc = light->getColor();
				PixelInfo _color = PixelInfo(lc.r(), lc.g(), lc.b(), 0.0f) * Intensity;

				if (!hasHit)
				{
					outColor += _color;
					clampColor(outColor);
				}


				//GI
				if (giBake && !hasHit)
				{
					indirectLight(Position, Normal, -lightVector, _color * pixelColor, material);
				}
			}
		}

		PixelInfo finalColor = outColor;

		clampColor(finalColor);

		return finalColor;
	}

	void LightmapBaker::indirectLight(const glm::vec3& Position, const glm::vec3& Normal, const glm::vec3& Direction, const PixelInfo& LightColor, Material* material)
	{
		int quality = static_cast<int>(giQuality) + 1;
		int iterations = quality * 4;

		for (int i = 1; i < iterations; ++i)
		{
			glm::vec3 pos = Position - Normal * 0.001f;
			glm::vec3 nrm = Normal;
			//glm::vec3 lightVector = Direction;
			PixelInfo _color = LightColor;
			clampColor(_color);

			float roughness = 0.2f;

			if (material != nullptr && material->isLoaded())
			{
				if (material != nullptr && material->isLoaded())
				{
					if (material->hasUniform("roughnessVal"))
					{
						roughness = material->getUniform<float>("roughnessVal");
						roughness = Mathf::clamp(roughness, 0.1f, 0.9f);
					}
				}
			}

			for (int b = 0; b < giBounces; ++b)
			{
				//lightVector = glm::normalize(glm::reflect(lightVector, nrm));
				
				glm::vec3 randVec = glm::vec3(Mathf::RandomFloat(-roughness, roughness),
					Mathf::RandomFloat(-roughness, roughness),
					Mathf::RandomFloat(-roughness, roughness)) * 89.0f;

				glm::quat rot = Mathf::toQuaternion(randVec);
				glm::vec3 lightVector = rot * nrm;

				Raycast raycast;
				raycast.checkLightingStatic = true;
				raycast.lightingStatic = true;
				raycast.raycastTransientRenderables = false;
				raycast.checkCastShadows = true;
				raycast.castShadows = true;
				raycast.fetchAdditionalInfo = true;
				raycast.checkBatchedGeometry = true;
				raycast.checkCSGGeometry = true;

				auto rayHit = raycast.execute(Ray(pos, lightVector));
				if (bakeMonitor) bakeMonitor->ray();

				if (rayHit.size() > 0)
				{
					Raycast::HitInfo inf = rayHit[0];
					MeshRenderer* renderer = inf.renderer;
					SubMesh* subMesh = nullptr;
					if (renderer != nullptr)
						subMesh = renderer->getMesh()->getSubMesh(inf.subMeshIndex);

					BatchedGeometry::Batch* batch = inf.batch;
					CSGGeometry::SubMesh* csgSubMesh = inf.csgSubMesh;

					if (subMesh != nullptr || batch != nullptr || csgSubMesh != nullptr)
					{
						pos = inf.hitPoint;
						nrm = inf.hitNormal;

						glm::vec3 P1 = inf.hitTrianglePosition[0];
						glm::vec3 P2 = inf.hitTrianglePosition[1];
						glm::vec3 P3 = inf.hitTrianglePosition[2];

						PixelInfo reflectedColor = PixelInfo(1, 1, 1, 0);
						//float roughness = 0.2f;
						if (inf.material != nullptr && inf.material->isLoaded())
						{
							if (inf.material->hasUniform("roughnessVal"))
							{
								roughness = inf.material->getUniform<float>("roughnessVal");
								roughness = Mathf::clamp(roughness, 0.1f, 0.9f);
							}

							glm::vec2 T1 = inf.hitTriangleTexCoord0[0];
							glm::vec2 T2 = inf.hitTriangleTexCoord0[1];
							glm::vec2 T3 = inf.hitTriangleTexCoord0[2];

							Texture* tex = inf.material->getUniform<Sampler2DDef>("albedoMap").second;
							Color col = inf.material->getUniform<Color>("vColor");

							float emission = inf.material->getUniform<float>("emissionVal") + 1.0f;
							col = Color(col[0] * emission, col[1] * emission, col[2] * emission, 0.0f);

							if (tex != nullptr)
							{
								glm::vec2 tx = worldToUV(P1, P2, P3, pos, T1, T2, T3);
								int i = getPixelCoordinate(tx.x, tex->getOriginalWidth());
								int j = getPixelCoordinate(tx.y, tex->getOriginalHeight());

								reflectedColor = fetchColor(tex, i, j) * PixelInfo(col[0], col[1], col[2], 0.0f);
							}
						}

						glm::vec2 T1 = inf.hitTriangleTexCoord1[0];
						glm::vec2 T2 = inf.hitTriangleTexCoord1[1];
						glm::vec2 T3 = inf.hitTriangleTexCoord1[2];

						T1 = Mathf::rotateUV(T1, Mathf::fDeg2Rad * 90.0f); T1.y = 1.0f - T1.y;
						T2 = Mathf::rotateUV(T2, Mathf::fDeg2Rad * 90.0f); T2.y = 1.0f - T2.y;
						T3 = Mathf::rotateUV(T3, Mathf::fDeg2Rad * 90.0f); T3.y = 1.0f - T3.y;

						glm::vec2 tx = worldToUV(P1, P2, P3, pos, T1, T2, T3);

						LightmapID* lm = nullptr;

						pos -= inf.hitNormal * 0.001f;

						if (subMesh != nullptr)
							lm = getLightmapID(renderer, inf.subMeshIndex);
						else if (batch != nullptr)
							lm = getLightmapID(batch);
						else if (csgSubMesh != nullptr)
							lm = getLightmapID(csgSubMesh);

						if (lm == nullptr) continue;
                        int textureSize = lm->texSize;

						int i = getPixelCoordinate(tx.x, textureSize);
						int j = getPixelCoordinate(tx.y, textureSize);

						if (lm != nullptr)
						{
							PixelInfo finalColor = (_color * giIntensity / (float)quality) * 0.1f / (b + 1);
							finalColor.a = 1.0f;
							clampColor(finalColor);

							int radius = 2;

							for (int ii = -radius; ii <= radius; ++ii)
							{
								for (int jj = -radius; jj <= radius; ++jj)
								{
									float a2 = (float)(ii * ii);
									float b2 = (float)(jj * jj);
									float brushAttn = (radius - std::sqrt(a2 + b2));
									brushAttn = Mathf::Clamp01(brushAttn);

									PixelInfo denomColor = PixelInfo(brushAttn, brushAttn, brushAttn, 1.0f);

									int ix = i + ii;
									int iy = j + jj;

									if (ix < 0) continue;
									if (ix > textureSize - 1) continue;
									if (iy < 0) continue;
									if (iy > textureSize - 1) continue;

									lm->indirectData[ix * textureSize + iy] += (finalColor * denomColor) * (PixelInfo(1, 1, 1, 1) - lm->indirectData[ix * textureSize + iy]);
									clampColor(lm->indirectData[ix * textureSize + iy]);
								}
							}

							_color *= reflectedColor;
						}
					}
				}
				else
				{
					break;
				}
			}
		}
	}

	void LightmapBaker::blurLightmap(PixelInfo* lightmap, int texSize, float radius)
    {
        bakeBlur(lightmap, texSize, radius, Mathf::PI, [] { if (bakeMonitor) bakeMonitor->advance(); });
    }

	void LightmapBaker::clampColor(PixelInfo& pixel)
	{
		pixel.r = Mathf::clamp(pixel.r, 0.0f, 1.0f);
		pixel.g = Mathf::clamp(pixel.g, 0.0f, 1.0f);
		pixel.b = Mathf::clamp(pixel.b, 0.0f, 1.0f);
		pixel.a = Mathf::clamp(pixel.a, 0.0f, 1.0f);
	}
}