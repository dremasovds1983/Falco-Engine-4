#include "Model3DLoader.h"

#include <iostream>
#include <limits>

#include <boost/regex.hpp>
#include <boost/algorithm/string/replace.hpp>

#include <assimp/Importer.hpp>      // C++ importer interface
#include <assimp/Exporter.hpp>      // C++ exporter interface
#include <assimp/scene.h>           // Output data structure
#include <assimp/postprocess.h>     // Post processing flags

#include <glm/gtc/type_ptr.hpp>
#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "../Core/Engine.h"
#include "../Core/Debug.h"
#include "../Classes/IO.h"
#include "../Classes/GUIDGenerator.h"
#include "../Classes/md5.h"
#include "../Core/GameObject.h"
#include "../Assets/Mesh.h"
#include "../Assets/Material.h"
#include "../Assets/Texture.h"
#include "../Assets/Prefab.h"
#include "../Assets/AnimationClip.h"
#include "../Components/Transform.h"
#include "../Components/MeshRenderer.h"
#include "../Components/CSGModel.h"
#include "../Renderer/CSGGeometry.h"

#include "../Classes/xatlas.h"
#include "../Math/Mathf.h"

#include "../Serialization/Meta/SModel3DMeta.h"
#include <FreeImage.h>
#ifdef FALCO_MODERN_BGFX
#include <meshoptimizer.h>
static_assert(MESHOPTIMIZER_VERSION == 250, "Modern LOD requires meshoptimizer 0.25");
#else
#include "../meshoptimizer/src/meshoptimizer.h"
#endif

namespace GX
{
    namespace
    {
        const std::string UNIT_PRIMITIVE_CACHE_DIR = "Primitives/UnitScaleV1/";

        glm::vec3 getUnitPrimitiveSize(const std::string& path)
        {
            const std::string name = IO::GetFileName(path);

            if (name == "Plane") return glm::vec3(10.0f, 0.0f, 10.0f);
            if (name == "Cylinder" || name == "Capsule" || name == "Cone") return glm::vec3(1.0f, 2.0f, 1.0f);
            if (name == "Teapot" || name == "Torus") return glm::vec3(1.0f);
            if (name == "Tube") return glm::vec3(1.0f, 1.0f, 1.0f);

            // Cube, Sphere and Pyramid use a one-metre bounding box.
            return glm::vec3(1.0f);
        }

        std::string getUnitPrimitiveMeshName(const std::string& path)
        {
            return UNIT_PRIMITIVE_CACHE_DIR + md5(path + "_unit_scale_v1") + ".mesh";
        }

        glm::vec3 safeNormalize(const glm::vec3& value)
        {
            const float lengthSquared = glm::dot(value, value);
            return lengthSquared > 0.000000000001f ? value / glm::sqrt(lengthSquared) : glm::vec3(0.0f);
        }

        std::vector<MeshRenderer*> findMeshRenderers(GameObject* root)
        {
            std::vector<MeshRenderer*> renderers;
            auto transforms = Transform::getPlainChildrenList(root->getTransform());
            for (Transform* transform : transforms)
            {
                MeshRenderer* renderer = (MeshRenderer*)transform->getGameObject()->getComponent(MeshRenderer::COMPONENT_TYPE);
                if (renderer != nullptr && renderer->getMesh() != nullptr)
                    renderers.push_back(renderer);
            }

            return renderers;
        }
    }

    Mesh * processMesh(const aiScene* scene, aiNode* mNode, std::string location, std::string path, int index);

    struct ImportData
    {
    public:
        ImportData(const aiScene* _scene, Assimp::Importer* _importer)
        {
            scene = _scene;
            importer = _importer;
        }

        const aiScene* scene = nullptr;
        Assimp::Importer* importer = nullptr;

        void free()
        {
            if (importer != nullptr)
                delete importer;

            importer = nullptr;
        }
    };

    ImportData readSourceFile(std::string location, std::string path)
    {
        std::string fullPath = location + path;

        SModel3DMeta meta;
        std::string metaFilePath = Engine::getSingleton()->getLibraryPath() + path + ".meta";
        if (IO::FileExists(metaFilePath))
            meta.load(metaFilePath);

        // Create an instance of the Importer class
        Assimp::Importer* importer = new Assimp::Importer();
        importer->SetPropertyFloat(AI_CONFIG_GLOBAL_SCALE_FACTOR_KEY, meta.scale);
        importer->SetPropertyFloat(AI_CONFIG_PP_GSN_MAX_SMOOTHING_ANGLE, meta.maxSmoothingAngle);
        importer->SetPropertyBool(AI_CONFIG_IMPORT_FBX_PRESERVE_PIVOTS, false);

        unsigned int importFlags =
            aiProcess_Triangulate |
            aiProcess_SortByPType |
            aiProcess_GlobalScale |
            aiProcess_CalcTangentSpace |
            aiProcess_PopulateArmatureData;

        //if (!meta.generateLightmapUVs)
        importFlags |= aiProcess_JoinIdenticalVertices;

        if (meta.calculateNormals)
            importFlags |= aiProcess_GenSmoothNormals | aiProcess_ForceGenNormals;

        if (meta.optimizeMeshes)
            importFlags |= aiProcess_OptimizeMeshes;

        //Import
        const aiScene* scene = importer->ReadFile(fullPath.c_str(), importFlags);

        // If the import failed, report it
        if (!scene)
        {
            Debug::logError("[" + fullPath + "] Import error: " + importer->GetErrorString());
            return ImportData(nullptr, importer);
        }

        return ImportData(scene, importer);
    }

