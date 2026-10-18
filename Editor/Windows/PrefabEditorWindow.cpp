#include "PrefabEditorWindow.h"
#include "MainWindow.h"
#include "InspectorWindow.h"
#include "../PropertyEditors/PropertyEditor.h"
#include "../PropertyEditors/Model3DEditor.h"
#include "../PropertyEditors/MaterialEditor.h"
#include "../Engine/Serialization/Settings/ProjectSettings.h"
#include "../Classes/TreeNode.h"
#include "../Engine/Assets/Prefab.h"
#include "../Engine/Assets/Mesh.h"
#include "../Engine/Assets/Material.h"
#include "../Engine/Assets/Texture.h"
#include "../Engine/Core/Engine.h"
#include "../Engine/Core/Debug.h"
#include "../Engine/Classes/GUIDGenerator.h"
#include "../Engine/Classes/IO.h"
#include "../Engine/Renderer/Renderer.h"
#include "../Engine/Components/Transform.h"
#include "../Engine/Serialization/Scene/SScene.h"
#include "../Engine/Gizmo/ImGuizmo.h"
#include "../Engine/glm/gtc/type_ptr.hpp"
#include "../Engine/glm/gtx/matrix_decompose.hpp"
#include <imgui.h>
#include <imgui_internal.h>
#include <dear-imgui/misc/cpp/imgui_stdlib.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <memory>
#include <set>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cfloat>
#include <cstring>
#include <stdexcept>
#include <map>
#ifdef _WIN32
#include <Windows.h>
#endif