    GameObject* Model3DLoader::load3DModel(std::string location, std::string path)
    {
        std::string fullPath = location + path;

        if (!IO::FileExists(fullPath))
        {
            Debug::logError("[" + fullPath + "] Import error: File does not exists");
            return nullptr;
        }

        ImportData data = readSourceFile(location, path);
        const aiScene* scene = data.scene;

        if (scene == nullptr)
        {
            data.free();
            return nullptr;
        }

        std::string assetName = IO::GetFileName(path);
        GameObject* root = Engine::getSingleton()->createGameObject();
        root->setName(assetName);

        std::vector<MeshRenderer*> meshRenderers;

        // Now we can access the file's contents. 
        aiNode* rootNode = scene->mRootNode;

        std::vector<std::pair<aiNode*, GameObject*>> nstack;
        for (int i = 0; i < rootNode->mNumChildren; ++i)
            nstack.push_back(std::make_pair(rootNode->mChildren[i], root));

        int idx = 0;
        while (nstack.size() > 0)
        {
            aiNode* child = nstack.begin()->first;
            GameObject* rootObj = nstack.begin()->second;
            nstack.erase(nstack.begin());

            std::string nodeName = child->mName.C_Str();
            boost::regex xRegEx("(_\\$Assimp(\\w+)\\$_)");
            nodeName = boost::regex_replace(nodeName, xRegEx, "_");

            GameObject* childObj = Engine::getSingleton()->createGameObject();
            childObj->setName(nodeName);
            childObj->getTransform()->setParent(rootObj->getTransform());

            aiMatrix4x4 trans = child->mTransformation;
            aiVector3D pos = { 0, 0, 0 }; aiVector3D scl = { 1, 1, 1 }; aiQuaternion rot = { 0, 0, 0, 1 };
            trans.Decompose(scl, rot, pos);
            childObj->getTransform()->setLocalPosition(glm::vec3(pos.x, pos.y, pos.z));
            childObj->getTransform()->setLocalScale(glm::vec3(scl.x, scl.y, scl.z));
            childObj->getTransform()->setLocalRotation(glm::highp_quat(rot.w, rot.x, rot.y, rot.z));

            //Load model
            Mesh * mesh = processMesh(scene, child, location, path, idx);
            if (mesh != nullptr)
            {
                std::vector<Material*> materials;

                MeshRenderer* meshRenderer = new MeshRenderer();
                meshRenderer->setRootObject(root);
                meshRenderer->setMesh(mesh);
                
                childObj->addComponent(meshRenderer);

                meshRenderers.push_back(meshRenderer);
            }

            ++idx;

            for (int j = 0; j < child->mNumChildren; ++j)
            {
                aiNode* ch = child->mChildren[j];
                nstack.insert(nstack.begin() + j, std::make_pair(ch, childObj));
            }
        }

        for (auto it = meshRenderers.begin(); it != meshRenderers.end(); ++it)
        {
            (*it)->resetBoneLinks();
        }

        meshRenderers.clear();
        data.free();

        return root;
    }

    Mesh* Model3DLoader::loadBuiltinPrimitiveMesh(std::string location, std::string path)
    {
        const std::string meshLocation = Engine::getSingleton()->getLibraryPath();
        const std::string meshName = getUnitPrimitiveMeshName(path);

        Mesh* cachedMesh = (Mesh*)Asset::getLoadedInstance(meshLocation, meshName);
        if (cachedMesh != nullptr && cachedMesh->isLoaded())
        {
            cachedMesh->setUserData(location);
            return cachedMesh;
        }

        if (IO::FileExists(meshLocation + meshName))
        {
            cachedMesh = Mesh::load(meshLocation, meshName);
            if (cachedMesh != nullptr)
                cachedMesh->setUserData(location);
            return cachedMesh;
        }

        GameObject* importedRoot = load3DModel(location, path);
        if (importedRoot == nullptr)
            return nullptr;

        std::vector<MeshRenderer*> sourceRenderers = findMeshRenderers(importedRoot);
        if (sourceRenderers.empty())
        {
            Engine::getSingleton()->destroyGameObject(importedRoot);
            Debug::logError("[" + path + "] Built-in primitive has no mesh renderer");
            return nullptr;
        }

        glm::vec3 boundsMin(std::numeric_limits<float>::max());
        glm::vec3 boundsMax(std::numeric_limits<float>::lowest());
        for (MeshRenderer* sourceRenderer : sourceRenderers)
        {
            Mesh* sourceMesh = sourceRenderer->getMesh();
            const glm::mat4 sourceTransform = sourceRenderer->getGameObject()->getTransform()->getTransformMatrix();
            for (int i = 0; i < sourceMesh->getSubMeshCount(); ++i)
            {
                for (const VertexBuffer& vertex : sourceMesh->getSubMesh(i)->getVertexBuffer())
                {
                    glm::vec3 position = glm::vec3(sourceTransform * glm::vec4(vertex.position, 1.0f));
                    boundsMin = glm::min(boundsMin, position);
                    boundsMax = glm::max(boundsMax, position);
                }
            }
        }

        const glm::vec3 sourceSize = boundsMax - boundsMin;
        const glm::vec3 targetSize = getUnitPrimitiveSize(path);
        glm::vec3 scale(1.0f);
        for (int axis = 0; axis < 3; ++axis)
        {
            if (sourceSize[axis] > 0.000001f && targetSize[axis] > 0.0f)
                scale[axis] = targetSize[axis] / sourceSize[axis];
        }

        // Teapot and Torus retain their authored proportions and fit inside a
        // one-metre box. Other primitives have explicit metric dimensions.
        const std::string primitiveName = IO::GetFileName(path);
        if (primitiveName == "Teapot" || primitiveName == "Torus")
        {
            const float maxSourceExtent = glm::max(sourceSize.x, glm::max(sourceSize.y, sourceSize.z));
            scale = glm::vec3(maxSourceExtent > 0.000001f ? 1.0f / maxSourceExtent : 1.0f);
        }

        const glm::vec3 center = (boundsMin + boundsMax) * 0.5f;
        const glm::mat4 normalizationTransform = glm::scale(glm::mat4(1.0f), scale) *
            glm::translate(glm::mat4(1.0f), -center);

        if (!IO::DirExists(meshLocation + UNIT_PRIMITIVE_CACHE_DIR))
            IO::CreateDir(meshLocation + UNIT_PRIMITIVE_CACHE_DIR, true);

        Mesh* mesh = Mesh::create(meshLocation, meshName);
        mesh->setSourceFile(path);
        mesh->setUserData(location);

        for (MeshRenderer* sourceRenderer : sourceRenderers)
        {
            Mesh* sourceMesh = sourceRenderer->getMesh();
            const glm::mat4 bakeTransform = normalizationTransform *
                sourceRenderer->getGameObject()->getTransform()->getTransformMatrix();
            const glm::mat3 linearTransform(bakeTransform);
            const glm::mat3 normalTransform = glm::inverseTranspose(linearTransform);

            for (int i = 0; i < sourceMesh->getSubMeshCount(); ++i)
            {
                SubMesh* sourceSubMesh = sourceMesh->getSubMesh(i);
                SubMesh* subMesh = new SubMesh();
                mesh->addSubMesh(subMesh);

                subMesh->getIndexBuffer() = sourceSubMesh->getIndexBuffer();
                subMesh->setLodLevelsCount(sourceSubMesh->getLodLevelsCount());
                for (int lod = 0; lod < sourceSubMesh->getLodLevelsCount(); ++lod)
                    subMesh->getLodIndexBuffer(lod) = sourceSubMesh->getLodIndexBuffer(lod);

                subMesh->setMaterialName(sourceSubMesh->getMaterialName());
                subMesh->setUseLightmapUVs(sourceSubMesh->getUseLightmapUVs());

                std::vector<VertexBuffer>& vertices = subMesh->getVertexBuffer();
                vertices = sourceSubMesh->getVertexBuffer();
                for (VertexBuffer& vertex : vertices)
                {
                    vertex.position = glm::vec3(bakeTransform * glm::vec4(vertex.position, 1.0f));
                    vertex.normal = safeNormalize(normalTransform * vertex.normal);

                    glm::vec3 tangent = safeNormalize(linearTransform * vertex.tangent);
                    tangent = safeNormalize(tangent - vertex.normal * glm::dot(vertex.normal, tangent));
                    glm::vec3 sourceBitangent = safeNormalize(linearTransform * vertex.bitangent);
                    float handedness = glm::dot(glm::cross(vertex.normal, tangent), sourceBitangent) < 0.0f ? -1.0f : 1.0f;
                    vertex.tangent = tangent;
                    vertex.bitangent = safeNormalize(glm::cross(vertex.normal, tangent)) * handedness;
                }
            }
        }

        mesh->load();
        mesh->save();
        Engine::getSingleton()->destroyGameObject(importedRoot);

        return mesh;
    }

    GameObject* Model3DLoader::loadBuiltinPrimitive(std::string location, std::string path)
    {
        Mesh* mesh = loadBuiltinPrimitiveMesh(location, path);
        if (mesh == nullptr)
            return nullptr;

        GameObject* object = Engine::getSingleton()->createGameObject();
        object->setName(IO::GetFileName(path));

        MeshRenderer* renderer = new MeshRenderer();
        renderer->setRootObject(object);
        renderer->setMesh(mesh);
        object->addComponent(renderer);

        return object;
    }

    glm::mat4x4 getWorldMatrix(aiNode* node)
    {
        glm::mat4x4 mtx = glm::identity<glm::mat4x4>();

        aiNode* parent = node;
        while (parent != nullptr)
        {
            aiMatrix4x4 trans = parent->mTransformation;
            aiVector3D pos = { 0, 0, 0 }; aiVector3D scl = { 1, 1, 1 }; aiQuaternion rot = { 0, 0, 0, 1 };
            trans.Decompose(scl, rot, pos);

            glm::mat4x4 rotMat = glm::mat4_cast(glm::highp_quat(rot.w, rot.x, rot.y, rot.z));
            glm::vec3 _pos = glm::inverse(rotMat) * (glm::vec4(glm::vec3(pos.x, pos.y, pos.z), 1.0f));

            glm::mat4x4 local = glm::identity<glm::mat4x4>();
            local = glm::translate(local, _pos);
            local = rotMat * local;
            local = glm::scale(local, glm::vec3(scl.x, scl.y, scl.z));

            mtx = local * mtx;

            parent = parent->mParent;
        }

        return mtx;
    }

    std::vector<Model3DLoader::ModelMeshData> Model3DLoader::load3DModelMeshes(std::string location, std::string path)
    {
        std::vector<ModelMeshData> meshList;

        std::string fullPath = location + path;

        if (!IO::FileExists(fullPath))
        {
            Debug::logError("[" + fullPath + "] Import error: File does not exists");
            return {};
        }

        ImportData data = readSourceFile(location, path);
        const aiScene* scene = data.scene;

        if (scene == nullptr)
        {
            data.free();
            return {};
        }

        // Now we can access the file's contents. 
        aiNode* rootNode = scene->mRootNode;

        std::vector<aiNode*> nstack;
        for (int i = 0; i < rootNode->mNumChildren; ++i)
            nstack.push_back(rootNode->mChildren[i]);

        int idx = 0;
        while (nstack.size() > 0)
        {
            aiNode* child = *nstack.begin();
            nstack.erase(nstack.begin());

            //Load model
            Mesh* mesh = processMesh(scene, child, location, path, idx);
            if (mesh != nullptr)
            {
                ModelMeshData data;

                data.transform = getWorldMatrix(child);
                data.mesh = mesh;

                //Apply materials
                for (int i = 0; i < mesh->getSubMeshCount(); ++i)
                {
                    std::string matName = mesh->getSubMesh(i)->getMaterialName();
                    Material* material = Material::load(location, matName);
                    if (material == nullptr)
                    {
                        material = Material::create(location, matName);
                        material->load();
                        material->save();
                    }

                    data.materials.push_back(material);
                }
                //

                meshList.push_back(data);
            }

            ++idx;

            for (int j = 0; j < child->mNumChildren; ++j)
            {
                aiNode* ch = child->mChildren[j];
                nstack.insert(nstack.begin() + j, ch);
            }
        }

        data.free();

        return meshList;
    }

    std::vector<Model3DLoader::ModelMeshData> Model3DLoader::load3DModelMeshesFromPrefab(std::string location, std::string name)
    {
        std::vector<Model3DLoader::ModelMeshData> outList;

        Prefab* prefab = Prefab::load(location, name);

        if (prefab != nullptr && prefab->isLoaded())
        {
            SScene& scene = prefab->getScene();

            std::string libraryPath = Engine::getSingleton()->getLibraryPath();

            glm::vec3 rootPosition(0.0f);
            if (scene.gameObjects.size() > 0)
            {
                SGameObject& sRootObj = scene.gameObjects[0];
                rootPosition = sRootObj.position.getValue();
            }

            for (auto it = scene.gameObjects.begin(); it != scene.gameObjects.end(); ++it)
            {
                SGameObject& sObj = *it;
                glm::mat4x4 transform = Transform::makeTransformMatrix(sObj.position.getValue(), sObj.rotation.getValue(), sObj.scale.getValue());
                // Vegetation placement replaces the prefab's scene location, not
                // its authored axis/unit correction. Removing the entire root
                // matrix made rotated/scaled imported trees tiny and sideways.
                transform[3] -= glm::vec4(rootPosition, 0.0f);

                for (auto m = sObj.meshRenderers.begin(); m != sObj.meshRenderers.end(); ++m)
                {
                    SMeshRenderer& sRend = *m;
                    if (!IO::FileExists(libraryPath + sRend.mesh))
                        Model3DLoader::cache3DModel(location, sRend.meshSourceFile, nullptr);

                    Mesh* mesh = Mesh::load(libraryPath, sRend.mesh);
                    if (mesh == nullptr || !mesh->isLoaded())
                    {
                        Debug::logError("[" + name + "] Vegetation prefab mesh could not be loaded: " + sRend.mesh);
                        continue;
                    }
                    std::vector<Material*> materials;
                    for (auto mi = sRend.materials.begin(); mi != sRend.materials.end(); ++mi)
                    {
                        Material* material = Material::load(Engine::getSingleton()->getBuiltinResourcesPath(), *mi);
                        if (material == nullptr)
                        {
                            material = Material::load(location, *mi);
                            if (material == nullptr)
                            {
                                material = Material::create(location, *mi);
                                material->load();
                                material->save();
                            }
                        }
                        materials.push_back(material);
                    }

                    Model3DLoader::ModelMeshData data;
                    data.mesh = mesh;
                    data.materials = materials;
                    data.transform = transform;

                    outList.push_back(data);
                }
            }
        }
        else
            Debug::logError("[" + name + "] Error loading prefab: file does not exists");

        return outList;
    }