namespace GX
{
#include "PrefabFields.h"
namespace
{
    struct Draft;
    class DraftInspector : public PropertyEditor
    {
        std::weak_ptr<Draft> draft;
    public:
        explicit DraftInspector(std::shared_ptr<Draft> value) : draft(value) { setEditorName("PrefabDraftInspector"); }
        void update() override;
    };
    std::shared_ptr<Draft> current;
    std::string readBytes(const std::filesystem::path& path)
    {
        std::ifstream file(path, std::ios::binary);
        if(!file) throw std::runtime_error("Cannot read prefab");
        std::ostringstream bytes; bytes << file.rdbuf();
        if(file.bad()) throw std::runtime_error("Cannot read prefab");
        return bytes.str();
    }
    std::string encode(SScene& scene)
    {
        std::ostringstream out(std::ios::binary | std::ios::out);
        BinarySerializer serializer; serializer.serialize(&out,&scene,Prefab::ASSET_TYPE); return out.str();
    }
    SScene decode(const std::string& bytes)
    {
        std::istringstream in(bytes,std::ios::binary | std::ios::in);
        SScene scene; BinarySerializer serializer; serializer.deserialize(&in,&scene,Prefab::ASSET_TYPE); return scene;
    }
    glm::mat4 matrix(SGameObject& object)
    { return Transform::makeTransformMatrix(object.position.getValue(),object.rotation.getValue(),object.scale.getValue()); }
    bool assignMatrix(SGameObject& object, const glm::mat4& value)
    {
        glm::vec3 scale,position,skew; glm::quat rotation; glm::vec4 perspective;
        if(!glm::decompose(value,scale,rotation,position,skew,perspective)) return false;
        if(!std::isfinite(position.x)||!std::isfinite(position.y)||!std::isfinite(position.z)
            ||!std::isfinite(scale.x)||!std::isfinite(scale.y)||!std::isfinite(scale.z)) return false;
        object.position=SVector3(position); object.rotation=SQuaternion(glm::normalize(rotation)); object.scale=SVector3(scale); return true;
    }
    struct Draft : std::enable_shared_from_this<Draft>
    {
        std::string path, diskBytes, savedBytes, error;
        SScene scene;
        std::vector<std::string> history;
        size_t historyPos=0, selected=0;
        bool activate=true, opened=true, focused=false, pending=false, previewDirty=true, firstFrame=true, requestInspector=true;
        int operation=0;
        std::unique_ptr<Model3DEditor> preview;
        std::vector<std::pair<size_t,Texture*>> decalImages;
        bool root(size_t index) const
        {
            const auto& parent=scene.gameObjects[index].parentGuid;
            return parent.empty() || std::none_of(scene.gameObjects.begin(),scene.gameObjects.end(),[&](const auto& o){return o.guid==parent;});
        }
        bool validHierarchy()
        {
            std::set<std::string> ids;size_t roots=0;
            for(size_t i=0;i<scene.gameObjects.size();++i)
            {
                const auto& object=scene.gameObjects[i];
                if(object.guid.empty() || !ids.insert(object.guid).second)return false;
                if(root(i))++roots;
                std::set<std::string> chain;const SGameObject* cursor=&object;
                while(cursor)
                {
                    if(!chain.insert(cursor->guid).second)return false;
                    auto parent=std::find_if(scene.gameObjects.begin(),scene.gameObjects.end(),[&](const auto& o){return o.guid==cursor->parentGuid;});
                    cursor=parent==scene.gameObjects.end()?nullptr:&*parent;
                }
            }
            return roots==1;
        }
        bool dirty() const { return pending || (!history.empty() && history[historyPos]!=savedBytes); }
        void changed() { pending=true; previewDirty=true; }
        void commitEdit()
        {
            if(!pending) return;
            auto bytes=encode(scene); pending=false;
            if(!history.empty() && bytes==history[historyPos]) return;
            history.resize(historyPos+1); history.push_back(std::move(bytes)); ++historyPos;
            size_t memory=0; for(const auto& entry:history) memory+=entry.size();
            while(history.size()>2 && (history.size()>64 || memory>64*1024*1024))
            { memory-=history.front().size(); history.erase(history.begin()); --historyPos; }
        }
        void travel(int direction)
        {
            commitEdit();
            if(direction<0 && historyPos>0) --historyPos;
            else if(direction>0 && historyPos+1<history.size()) ++historyPos;
            else return;
            scene=decode(history[historyPos]); selected=std::min(selected,scene.gameObjects.empty()?size_t(0):scene.gameObjects.size()-1);
            previewDirty=true;
        }
        std::set<std::string> descendants(size_t index) const
        {
            std::set<std::string> ids{scene.gameObjects[index].guid};
            bool added=true; while(added) { added=false; for(const auto& obj:scene.gameObjects)
                if(ids.count(obj.parentGuid) && ids.insert(obj.guid).second) added=true; }
            return ids;
        }
        void transform(const glm::mat4& value)
        {
            if(selected>=scene.gameObjects.size())return;
            auto& object=scene.gameObjects[selected]; const auto old=matrix(object);
            if(std::abs(glm::determinant(old))<0.000001f) { error="Cannot transform a prefab object with zero scale.";return; }
            const auto ids=descendants(selected); const auto delta=value*glm::inverse(old);
            if(!assignMatrix(object,value))return;
            for(auto& child:scene.gameObjects) if(child.guid!=object.guid && ids.count(child.guid)) assignMatrix(child,delta*matrix(child));
            changed();
        }
        void select(size_t index) { commitEdit(); selected=index; requestInspector=true; }
        void attachInspector()
        {
            requestInspector=false;
            auto weak=weak_from_this();
            MainWindow::addOnEndUpdateCallback([weak]() { if(auto d=weak.lock(); d && d->opened)
                MainWindow::getInspectorWindow()->setEditor(new DraftInspector(d)); });
        }
        bool save()
        {
            try
            {
                commitEdit();
                const auto target=std::filesystem::u8path(Engine::getSingleton()->getAssetsPath()+path);
                if(readBytes(target)!=diskBytes) { error="Prefab changed on disk. Close this draft and reopen it before saving.";return false; }
                if(!validHierarchy()) { error="Prefab must have one root, unique objects and no hierarchy cycles.";return false; }
                auto bytes=encode(scene); auto check=decode(bytes);
                if(check.gameObjects.size()!=scene.gameObjects.size())throw std::runtime_error("Prefab verification failed");
                const auto backupDir=std::filesystem::u8path(Engine::getSingleton()->getLibraryPath())/"PrefabBackups";
                std::filesystem::create_directories(backupDir);
                const auto backup=backupDir/(target.filename().wstring()+L"."+std::to_wstring(std::chrono::system_clock::now().time_since_epoch().count())+L".bak");
                std::filesystem::copy_file(target,backup);
                auto temp=target; temp+=L".editing-"+std::filesystem::u8path(GUIDGenerator::genGuid()).wstring()+L".tmp";
                try
                {
                    { std::ofstream out(temp,std::ios::binary|std::ios::trunc); out.write(bytes.data(),bytes.size());out.flush();if(!out)throw std::runtime_error("Prefab write failed"); }
#ifdef _WIN32
                    if(readBytes(target)!=diskBytes)throw std::runtime_error("Prefab changed on disk while saving");
                    if(!MoveFileExW(temp.c_str(),target.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)) throw std::runtime_error("Cannot replace prefab file");
#else
                    std::filesystem::rename(temp,target);
#endif
                }
                catch(...) { std::error_code ec;std::filesystem::remove(temp,ec);throw; }
                diskBytes=bytes;savedBytes=bytes;error.clear();
                if(auto* prefab=Prefab::load(Engine::getSingleton()->getAssetsPath(),path)) prefab->reload();
                Debug::log("Prefab saved: "+path+"; previous version backed up in Library/PrefabBackups");
                return true;
            }
            catch(const std::exception& ex) { error=ex.what();return false; }
        }
        void discard() { scene=decode(savedBytes);history={savedBytes};historyPos=0;pending=false;previewDirty=true;error.clear();selected=0; }
        bool confirm()
        {
            commitEdit(); if(!dirty()) return true;
#ifdef _WIN32
            int choice=MessageBoxA(nullptr,"Save changes to the prefab?\nYes: save. No: discard. Cancel: keep editing.","Unsaved prefab",MB_YESNOCANCEL|MB_ICONQUESTION);
            if(choice==IDCANCEL)return false;
            if(choice==IDYES)return save();
            return true;
#else
            error="Save or discard the prefab draft before closing.";return false;
#endif
        }
        void rebuildPreview()
        {
            if(!preview) { preview=std::make_unique<Model3DEditor>();firstFrame=true; }
            std::vector<Model3DLoader::ModelMeshData> data;
            decalImages.clear();
            auto assets=Engine::getSingleton()->getAssetsPath();auto library=Engine::getSingleton()->getLibraryPath();
            for(size_t i=0;i<scene.gameObjects.size();++i)
            {
                auto& object=scene.gameObjects[i];bool active=object.enabled;
                const SGameObject* ancestor=&object;
                while(active)
                {
                    auto parent=std::find_if(scene.gameObjects.begin(),scene.gameObjects.end(),[&](const auto& candidate){return candidate.guid==ancestor->parentGuid;});
                    if(parent==scene.gameObjects.end())break;
                    ancestor=&*parent;active=ancestor->enabled;
                }
                if(!active)continue;
                for(auto& renderer:object.meshRenderers)
                {
                    if(!renderer.enabled || renderer.mesh.empty())continue;
                    Model3DLoader::ModelMeshData mesh;mesh.transform=matrix(object);
                    const auto builtin=Engine::getSingleton()->getBuiltinResourcesPath();
                    if(renderer.mesh.find("Primitives/UnitScaleV1/")==0)
                    {
                        // The unit-primitive cache is created by the normal asset importer.
                        // Avoid loadBuiltinPrimitiveMesh here: its fallback instantiates scene objects.
                        if(IO::FileExists(library+renderer.mesh))mesh.mesh=Mesh::load(library,renderer.mesh);
                        else { error="Primitive preview cache is missing; prefab data is preserved.";continue; }
                    }
                    else
                    {
                        if(!IO::FileExists(library+renderer.mesh) && IO::FileExists(assets+renderer.meshSourceFile))
                            Model3DLoader::cache3DModel(assets,renderer.meshSourceFile,nullptr);
                        mesh.mesh=Mesh::load(library,renderer.mesh);
                    }
                    if(!mesh.mesh)continue;
                    for(const auto& name:renderer.materials)
                    {
                        auto* material=Material::load(assets,name);
                        if(!material)material=Material::load(Engine::getSingleton()->getBuiltinResourcesPath(),name);
                        mesh.materials.push_back(material?material:Renderer::getDefaultStandardMaterial());
                    }
                    data.push_back(std::move(mesh));
                }
                for(auto& decal:object.decals)
                {
                    if(!decal.enabled)continue;
                    auto* mat=Material::load(assets,decal.material);
                    Texture* texture=nullptr;
                    if(mat && mat->hasUniform("t_albedoMap")) texture=mat->getUniform<Sampler2DDef>("t_albedoMap").second;
                    decalImages.emplace_back(i,texture);
                }
            }
            glm::vec3 minimum(FLT_MAX),maximum(-FLT_MAX);
            for(auto& object:scene.gameObjects)
            {
                const auto extent=glm::abs(object.scale.getValue())*0.5f;
                minimum=glm::min(minimum,object.position.getValue()-extent);
                maximum=glm::max(maximum,object.position.getValue()+extent);
            }
            glm::vec3 origin(0.0f);
            for(size_t i=0;i<scene.gameObjects.size();++i)
                if(root(i)){origin=scene.gameObjects[i].position.getValue();break;}
            preview->setDraftPreview(data,firstFrame,(minimum+maximum)*0.5f,glm::length(maximum-minimum),origin);firstFrame=false;previewDirty=false;
        }
        void drawInspector();
        void drawTree();
        void drawOverlay(const glm::mat4& view,const glm::mat4& projection,const glm::mat4& orientation,float x,float y,float w,float h);
    };