    void Model3DLoader::cache3DModel(std::string location, std::string path, std::function<void(float progress, std::string status)> progressCb)
    {
        std::string fullPath = location + path;

        if (!IO::FileExists(fullPath))
        {
            Debug::logError("[" + fullPath + "] Import error: File does not exists");
            return;
        }

        ImportData data = readSourceFile(location, path);

        const aiScene* scene = data.scene;

        if (scene == nullptr)
        {
            data.free();
            return;
        }
        
        aiNode* rootNode = scene->mRootNode;

        std::vector<aiNode*> nstack;
        for (int i = 0; i < rootNode->mNumChildren; ++i)
            nstack.push_back(rootNode->mChildren[i]);

        int idx = 0;
        int iter = 0;
        while (nstack.size() > 0)
        {
            aiNode* child = *nstack.begin();
            nstack.erase(nstack.begin());

            if (child->mNumMeshes > 0)
            {
                if (progressCb != nullptr)
                {
                    int progressEvery = (float)scene->mNumMeshes * (5.0f / 100.0f);
                    if (progressEvery > 0)
                    {
                        int progress = 100.0f / (float)scene->mNumMeshes * (float)iter;
                        if (iter % progressEvery == 0)
                            progressCb((float)progress / 100.0f, "Caching mesh: " + std::string(child->mName.C_Str()));
                    }
                }
                iter += child->mNumMeshes;
            }

            //Load model
            processMesh(scene, child, location, path, idx);
            ++idx;

            for (int j = 0; j < child->mNumChildren; ++j)
            {
                aiNode* ch = child->mChildren[j];
                nstack.insert(nstack.begin() + j, ch);
            }
        }

        data.free();
    }