    template<class T> bool componentList(const char* name, std::vector<T>& list)
    {
        bool changed=false;
        ImGui::PushID(name);
        for(size_t i=0;i<list.size();++i)
        {
            ImGui::PushID(static_cast<int>(i));
            bool expanded=ImGui::CollapsingHeader(name,ImGuiTreeNodeFlags_DefaultOpen);
            if(expanded)
            {
                changed |= PrefabFields::fields(list[i]);
                if(ImGui::SmallButton("Remove component"))
                { list.erase(list.begin()+i);changed=true;ImGui::PopID();break; }
            }
            ImGui::PopID();
        }
        ImGui::PopID();return changed;
    }
    void DraftInspector::update()
    {
        if(auto d=draft.lock(); d && d->opened) d->drawInspector();
        else ImGui::TextDisabled("Prefab editor closed.");
    }
    void Draft::drawInspector()
    {
        ImGui::TextWrapped("Prefab: %s%s",path.c_str(),dirty()?" *":"");
        if(ImGui::Button("Save prefab"))save();ImGui::SameLine();
        if(ImGui::Button("Discard changes"))discard();
        if(!error.empty())ImGui::TextWrapped("%s",error.c_str());
        ImGui::Separator();
        if(selected>=scene.gameObjects.size())return;
        auto& object=scene.gameObjects[selected];bool edited=false;
        PrefabFields::draftObjects=&scene.gameObjects;
        edited |= PrefabFields::field("Enabled",object.enabled);
        edited |= PrefabFields::field("Name",object.name);
        auto tags=Engine::getSingleton()->getSettings()->getTags(); tags.insert(tags.begin(),"Untagged");
        auto layers=Engine::getSingleton()->getSettings()->getLayers(); layers.insert(layers.begin(),"Default");
        auto choice=[&](const char* label, int& value, const std::vector<std::string>& names)
        {
            const std::string selectedName=value>=0 && value<int(names.size())?names[value]:"Unknown ("+std::to_string(value)+")";
            bool changed=false;
            if(ImGui::BeginCombo(label,selectedName.c_str()))
            {
                for(size_t i=0;i<names.size();++i)
                {
                    ImGui::PushID(static_cast<int>(i));
                    if(!names[i].empty() && ImGui::Selectable(names[i].c_str(),value==int(i))){value=static_cast<int>(i);changed=true;}
                    ImGui::PopID();
                }
                ImGui::EndCombo();
            }
            return changed;
        };
        edited |= choice("Tag",object.tag,tags);
        edited |= choice("Layer",object.layer,layers);
        if(ImGui::CollapsingHeader("Transform",ImGuiTreeNodeFlags_DefaultOpen))
        {
            glm::mat4 parent(1.0f);
            for(auto& candidate:scene.gameObjects)if(candidate.guid==object.parentGuid){parent=matrix(candidate);break;}
            if(std::abs(glm::determinant(parent))>0.000001f)
            {
                auto local=object;assignMatrix(local,glm::inverse(parent)*matrix(object));
                bool moved=PrefabFields::field("Position",local.position);
                moved |= PrefabFields::field("Rotation",local.rotation);
                moved |= PrefabFields::field("Scale",local.scale);
                if(moved)transform(parent*matrix(local));
            }
        }
        if(ImGui::TreeNode("Static flags"))
        {
            edited |= PrefabFields::field("Navigation static",object.navigationStatic);
            edited |= PrefabFields::field("Lighting static",object.lightingStatic);
            edited |= PrefabFields::field("Batching static",object.batchingStatic);
            edited |= PrefabFields::field("Occlusion static",object.occlusionStatic);
            ImGui::TreePop();
        }
        edited |= componentList("MeshRenderer",object.meshRenderers);
        edited |= componentList("Light",object.lights);
        edited |= componentList("Camera",object.cameras);
        edited |= componentList("Animation",object.animations);
        edited |= componentList("RigidBody",object.rigidBodies);
        edited |= componentList("BoxCollider",object.boxColliders);
        edited |= componentList("CapsuleCollider",object.capsuleColliders);
        edited |= componentList("MeshCollider",object.meshColliders);
        edited |= componentList("SphereCollider",object.sphereColliders);
        edited |= componentList("TerrainCollider",object.terrainColliders);
        edited |= componentList("Vehicle",object.vehicles);
        edited |= componentList("FixedJoint",object.fixedJoints);
        edited |= componentList("FreeJoint",object.freeJoints);
        edited |= componentList("HingeJoint",object.hingeJoints);
        edited |= componentList("ConeTwistJoint",object.coneTwistJoints);
        edited |= componentList("AudioListener",object.audioListeners);
        edited |= componentList("AudioSource",object.audioSources);
        edited |= componentList("NavMeshAgent",object.navMeshAgents);
        edited |= componentList("NavMeshObstacle",object.navMeshObstacles);
        edited |= componentList("Canvas",object.canvases);
        edited |= componentList("Image",object.images);
        edited |= componentList("Text",object.texts);
        edited |= componentList("Button",object.buttons);
        edited |= componentList("Mask",object.masks);
        edited |= componentList("MonoScript",object.monoScripts);
        edited |= componentList("Terrain",object.terrains);
        edited |= componentList("ParticleSystem",object.particleSystems);
        edited |= componentList("Water",object.waters);
        edited |= componentList("CloudSystem",object.cloudSystems);
        edited |= componentList("Spline",object.splines);
        edited |= componentList("VideoPlayer",object.videoPlayers);
        edited |= componentList("CSGModel",object.csgModels);
        edited |= componentList("CSGBrush",object.csgBrushes);
        edited |= componentList("TextInput",object.textInputs);
        edited |= componentList("DecalRenderer",object.decals);
        if(ImGui::Button("Add component"))ImGui::OpenPopup("AddPrefabComponent");
        if(ImGui::BeginPopup("AddPrefabComponent"))
        {
            int nextIndex=0;
            for(const auto& component:object.meshRenderers)nextIndex=std::max(nextIndex,component.index+1);
            for(const auto& component:object.lights)nextIndex=std::max(nextIndex,component.index+1);
            for(const auto& component:object.cameras)nextIndex=std::max(nextIndex,component.index+1);
            for(const auto& component:object.animations)nextIndex=std::max(nextIndex,component.index+1);
            for(const auto& component:object.rigidBodies)nextIndex=std::max(nextIndex,component.index+1);
            for(const auto& component:object.boxColliders)nextIndex=std::max(nextIndex,component.index+1);
            for(const auto& component:object.capsuleColliders)nextIndex=std::max(nextIndex,component.index+1);
            for(const auto& component:object.meshColliders)nextIndex=std::max(nextIndex,component.index+1);
            for(const auto& component:object.sphereColliders)nextIndex=std::max(nextIndex,component.index+1);
            for(const auto& component:object.terrainColliders)nextIndex=std::max(nextIndex,component.index+1);
            for(const auto& component:object.vehicles)nextIndex=std::max(nextIndex,component.index+1);
            for(const auto& component:object.fixedJoints)nextIndex=std::max(nextIndex,component.index+1);
            for(const auto& component:object.freeJoints)nextIndex=std::max(nextIndex,component.index+1);
            for(const auto& component:object.hingeJoints)nextIndex=std::max(nextIndex,component.index+1);
            for(const auto& component:object.coneTwistJoints)nextIndex=std::max(nextIndex,component.index+1);
            for(const auto& component:object.audioListeners)nextIndex=std::max(nextIndex,component.index+1);
            for(const auto& component:object.audioSources)nextIndex=std::max(nextIndex,component.index+1);
            for(const auto& component:object.navMeshAgents)nextIndex=std::max(nextIndex,component.index+1);
            for(const auto& component:object.navMeshObstacles)nextIndex=std::max(nextIndex,component.index+1);
            for(const auto& component:object.canvases)nextIndex=std::max(nextIndex,component.index+1);
            for(const auto& component:object.images)nextIndex=std::max(nextIndex,component.index+1);
            for(const auto& component:object.texts)nextIndex=std::max(nextIndex,component.index+1);
            for(const auto& component:object.buttons)nextIndex=std::max(nextIndex,component.index+1);
            for(const auto& component:object.masks)nextIndex=std::max(nextIndex,component.index+1);
            for(const auto& component:object.monoScripts)nextIndex=std::max(nextIndex,component.index+1);
            for(const auto& component:object.terrains)nextIndex=std::max(nextIndex,component.index+1);
            for(const auto& component:object.particleSystems)nextIndex=std::max(nextIndex,component.index+1);
            for(const auto& component:object.waters)nextIndex=std::max(nextIndex,component.index+1);
            for(const auto& component:object.cloudSystems)nextIndex=std::max(nextIndex,component.index+1);
            for(const auto& component:object.splines)nextIndex=std::max(nextIndex,component.index+1);
            for(const auto& component:object.videoPlayers)nextIndex=std::max(nextIndex,component.index+1);
            for(const auto& component:object.csgModels)nextIndex=std::max(nextIndex,component.index+1);
            for(const auto& component:object.csgBrushes)nextIndex=std::max(nextIndex,component.index+1);
            for(const auto& component:object.textInputs)nextIndex=std::max(nextIndex,component.index+1);
            for(const auto& component:object.decals)nextIndex=std::max(nextIndex,component.index+1);
            if(object.meshRenderers.empty() && ImGui::Selectable("MeshRenderer")) { object.meshRenderers.emplace_back();object.meshRenderers.back().index=nextIndex;edited=true; }
            if(object.lights.empty() && ImGui::Selectable("Light")) { object.lights.emplace_back();object.lights.back().index=nextIndex;edited=true; }
            if(object.cameras.empty() && ImGui::Selectable("Camera")) { object.cameras.emplace_back();object.cameras.back().index=nextIndex;edited=true; }
            if(object.animations.empty() && ImGui::Selectable("Animation")) { object.animations.emplace_back();object.animations.back().index=nextIndex;edited=true; }
            if(object.rigidBodies.empty() && ImGui::Selectable("RigidBody")) { object.rigidBodies.emplace_back();object.rigidBodies.back().index=nextIndex;edited=true; }
            if(ImGui::Selectable("BoxCollider")) { object.boxColliders.emplace_back();object.boxColliders.back().index=nextIndex;edited=true; }
            if(ImGui::Selectable("CapsuleCollider")) { object.capsuleColliders.emplace_back();object.capsuleColliders.back().index=nextIndex;edited=true; }
            if(object.meshColliders.empty() && ImGui::Selectable("MeshCollider")) { object.meshColliders.emplace_back();object.meshColliders.back().index=nextIndex;edited=true; }
            if(ImGui::Selectable("SphereCollider")) { object.sphereColliders.emplace_back();object.sphereColliders.back().index=nextIndex;edited=true; }
            if(object.terrainColliders.empty() && ImGui::Selectable("TerrainCollider")) { object.terrainColliders.emplace_back();object.terrainColliders.back().index=nextIndex;edited=true; }
            if(object.vehicles.empty() && ImGui::Selectable("Vehicle")) { object.vehicles.emplace_back();object.vehicles.back().index=nextIndex;edited=true; }
            if(object.fixedJoints.empty() && ImGui::Selectable("FixedJoint")) { object.fixedJoints.emplace_back();object.fixedJoints.back().index=nextIndex;edited=true; }
            if(object.freeJoints.empty() && ImGui::Selectable("FreeJoint")) { object.freeJoints.emplace_back();object.freeJoints.back().index=nextIndex;edited=true; }
            if(object.hingeJoints.empty() && ImGui::Selectable("HingeJoint")) { object.hingeJoints.emplace_back();object.hingeJoints.back().index=nextIndex;edited=true; }
            if(object.coneTwistJoints.empty() && ImGui::Selectable("ConeTwistJoint")) { object.coneTwistJoints.emplace_back();object.coneTwistJoints.back().index=nextIndex;edited=true; }
            if(object.audioListeners.empty() && ImGui::Selectable("AudioListener")) { object.audioListeners.emplace_back();object.audioListeners.back().index=nextIndex;edited=true; }
            if(object.audioSources.empty() && ImGui::Selectable("AudioSource")) { object.audioSources.emplace_back();object.audioSources.back().index=nextIndex;edited=true; }
            if(object.navMeshAgents.empty() && ImGui::Selectable("NavMeshAgent")) { object.navMeshAgents.emplace_back();object.navMeshAgents.back().index=nextIndex;edited=true; }
            if(ImGui::Selectable("NavMeshObstacle")) { object.navMeshObstacles.emplace_back();object.navMeshObstacles.back().index=nextIndex;edited=true; }
            if(object.canvases.empty() && ImGui::Selectable("Canvas")) { object.canvases.emplace_back();object.canvases.back().index=nextIndex;edited=true; }
            if(object.images.empty() && ImGui::Selectable("Image")) { object.images.emplace_back();object.images.back().index=nextIndex;edited=true; }
            if(object.texts.empty() && ImGui::Selectable("Text")) { object.texts.emplace_back();object.texts.back().index=nextIndex;edited=true; }
            if(object.buttons.empty() && ImGui::Selectable("Button")) { object.buttons.emplace_back();object.buttons.back().index=nextIndex;edited=true; }
            if(object.masks.empty() && ImGui::Selectable("Mask")) { object.masks.emplace_back();object.masks.back().index=nextIndex;edited=true; }
            if(ImGui::Selectable("MonoScript")) { object.monoScripts.emplace_back();object.monoScripts.back().index=nextIndex;edited=true; }
            if(object.terrains.empty() && ImGui::Selectable("Terrain")) { object.terrains.emplace_back();object.terrains.back().index=nextIndex;edited=true; }
            if(object.particleSystems.empty() && ImGui::Selectable("ParticleSystem")) { object.particleSystems.emplace_back();object.particleSystems.back().index=nextIndex;edited=true; }
            if(object.waters.empty() && ImGui::Selectable("Water")) { object.waters.emplace_back();object.waters.back().index=nextIndex;edited=true; }
            if(object.cloudSystems.empty() && ImGui::Selectable("CloudSystem")) { object.cloudSystems.emplace_back();object.cloudSystems.back().index=nextIndex;edited=true; }
            if(object.splines.empty() && ImGui::Selectable("Spline")) { object.splines.emplace_back();object.splines.back().index=nextIndex;edited=true; }
            if(object.videoPlayers.empty() && ImGui::Selectable("VideoPlayer")) { object.videoPlayers.emplace_back();object.videoPlayers.back().index=nextIndex;edited=true; }
            if(object.csgModels.empty() && ImGui::Selectable("CSGModel")) { object.csgModels.emplace_back();object.csgModels.back().index=nextIndex;edited=true; }
            if(object.csgBrushes.empty() && ImGui::Selectable("CSGBrush")) { object.csgBrushes.emplace_back();object.csgBrushes.back().index=nextIndex;edited=true; }
            if(object.textInputs.empty() && ImGui::Selectable("TextInput")) { object.textInputs.emplace_back();object.textInputs.back().index=nextIndex;edited=true; }
            if(object.decals.empty() && ImGui::Selectable("DecalRenderer")) { object.decals.emplace_back();object.decals.back().index=nextIndex;edited=true; }
            ImGui::EndPopup();
        }
        ImGui::TextWrapped("Changes affect this prefab after Save. Material paths refer to shared project assets.");
        if(edited)changed();
        PrefabFields::draftObjects=nullptr;
    }
    void Draft::drawTree()
    {
        if(ImGui::Button("Add child") && selected<scene.gameObjects.size())
        {
            commitEdit();SGameObject child;child.name="GameObject";child.guid=GUIDGenerator::genGuid();
            child.parentGuid=scene.gameObjects[selected].guid;assignMatrix(child,matrix(scene.gameObjects[selected]));
            scene.gameObjects.push_back(child);selected=scene.gameObjects.size()-1;changed();requestInspector=true;
        }
        if(selected<scene.gameObjects.size() && !root(selected))
        {
            ImGui::SameLine();if(ImGui::Button("Delete"))
            {
                commitEdit();auto ids=descendants(selected);
                auto& objects=scene.gameObjects;objects.erase(std::remove_if(objects.begin(),objects.end(),[&](const auto& obj){return ids.count(obj.guid)!=0;}),objects.end());
                selected=0;changed();requestInspector=true;
            }
        }
        std::vector<bool> visited(scene.gameObjects.size(),false);
        std::function<void(size_t,int)> node=[&](size_t i,int depth)
        {
            if(visited[i] || depth>64)return;visited[i]=true;
            auto& object=scene.gameObjects[i];
            ImGui::PushID(object.guid.c_str());
            auto flags=ImGuiTreeNodeFlags_OpenOnArrow|ImGuiTreeNodeFlags_DefaultOpen;
            if(selected==i)flags|=ImGuiTreeNodeFlags_Selected;
            bool expanded=ImGui::TreeNodeEx("node",flags,"%s%s",object.name.c_str(),object.enabled?"":" (disabled)");
            if(ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())select(i);
            if(ImGui::BeginDragDropSource())
            {
                ImGui::SetDragDropPayload("FalcoPrefabObject",&i,sizeof(i));ImGui::TextUnformatted(object.name.c_str());ImGui::EndDragDropSource();
            }
            if(ImGui::BeginDragDropTarget())
            {
                if(const auto* payload=ImGui::AcceptDragDropPayload("FalcoPrefabObject"))
                {
                    const auto source=*static_cast<const size_t*>(payload->Data);
                    if(source<scene.gameObjects.size() && !root(source) && !descendants(source).count(object.guid))
                    {commitEdit();scene.gameObjects[source].parentGuid=object.guid;changed();}
                }
                ImGui::EndDragDropTarget();
            }
            if(expanded)
            {
                for(size_t j=0;j<scene.gameObjects.size();++j)if(scene.gameObjects[j].parentGuid==object.guid)node(j,depth+1);
                ImGui::TreePop();
            }
            else {auto ids=descendants(i);for(size_t j=0;j<scene.gameObjects.size();++j)if(ids.count(scene.gameObjects[j].guid))visited[j]=true;}
            ImGui::PopID();
        };
        for(size_t i=0;i<scene.gameObjects.size();++i)if(root(i))node(i,0);
        for(size_t i=0;i<scene.gameObjects.size();++i)if(!visited[i])node(i,0);
    }
    void Draft::drawOverlay(const glm::mat4& view,const glm::mat4& projection,const glm::mat4& orientation,float x,float y,float w,float h)
    {
        auto* draw=ImGui::GetWindowDrawList();draw->PushClipRect(ImVec2(x,y),ImVec2(x+w,y+h),true);
        const auto vp=projection*view;
        auto point=[&](const glm::vec3& v,ImVec2& out)
        {
            auto clip=vp*glm::vec4(v,1);if(clip.w<=0.001f)return false;
            out=ImVec2(x+(clip.x/clip.w+1)*w*0.5f,y+(1-clip.y/clip.w)*h*0.5f);return true;
        };
        // A neutral local XZ receiver illustrates each decal without becoming prefab data.
        for(const auto& entry:decalImages)
        {
            if(entry.first>=scene.gameObjects.size())continue;
            const auto m=orientation*matrix(scene.gameObjects[entry.first]);
            const glm::vec3 corners[]={{-0.5f,0,-0.5f},{0.5f,0,-0.5f},{0.5f,0,0.5f},{-0.5f,0,0.5f}};
            ImVec2 p[4],surface[4];bool visible=true;
            for(int i=0;i<4;++i){visible &= point(glm::vec3(m*glm::vec4(corners[i],1)),p[i]);visible &= point(glm::vec3(m*glm::vec4(corners[i]*1.5f,1)),surface[i]);}
            if(!visible)continue;
            draw->AddQuadFilled(surface[0],surface[1],surface[2],surface[3],IM_COL32(125,125,125,255));
            if(entry.second)draw->AddImageQuad((ImTextureID)(intptr_t)entry.second->getHandle().idx,p[0],p[1],p[2],p[3],ImVec2(0,1),ImVec2(1,1),ImVec2(1,0),ImVec2(0,0));
            draw->AddQuad(p[0],p[1],p[2],p[3],IM_COL32(70,170,240,255));
        }
        draw->PopClipRect();
        if(selected>=scene.gameObjects.size())return;
        auto model=orientation*matrix(scene.gameObjects[selected]);
        ImGuizmo::SetID(7401);ImGuizmo::SetDrawlist();ImGuizmo::SetRect(x,y,w,h);ImGuizmo::SetClipRect(x,y,x+w,y+h);ImGuizmo::SetOrthographic(false);
        const auto before=model;
        ImGuizmo::Manipulate(glm::value_ptr(view),glm::value_ptr(projection),operation==0?ImGuizmo::TRANSLATE:operation==1?ImGuizmo::ROTATE:ImGuizmo::SCALE,ImGuizmo::LOCAL,glm::value_ptr(model));
        if(ImGuizmo::IsUsing() && std::memcmp(glm::value_ptr(before),glm::value_ptr(model),sizeof(glm::mat4))!=0)transform(glm::inverse(orientation)*model);
    }
}
    void PrefabEditorWindow::open(const std::string& path)
    {
        if(MainWindow::getIsPlaying())return;
        if(current && current->path==path){current->activate=true;current->requestInspector=true;return;}
        if(current && !current->confirm())return;
        try
        {
            auto next=std::make_shared<Draft>();next->path=path;
            next->diskBytes=readBytes(std::filesystem::u8path(Engine::getSingleton()->getAssetsPath()+path));
            next->scene=decode(next->diskBytes);
            if(next->scene.gameObjects.empty())throw std::runtime_error("Prefab has no objects");
            if(!next->validHierarchy())throw std::runtime_error("Prefab hierarchy is invalid");
            next->savedBytes=encode(next->scene);next->history={next->savedBytes};
            if(current)current->opened=false;current=next;
        }
        catch(const std::exception& ex){Debug::logError(std::string("Cannot edit prefab: ")+ex.what());}
    }
    void PrefabEditorWindow::update()
    {
        if(!current)return;auto d=current;
        bool open=true;
        ImGui::SetNextWindowSize(ImVec2(850,550),ImGuiCond_FirstUseEver);
        if(auto* sceneWindow=ImGui::FindWindowByName("Scene");sceneWindow && sceneWindow->DockId)
            ImGui::SetNextWindowDockID(sceneWindow->DockId,ImGuiCond_FirstUseEver);
        const std::string title="Prefab: "+d->path+(d->dirty()?" *":"")+"###PrefabEditor";
        if(d->activate){ImGui::SetNextWindowFocus();d->activate=false;}
        bool visible=ImGui::Begin(title.c_str(),&open,ImGuiWindowFlags_NoCollapse);
        const bool wasFocused=d->focused;
        d->focused=visible && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        if(visible)
        {
            if(d->focused && !wasFocused && (!MainWindow::getInspectorWindow()->getCurrentEditor() || MainWindow::getInspectorWindow()->getCurrentEditor()->getEditorName()!="PrefabDraftInspector"))d->requestInspector=true;
            if(ImGui::Button("Save prefab"))d->save();ImGui::SameLine();
            if(ImGui::Button("Discard changes"))d->discard();ImGui::SameLine();
            if(ImGui::Button("Undo"))d->travel(-1);ImGui::SameLine();
            if(ImGui::Button("Redo"))d->travel(1);ImGui::SameLine();
            if(ImGui::Button("Frame prefab")){d->firstFrame=true;d->previewDirty=true;}ImGui::SameLine();
            ImGui::RadioButton("Move",&d->operation,0);ImGui::SameLine();ImGui::RadioButton("Rotate",&d->operation,1);ImGui::SameLine();ImGui::RadioButton("Scale",&d->operation,2);
            if(!d->error.empty())ImGui::TextWrapped("%s",d->error.c_str());
            ImGui::TextDisabled("Right drag: orbit | Middle drag: pan | Wheel: zoom");
            if(ImGui::BeginChild("PrefabHierarchy",ImVec2(220,0),true))d->drawTree();ImGui::EndChild();ImGui::SameLine();
            if(ImGui::BeginChild("PrefabPreview",ImVec2(0,0),true))
            {
                if(MainWindow::getIsPlaying()) ImGui::TextDisabled("Prefab preview is paused during Play.");
                else
                {
                if(d->previewDirty || !d->preview)d->rebuildPreview();
                ImGui::TextDisabled("Mesh/decal preview; scripts, physics and particle simulation are stopped.");
                d->preview->drawDraftPreview(ImGui::GetContentRegionAvail().y,[d](const auto& view,const auto& projection,const auto& orientation,float x,float y,float w,float h){d->drawOverlay(view,projection,orientation,x,y,w,h);});
                }
            }
            ImGui::EndChild();
            if(d->requestInspector)d->attachInspector();
        }
        ImGui::End();
        if(d->pending && !ImGui::IsAnyItemActive() && !ImGuizmo::IsUsing())d->commitEdit();
        if(!open && d->confirm())shutdown();
    }
    bool PrefabEditorWindow::ownsFocus()
    {
        if(!current)return false;
        auto* inspector=MainWindow::getInspectorWindow();auto* editor=inspector->getCurrentEditor();
        return current->focused || (inspector->isFocused() && editor && editor->getEditorName()=="PrefabDraftInspector");
    }
    bool PrefabEditorWindow::save(){return current?current->save():false;}
    void PrefabEditorWindow::undo(){if(current)current->travel(-1);}
    void PrefabEditorWindow::redo(){if(current)current->travel(1);}
    bool PrefabEditorWindow::confirmClose(){return !current || current->confirm();}
    void PrefabEditorWindow::invalidatePreview(){if(current){current->preview.reset();current->decalImages.clear();current->previewDirty=true;}}
    void PrefabEditorWindow::shutdown()
    {
        if(current)current->opened=false;
        auto* inspector=MainWindow::getInspectorWindow();auto* editor=inspector->getCurrentEditor();
        if(editor && editor->getEditorName()=="PrefabDraftInspector")inspector->setEditor(nullptr);
        current.reset();
    }
}