    Mesh* processMesh(const aiScene* scene, aiNode* mNode, std::string location, std::string path, int index)
    {
        if (mNode->mNumMeshes == 0)
            return nullptr;

        auto exts = Engine::getImageFileFormats();

        std::string meshLocation = Engine::getSingleton()->getLibraryPath();
        std::string matDir = IO::GetFilePath(path) + "Materials/";
        std::string texDir = IO::GetFilePath(path);
        std::string meshDir = IO::GetFilePath(path);

        std::string meshAlias = mNode->mName.C_Str();
        std::string meshName = meshDir + md5(path + "_" + std::to_string(index)) + ".mesh";

        if (location != Engine::getSingleton()->getBuiltinResourcesPath())
        {
            if (!IO::DirExists(location + matDir))
                IO::CreateDir(location + matDir);
        }

        if (!IO::DirExists(meshLocation + meshDir))
            IO::CreateDir(meshLocation + meshDir, true);

        SModel3DMeta meta;
        std::string metaFilePath = Engine::getSingleton()->getLibraryPath() + path + ".meta";
        if (IO::FileExists(metaFilePath))
            meta.load(metaFilePath);
        
        Mesh* mesh = (Mesh*)Asset::getLoadedInstance(meshLocation, meshName);
        if (mesh == nullptr)
            mesh = (Mesh*)Asset::getLoadedInstance(meshLocation, meshAlias);
        
        if (mesh == nullptr || !mesh->isLoaded())
        {
            if (IO::FileExists(meshLocation + meshName))
            {
                //Load from cache in Library folder
                mesh = Mesh::load(meshLocation, meshName);
            }
            else
            {
                //Import from source file
                if (mesh == nullptr)
                {
                    mesh = Mesh::create(meshLocation, meshName);
                    mesh->setAlias(meshAlias);
                    mesh->setSourceFile(path);
                    mesh->setUserData(location);
                }

                for (int i = 0; i < mNode->mNumMeshes; ++i)
                {
                    //Geometry
                    unsigned int meshIndex = mNode->mMeshes[i];
                    aiMesh* mMesh = scene->mMeshes[meshIndex];

                    SubMesh* subMesh = new SubMesh();
                    mesh->addSubMesh(subMesh);

                    std::vector<VertexBuffer>& vbuf = subMesh->getVertexBuffer();
                    std::vector<uint32_t>& ibuf = subMesh->getIndexBuffer();
                    
                    vbuf.resize(mMesh->mNumVertices);
                    ibuf.resize(mMesh->mNumFaces * 3);

                    std::vector<glm::vec2> lightmapUVs;
                    std::vector<std::vector<std::pair<int, float>>> _blendWeights;
                    std::vector<std::vector<std::pair<int, float>>> blendWeights;
                    std::vector<int> vertIndexes;

                    vertIndexes.resize(mMesh->mNumVertices);
                    _blendWeights.resize(mMesh->mNumVertices);

                    for (int idx = 0; idx < mMesh->mNumVertices; ++idx)
                        vertIndexes[idx] = idx;

                    for (int b = 0; b < mMesh->mNumBones; ++b)
                    {
                        aiBone* mbone = mMesh->mBones[b];

                        for (int k = 0; k < mbone->mNumWeights; ++k)
                        {
                            aiVertexWeight w = mbone->mWeights[k];
                            _blendWeights[w.mVertexId].push_back(std::make_pair(b, w.mWeight));
                        }
                    }

                    //Optimize blend weights
                    for (auto& it : _blendWeights)
                    {
                        std::sort(it.begin(), it.end(), [=](std::pair<int, float>& a, std::pair<int, float>& b) -> bool
                            {
                                return a.second < b.second;
                            }
                        );
                        while (it.size() > 4)
                        {
                            it.erase(it.begin());
                        }

                        float totalWeight = 0.0f;
                        for (auto& w : it)
                            totalWeight += w.second;

                        if (totalWeight != 1.0f)
                        {
                            for (auto& w : it)
                                w.second = w.second / totalWeight;
                        }
                    }

                    bool genLightmapUVs = meta.generateLightmapUVs || location == Engine::getSingleton()->getBuiltinResourcesPath();

                    //Calculate lightmap UVs
                    if (genLightmapUVs)
                    {
                        float* vertices = new float[vbuf.size() * 3];
                        float* normals = new float[vbuf.size() * 3];
                        float* texCoords = new float[vbuf.size() * 2];
                        uint32_t* indices = new uint32_t[ibuf.size()];

                        for (int jj = 0, k = 0, j = 0; jj < mMesh->mNumVertices; ++jj, k += 3, j += 2)
                        {
                            aiVector3D p = mMesh->mVertices[jj];
                            vertices[k] = p.x;
                            vertices[k + 1] = p.y;
                            vertices[k + 2] = p.z;

                            aiVector3D n = mMesh->mNormals[jj];
                            normals[k] = n.x;
                            normals[k + 1] = n.y;
                            normals[k + 2] = n.z;

                            if (mMesh->mTextureCoords[0] != nullptr)
                            {
                                aiVector3D t = mMesh->mTextureCoords[0][jj];
                                texCoords[j] = t.x;
                                texCoords[j + 1] = t.y;
                            }
                            else
                            {
                                texCoords[j] = 0.0f;
                                texCoords[j + 1] = 0.0f;
                            }
                        }

                        for (int k = 0; k < mMesh->mNumFaces; ++k)
                        {
                            aiFace face = mMesh->mFaces[k];

                            for (int jj = 0; jj < 3; ++jj)
                                indices[k * 3 + jj] = (uint32_t)face.mIndices[jj];
                        }

                        xatlas::Atlas* atlas = xatlas::Create();
                        xatlas::MeshDecl meshDecl;
                        meshDecl.indexCount = ibuf.size();
                        meshDecl.indexFormat = xatlas::IndexFormat::UInt32;
                        meshDecl.indexData = indices;

                        meshDecl.vertexCount = vbuf.size();
                        meshDecl.vertexPositionData = vertices;
                        meshDecl.vertexPositionStride = sizeof(float) * 3;
                        meshDecl.vertexNormalData = normals;
                        meshDecl.vertexNormalStride = sizeof(float) * 3;

                        meshDecl.vertexUvData = texCoords;
                        meshDecl.vertexUvStride = sizeof(float) * 2;

                        xatlas::ChartOptions chartOptions = xatlas::ChartOptions();
                        xatlas::PackOptions packOptions = xatlas::PackOptions();
                        packOptions.padding = 4.0f;

                        //chartOptions.useInputMeshUvs = true;

                        xatlas::AddMesh(atlas, meshDecl);
                        xatlas::Generate(atlas, chartOptions, packOptions);

                        if (atlas->meshCount > 0)
                        {
                            xatlas::Mesh output_mesh = atlas->meshes[0];
                            
                            ibuf.resize(output_mesh.indexCount);
                            vbuf.resize(output_mesh.vertexCount);

                            lightmapUVs.resize(output_mesh.vertexCount);
                            vertIndexes.resize(output_mesh.vertexCount);
                            blendWeights.resize(output_mesh.vertexCount);

                            for (int j = 0; j < output_mesh.indexCount; ++j)
                            {
                                int idx = output_mesh.indexArray[j];
                                ibuf[j] = idx;
                            }
                            
                            for (int j = 0; j < output_mesh.vertexCount; ++j)
                            {
                                xatlas::Vertex vertex = output_mesh.vertexArray[j];

                                float x = vertex.uv[0] / atlas->width;
                                float y = vertex.uv[1] / atlas->height;

                                vertIndexes[j] = vertex.xref;
                                lightmapUVs[j] = glm::vec2(x, y);
                                blendWeights[j] = _blendWeights[vertex.xref];
                            }
                        }

                        xatlas::Destroy(atlas);

                        delete[] vertices;
                        delete[] normals;
                        delete[] texCoords;
                        delete[] indices;
                    }
                    else
                    {
                        blendWeights = _blendWeights;

                        for (int j = 0; j < mMesh->mNumFaces; ++j)
                        {
                            aiFace f = mMesh->mFaces[j];

                            for (int k = 0; k < f.mNumIndices; ++k)
                            {
                                unsigned int id = f.mIndices[k];
                                ibuf[j * 3 + k] = id;
                            }
                        }
                    }

                    //Fill vertex buffer
                    for (int jj = 0; jj < vertIndexes.size(); ++jj)
                    {
                        uint32_t j = vertIndexes[jj];

                        aiVector3D p = mMesh->mVertices[j];
                        aiVector3D n = aiVector3D(0, 0, 0);
                        aiVector3D tn = aiVector3D(0, 0, 0);
                        aiVector3D tbn = aiVector3D(0, 0, 0);
                        aiVector3D t1 = aiVector3D(0, 0, 0);
                        aiColor4D c = aiColor4D(1, 1, 1, 1);
                        glm::vec2 t2 = glm::vec2(0, 0);

                        if (mMesh->mNormals != nullptr)
                            n = mMesh->mNormals[j];

                        if (mMesh->mTangents != nullptr)
                            tn = mMesh->mTangents[j];

                        if (mMesh->mBitangents != nullptr)
                            tbn = mMesh->mBitangents[j];

                        if (mMesh->mTextureCoords[0] != nullptr)
                            t1 = mMesh->mTextureCoords[0][j];

                        if (mMesh->mColors != nullptr && mMesh->mColors[0] != nullptr)
                            c = mMesh->mColors[0][j];

                        if (genLightmapUVs)
                            t2 = lightmapUVs[jj];

                        glm::vec4 weights = glm::vec4(0, 0, 0, 0);
                        glm::vec4 indices = glm::vec4(0, 0, 0, 0);

                        for (int l = 0; l < blendWeights[jj].size(); ++l)
                        {
                            weights[l] = blendWeights[jj][l].second;
                            indices[l] = blendWeights[jj][l].first;
                        }

                        vbuf[jj] = VertexBuffer({
                            glm::vec3(p.x, p.y, p.z),
                            glm::vec3(n.x, n.y, n.z),
                            glm::vec3(tn.x, tn.y, tn.z),
                            glm::vec3(tbn.x, tbn.y, tbn.z),
                            glm::vec2(t1.x, t1.y),
                            t2,
                            weights,
                            indices,
                            Color::packABGR(Color(c.r, c.g, c.b, c.a))
                            }
                        );
                    }

                    lightmapUVs.clear();
                    vertIndexes.clear();
                    
                    for (auto& it : blendWeights)
                        it.clear();

                    for (auto& it : _blendWeights)
                        it.clear();

                    blendWeights.clear();
                    _blendWeights.clear();

                    //Generate LOD
                    if (meta.generateLod)
                    {
                        std::vector<glm::vec3> meshVerts;
                        for (auto& v : vbuf)
                            meshVerts.push_back(v.position);

                        int index_count = ibuf.size();

                        subMesh->setLodLevelsCount(meta.lodLevels);

                        for (int l = 1; l < meta.lodLevels + 1; ++l)
                        {
                            std::vector<uint32_t>& lodIBuf = subMesh->getLodIndexBuffer(l - 1);

                            float threshold = 1.0f - ((float)l / (float)(meta.lodLevels + 1)); //0..1
                            size_t target_index_count = size_t(index_count * threshold);
                            float target_error = meta.lodError;

                            std::vector<unsigned int> lod(index_count);
                            float lod_error = 0.f;

                            if (meta.lodPreserveMeshTopology)
                            {
                                lod.resize(meshopt_simplify(&lod[0], ibuf.data(), index_count, &meshVerts[0].x, meshVerts.size(), sizeof(glm::vec3),
                                    target_index_count, target_error,
#ifdef FALCO_MODERN_BGFX
                                    0, // meshoptimizer 0.25 simplify options
#endif
                                    &lod_error));
                            }
                            else
                            {
                                lod.resize(meshopt_simplifySloppy(&lod[0], ibuf.data(), index_count, &meshVerts[0].x, meshVerts.size(), sizeof(glm::vec3),
                                    target_index_count, target_error, &lod_error));
                            }

                            lodIBuf = lod;
                            lod.clear();
                        }

                        meshVerts.clear();
                    }

                    //Bones
                    for (int j = 0; j < mMesh->mNumBones; ++j)
                    {
                        aiBone* mbone = mMesh->mBones[j];

                        BoneInfo* bone = new BoneInfo();
                        bone->setName(mbone->mName.C_Str());

                        glm::mat4x4 offset = glm::identity<glm::mat4x4>();
                        offset[0] = glm::vec4(mbone->mOffsetMatrix[0][0], mbone->mOffsetMatrix[1][0], mbone->mOffsetMatrix[2][0], mbone->mOffsetMatrix[3][0]);
                        offset[1] = glm::vec4(mbone->mOffsetMatrix[0][1], mbone->mOffsetMatrix[1][1], mbone->mOffsetMatrix[2][1], mbone->mOffsetMatrix[3][1]);
                        offset[2] = glm::vec4(mbone->mOffsetMatrix[0][2], mbone->mOffsetMatrix[1][2], mbone->mOffsetMatrix[2][2], mbone->mOffsetMatrix[3][2]);
                        offset[3] = glm::vec4(mbone->mOffsetMatrix[0][3], mbone->mOffsetMatrix[1][3], mbone->mOffsetMatrix[2][3], mbone->mOffsetMatrix[3][3]);

                        bone->setOffsetMatrix(offset);

                        subMesh->addBone(bone);
                    }
                    
                    //Material
                    aiMaterial* mat = scene->mMaterials[mMesh->mMaterialIndex];

                    std::string mname = std::string(mat->GetName().C_Str());
                    mname = boost::replace_all_copy(mname, "\\", "_");
                    mname = boost::replace_all_copy(mname, "//", "_");
                    mname = boost::replace_all_copy(mname, "/", "_");

                    std::string matName = matDir + mname + ".material";
                    
                    Material* material = Material::load(location, matName);
                    if (material == nullptr)
                    {
                        material = Material::create(location, matName);
                        material->load();

                        aiString texture_file;
                        if (mat->GetTexture(aiTextureType_BASE_COLOR, 0, &texture_file) != AI_SUCCESS)
                            mat->GetTexture(aiTextureType_DIFFUSE, 0, &texture_file);
                        std::string _texture_file = IO::ReplaceBackSlashes(texture_file.C_Str());

                        Texture* _tex = nullptr;

                        const aiTexture* texture = _texture_file.empty() ? nullptr : scene->GetEmbeddedTexture(texture_file.C_Str());
                        if (texture != nullptr)
                        {
                            // Assimp embedded references such as *0 are not filenames.
                            std::string extension = texture->mHeight == 0 ? std::string(texture->achFormatHint) : "png";
                            if (extension.empty() || extension.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789") != std::string::npos)
                                extension = "bin";
                            _texture_file = "Textures/embedded_" + md5(path + "#" + _texture_file) + "." + extension;
                            const std::string texName = location + texDir + _texture_file;
                            if (!IO::FileExists(texName))
                            {
                                std::string _texDir = IO::GetFilePath(texName);
                                if (!IO::DirExists(_texDir))
                                    IO::CreateDir(_texDir, true);

                                //returned pointer is not null, read texture from memory
                                if (texture->mHeight == 0)
                                {
                                    std::ofstream output(texName, std::ios::binary);
                                    if (output)
                                        output.write(reinterpret_cast<const char*>(texture->pcData), texture->mWidth);
                                    if (!output)
                                        Debug::logError("Unable to extract embedded texture: " + texName);
                                }
                                else
                                {
                                    FIBITMAP* imagen = FreeImage_ConvertFromRawBits(reinterpret_cast<BYTE*>(texture->pcData), texture->mWidth, texture->mHeight, texture->mWidth * sizeof(aiTexel), 32, 0xFF0000, 0x00FF00, 0x0000FF, true);
                                    if (imagen != nullptr)
                                    {
                                        if (!FreeImage_Save(FIF_PNG, imagen, texName.c_str()))
                                            Debug::logError("Unable to save embedded texture: " + texName);
                                        FreeImage_Unload(imagen);
                                    }
                                }
                            }

                            if (IO::FileExists(texName))
                                _tex = Texture::load(location, texDir + _texture_file);
                        }
                        else
                        {
                            // Prefer the actual relative reference. Keep ../ segments meaningful.
                            const bool inferred = _texture_file.empty();
                            const std::string fileName = inferred ? mname : IO::GetFileNameWithExt(_texture_file);
                            auto tryRelative = [&](const std::string& candidate)
                            {
                                const auto root = boost::filesystem::absolute(location).lexically_normal();
                                const auto full = boost::filesystem::absolute(candidate).lexically_normal();
                                const std::string relative = IO::ReplaceBackSlashes(full.lexically_relative(root).generic_string());
                                if (relative.empty() || relative == ".." || relative.find("../") == 0)
                                    return false;
                                if (!IO::FileExists(full.string())) return false;
                                _tex = Texture::load(location, relative);
                                return _tex != nullptr && _tex->isLoaded();
                            };
                            if (!inferred)
                            {
                                if (!tryRelative(location + texDir + _texture_file))
                                    tryRelative(location + _texture_file);
                            }

                            // A missing reference may use material-name.ext, but only if unique.
                            // Never use substring matching: find("") used to choose any image.
                            std::vector<std::string> candidates;
                            if (_tex == nullptr && !fileName.empty())
                            IO::listFiles(location, true, nullptr, [=, &candidates](std::string d, std::string f) -> bool
                                {
                                    const bool matches = inferred ? IO::GetFileName(f) == fileName : f == fileName;
                                    if (matches)
                                    {
                                        std::string ext = IO::GetFileExtension(f);

                                        if (std::find(exts.begin(), exts.end(), ext) != exts.end())
                                        {
                                            std::string fullPath = d + f;
                                            candidates.push_back(fullPath);
                                        }
                                    }

                                    //Search next otherwise
                                    return true;
                                }
                            );
                            if (candidates.size() == 1)
                            {
                                if (tryRelative(candidates[0]) && inferred)
                                    Debug::log("Texture inferred from material name: " + mname + " -> " + candidates[0]);
                            }
                            else if (candidates.size() > 1)
                                Debug::logWarning("Ambiguous texture for material " + mname + "; assign it manually.");
                        }

                        //Set texture
                        if (_tex != nullptr && _tex->isLoaded())
                        {
                            //Search the first uniform of type Sampler2D
                            auto& uniforms = material->getUniforms();
                            for (auto& u : uniforms)
                            {
                                if (u.getType() == UniformType::Sampler2D)
                                {
                                    auto val = u.getValue<Sampler2DDef>();
                                    u.setValue(std::pair<int, Texture*>(val.first, _tex));
                                    break;
                                }
                            }
                        }

                        //Save material
                        material->save();
                    }

                    subMesh->setMaterialName(matName);
                    subMesh->setUseLightmapUVs(genLightmapUVs);
                }

                mesh->load();
                mesh->save();
            }
        }

        // Mesh cache files do not serialize runtime provenance. Restore the
        // original source location for both fresh disk loads and memory hits.
        if (mesh != nullptr)
            mesh->setUserData(location);

        return mesh;
    }

    aiMatrix4x4 getWorldTransform(aiNode* node)
    {
        aiMatrix4x4 result = node->mTransformation;
        
        aiNode* parent = node;
        while (parent->mParent != nullptr)
        {
            result = parent->mParent->mTransformation * result;
            parent = parent->mParent;
        }

        return result;
    }

    void Model3DLoader::extractAnimations(std::string location, std::string path)
    {
        std::string fullPath = location + path;

        if (!IO::FileExists(fullPath))
        {
            std::cerr << "[" << fullPath << "] Import error: File does not exists" << std::endl;
            return;
        }

        ImportData data = readSourceFile(location, path);
        const aiScene* scene = data.scene;

        if (scene == nullptr)
        {
            data.free();
            return;
        }

        extractAnimations(scene, location, path);

        data.free();
    }

    void Model3DLoader::extractAnimations(const aiScene* scene, std::string location, std::string path)
    {
        if (!scene->HasAnimations())
            return;

        IO::CreateDir(location + IO::GetFilePath(path) + IO::GetFileName(path));

        if (scene->mNumAnimations > 0)
        {
            for (int i = 0; i < scene->mNumAnimations; ++i)
            {
                aiAnimation* anim = scene->mAnimations[i];

                std::string animName = IO::GetFileName(path) + "/" + anim->mName.C_Str();
                if (animName.empty()) animName = IO::GetFileName(path) + "/Animation " + std::to_string(i) + ".animation";
                std::string animPath = IO::GetFilePath(path) + animName;

                AnimationClip* animClip = AnimationClip::load(location, animPath, false);
                if (animClip != nullptr)
                    animClip->clear();
                else
                    animClip = AnimationClip::create(location, animPath);

                animClip->setDuration(anim->mDuration + 1);
                animClip->setFramesPerSecond(anim->mTicksPerSecond);

                for (int j = 0; j < anim->mNumChannels; ++j)
                {
                    aiNodeAnim* nodeAnim = anim->mChannels[j];
                    AnimationClipNode* clipNode = new AnimationClipNode();

                    clipNode->setName(nodeAnim->mNodeName.C_Str());

                    for (int k = 0; k < anim->mDuration + 1; ++k)
                    {
                        int cur = k;

                        if (cur > nodeAnim->mNumPositionKeys - 1) cur = nodeAnim->mNumPositionKeys - 1;
                        aiVectorKey kval = nodeAnim->mPositionKeys[cur];
                        glm::vec3 pos = glm::vec3(kval.mValue.x, kval.mValue.y, kval.mValue.z);
                        clipNode->addPositionKey(kval.mTime, pos);

                        cur = k;
                        if (cur > nodeAnim->mNumScalingKeys - 1) cur = nodeAnim->mNumScalingKeys - 1;
                        aiVectorKey kval1 = nodeAnim->mScalingKeys[cur];
                        glm::vec3 scale = glm::vec3(kval1.mValue.x, kval1.mValue.y, kval1.mValue.z);
                        clipNode->addScalingKey(kval1.mTime, scale);

                        cur = k;
                        if (cur > nodeAnim->mNumRotationKeys - 1) cur = nodeAnim->mNumRotationKeys - 1;
                        aiQuatKey kval2 = nodeAnim->mRotationKeys[cur];
                        glm::highp_quat rot = glm::highp_quat(kval2.mValue.w, kval2.mValue.x, kval2.mValue.y, kval2.mValue.z);
                        clipNode->addRotationKey(kval2.mTime, rot);
                    }

                    animClip->addAnimationClipNode(clipNode);
                }

                animClip->save();
            }
        }
    }

    void Model3DLoader::exportObjects(std::string path, std::vector<GameObject*>& objects, int format)
    {
        //Fill data
        Assimp::Exporter exporter;
        aiScene scene;

        struct MaterialInfo
        {
            std::string name = "";
            int index = 0;
            aiMaterial* material = nullptr;
        };

        std::vector<aiMesh*> meshes;
        std::vector<MaterialInfo> materials;

        scene.mRootNode = new aiNode();
        std::vector<std::pair<Transform*, aiNode*>> nstack;

        for (auto obj : objects)
            nstack.push_back(std::make_pair(obj->getTransform(), scene.mRootNode));

        int currentMesh = 0;
        int currentMaterial = 0;

        while (nstack.size() > 0)
        {
            Transform* child = nstack.begin()->first;
            aiNode* pnode = nstack.begin()->second;
            nstack.erase(nstack.begin());

            //
            GameObject* obj = child->getGameObject();
            aiNode* nnode = new aiNode(obj->getName());
            pnode->addChildren(1, &nnode);

            aiVector3D pos = aiVector3D(child->getPosition().x, child->getPosition().y, child->getPosition().z);
            aiVector3D scl = aiVector3D(child->getScale().x, child->getScale().y, child->getScale().z);
            aiQuaternion rot = aiQuaternion(child->getRotation().w, child->getRotation().x, child->getRotation().y, child->getRotation().z);
            nnode->mTransformation = aiMatrix4x4(scl, rot, pos);

            MeshRenderer* renderer = (MeshRenderer*)obj->getComponent(MeshRenderer::COMPONENT_TYPE);
            CSGModel* csgModel = (CSGModel*)obj->getComponent(CSGModel::COMPONENT_TYPE);

            if (renderer != nullptr)
            {
                Mesh* mesh = renderer->getMesh();
                if (mesh != nullptr)
                {
                    nnode->mNumMeshes = mesh->getSubMeshCount();
                    nnode->mMeshes = new unsigned int[nnode->mNumMeshes];

                    for (int i = 0; i < nnode->mNumMeshes; ++i)
                    {
                        nnode->mMeshes[i] = currentMesh;
                        ++currentMesh;

                        SubMesh* subMesh = mesh->getSubMesh(i);

                        auto& vbuf = subMesh->getVertexBuffer();
                        auto& ibuf = subMesh->getIndexBuffer();

                        //Fill mesh data
                        aiMesh* aMesh = new aiMesh();
                        aMesh->mPrimitiveTypes = aiPrimitiveType_TRIANGLE;
                        meshes.push_back(aMesh);

                        aMesh->mNumVertices = vbuf.size();
                        aMesh->mVertices = new aiVector3D[aMesh->mNumVertices];
                        aMesh->mNormals = new aiVector3D[aMesh->mNumVertices];
                        aMesh->mTangents = new aiVector3D[aMesh->mNumVertices];
                        aMesh->mBitangents = new aiVector3D[aMesh->mNumVertices];
                        aMesh->mTextureCoords[0] = new aiVector3D[aMesh->mNumVertices];

                        aMesh->mNumUVComponents[0] = 2;

                        for (int j = 0; j < vbuf.size(); ++j)
                        {
                            aMesh->mVertices[j] = aiVector3D(vbuf[j].position.x, vbuf[j].position.y, vbuf[j].position.z);
                            aMesh->mNormals[j] = aiVector3D(vbuf[j].normal.x, vbuf[j].normal.y, vbuf[j].normal.z);
                            aMesh->mTangents[j] = aiVector3D(vbuf[j].tangent.x, vbuf[j].tangent.y, vbuf[j].tangent.z);
                            aMesh->mBitangents[j] = aiVector3D(vbuf[j].bitangent.x, vbuf[j].bitangent.y, vbuf[j].bitangent.z);
                            aMesh->mTextureCoords[0][j] = aiVector3D(vbuf[j].texcoord0.x, vbuf[j].texcoord0.y, 0);
                        }

                        aMesh->mNumFaces = ibuf.size() / 3;
                        aMesh->mFaces = new aiFace[aMesh->mNumFaces];

                        int faceIdx = 0;
                        for (int j = 0; j < ibuf.size(); j += 3)
                        {
                            aMesh->mFaces[faceIdx].mNumIndices = 3;
                            aMesh->mFaces[faceIdx].mIndices = new unsigned int[3];

                            aMesh->mFaces[faceIdx].mIndices[0] = ibuf[j];
                            aMesh->mFaces[faceIdx].mIndices[1] = ibuf[j + 1];
                            aMesh->mFaces[faceIdx].mIndices[2] = ibuf[j + 2];

                            ++faceIdx;
                        }

                        auto it = std::find_if(materials.begin(), materials.end(), [=](MaterialInfo& mt) -> bool
                            {
                                return mt.name == subMesh->getMaterialName();
                            }
                        );

                        if (it != materials.end())
                        {
                            aMesh->mMaterialIndex = it->index;
                        }
                        else
                        {
                            aMesh->mMaterialIndex = currentMaterial;
                            
                            MaterialInfo inf;
                            inf.index = currentMaterial;
                            inf.name = subMesh->getMaterialName();
                            inf.material = new aiMaterial();

                            ++currentMaterial;

                            materials.push_back(inf);
                        }
                    }
                }
            }
            else if (csgModel != nullptr)
            {
                nnode->mTransformation = aiMatrix4x4();

                //int modelIdx = 0;
                auto& models = CSGGeometry::getSingleton()->getModels();
                auto it = std::find_if(models.begin(), models.end(), [=](CSGGeometry::Model* mdl) -> bool
                    {
                        return mdl->component == csgModel;
                    }
                );

                if (it != models.end())
                {
                    CSGGeometry::Model* model = *it;

                    nnode->mNumMeshes = model->subMeshes.size();
                    nnode->mMeshes = new unsigned int[nnode->mNumMeshes];

                    for (int i = 0; i < nnode->mNumMeshes; ++i)
                    {
                        nnode->mMeshes[i] = currentMesh;
                        ++currentMesh;

                        CSGGeometry::SubMesh* subMesh = model->subMeshes[i];

                        auto& vbuf = subMesh->getVertexBuffer();
                        auto& ibuf = subMesh->getIndexBuffer();

                        //Fill mesh data
                        aiMesh* aMesh = new aiMesh();
                        aMesh->mPrimitiveTypes = aiPrimitiveType_TRIANGLE;
                        meshes.push_back(aMesh);

                        aMesh->mNumVertices = vbuf.size();
                        aMesh->mVertices = new aiVector3D[aMesh->mNumVertices];
                        aMesh->mNormals = new aiVector3D[aMesh->mNumVertices];
                        aMesh->mTangents = new aiVector3D[aMesh->mNumVertices];
                        aMesh->mBitangents = new aiVector3D[aMesh->mNumVertices];
                        aMesh->mTextureCoords[0] = new aiVector3D[aMesh->mNumVertices];

                        aMesh->mNumUVComponents[0] = 2;

                        for (int j = 0; j < vbuf.size(); ++j)
                        {
                            aMesh->mVertices[j] = aiVector3D(vbuf[j].position.x, vbuf[j].position.y, vbuf[j].position.z);
                            aMesh->mNormals[j] = aiVector3D(vbuf[j].normal.x, vbuf[j].normal.y, vbuf[j].normal.z);
                            aMesh->mTangents[j] = aiVector3D(vbuf[j].tangent.x, vbuf[j].tangent.y, vbuf[j].tangent.z);
                            aMesh->mBitangents[j] = aiVector3D(vbuf[j].bitangent.x, vbuf[j].bitangent.y, vbuf[j].bitangent.z);
                            aMesh->mTextureCoords[0][j] = aiVector3D(vbuf[j].texcoord0.x, vbuf[j].texcoord0.y, 0);
                        }

                        aMesh->mNumFaces = ibuf.size() / 3;
                        aMesh->mFaces = new aiFace[aMesh->mNumFaces];

                        int faceIdx = 0;
                        for (int j = 0; j < ibuf.size(); j += 3)
                        {
                            aMesh->mFaces[faceIdx].mNumIndices = 3;
                            aMesh->mFaces[faceIdx].mIndices = new unsigned int[3];

                            aMesh->mFaces[faceIdx].mIndices[0] = ibuf[j];
                            aMesh->mFaces[faceIdx].mIndices[1] = ibuf[j + 1];
                            aMesh->mFaces[faceIdx].mIndices[2] = ibuf[j + 2];

                            ++faceIdx;
                        }

                        auto it = std::find_if(materials.begin(), materials.end(), [=](MaterialInfo& mt) -> bool
                            {
                                Material* mat = subMesh->getMaterial();
                                std::string matName = "No Material";
                                if (mat != nullptr)
                                    matName = mat->getName();

                                return mt.name == matName;
                            }
                        );

                        if (it != materials.end())
                        {
                            aMesh->mMaterialIndex = it->index;
                        }
                        else
                        {
                            aMesh->mMaterialIndex = currentMaterial;

                            Material* mat = subMesh->getMaterial();
                            std::string matName = "No Material";
                            if (mat != nullptr)
                                matName = mat->getName();

                            MaterialInfo inf;
                            inf.index = currentMaterial;
                            inf.name = matName;
                            inf.material = new aiMaterial();

                            ++currentMaterial;

                            materials.push_back(inf);
                        }
                    }
                }
            }
            //

            int j = 0;
            for (auto it = child->getChildren().begin(); it != child->getChildren().end(); ++it, ++j)
            {
                Transform* ch = *it;
                nstack.insert(nstack.begin() + j, std::make_pair(ch, nnode));
            }
        }

        //Export
        scene.mMeshes = new aiMesh*[meshes.size()];
        scene.mNumMeshes = meshes.size();

        for (int i = 0; i < meshes.size(); ++i)
            scene.mMeshes[i] = meshes[i];

        scene.mMaterials = new aiMaterial*[materials.size()];
        scene.mNumMaterials = materials.size();

        for (int i = 0; i < materials.size(); ++i)
            scene.mMaterials[i] = materials[i].material;

        std::string fmt = "fbx";
        if (format == 0)
            fmt = "fbx";
        if (format == 1)
            fmt = "obj";

        exporter.Export(&scene, fmt, path);

        //Cleanup
        meshes.clear();
        materials.clear();
    }
}
