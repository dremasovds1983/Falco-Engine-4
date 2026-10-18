// Serialized prefab field adapters. Runtime components are never instantiated.
// Keep these adapters in sync when adding fields to Engine/Serialization.
namespace PrefabFields
{
    inline const std::vector<SGameObject>* draftObjects = nullptr;
    inline bool field(const char* label, bool& v) { return ImGui::Checkbox(label, &v); }
    inline bool field(const char* label, int& v) { return ImGui::InputInt(label, &v); }
    inline bool field(const char* label, unsigned int& v) { return ImGui::InputScalar(label, ImGuiDataType_U32, &v); }
    inline bool field(const char* label, unsigned long long& v) { return ImGui::InputScalar(label, ImGuiDataType_U64, &v); }
    inline bool field(const char* label, float& v) { return ImGui::DragFloat(label, &v, 0.01f); }
    inline bool field(const char* label, double& v) { return ImGui::InputDouble(label, &v); }
    inline bool field(const char* label, unsigned char& v) { int n=v; if (!ImGui::InputInt(label,&n)) return false; v=static_cast<unsigned char>(std::clamp(n,0,255)); return true; }
    inline bool field(const char* label, char& v) { int n=v; if (!ImGui::InputInt(label,&n)) return false; v=static_cast<char>(std::clamp(n,-128,127)); return true; }
    inline bool field(const char* label, std::string& v)
    {
        std::string name(label);
        if(name.find("guid")!=std::string::npos && draftObjects)
        {
            std::string title=name.substr(0,name.find("guid"));
            std::string selected=v.empty()?"None":"Missing object";
            for(const auto& object:*draftObjects)if(object.guid==v)selected=object.name;
            bool changed=false;
            if(ImGui::BeginCombo(title.c_str(),selected.c_str()))
            {
                if(ImGui::Selectable("None",v.empty())){v.clear();changed=true;}
                for(const auto& object:*draftObjects)
                {
                    ImGui::PushID(object.guid.c_str());
                    if(ImGui::Selectable(object.name.c_str(),v==object.guid)){v=object.guid;changed=true;}
                    ImGui::PopID();
                }
                ImGui::EndCombo();
            }
            return changed;
        }
        bool changed=ImGui::InputText(label,&v);
        const bool resource = name.find("Material") != std::string::npos || name.find("Texture") != std::string::npos
            || name.find("File") != std::string::npos || name.find("Path") != std::string::npos || name.find("Mesh") != std::string::npos || name.find("Model") != std::string::npos;
        if (resource && ImGui::BeginDragDropTarget())
        {
            if (const auto* payload=ImGui::AcceptDragDropPayload("DND_TreeView"))
            {
                const auto* node=static_cast<TreeNode*>(payload->Data);
                if (node) { v=const_cast<TreeNode*>(node)->getPath(); changed=true; }
            }
            ImGui::EndDragDropTarget();
        }
        if(name.find("Material")!=std::string::npos && !v.empty())
        {
            ImGui::PushID(label);
            if(ImGui::SmallButton("Open shared material"))
            {
                const auto path=v;
                MainWindow::addOnEndUpdateCallback([path]() {
                    auto* material=Material::load(Engine::getSingleton()->getAssetsPath(),path);
                    if(!material || !material->isLoaded())return;
                    auto* editor=new MaterialEditor();editor->init(material,true);
                    MainWindow::getInspectorWindow()->setEditor(editor);
                });
            }
            if(ImGui::IsItemHovered())ImGui::SetTooltip("Material edits are saved separately and are not discarded with the prefab draft.");
            ImGui::PopID();
        }
        return changed;
    }
    inline bool field(const char* label, SVector2& v) { float a[]={v.x,v.y}; if(!ImGui::DragFloat2(label,a,0.01f))return false; v=SVector2(a[0],a[1]);return true; }
    inline bool field(const char* label, SVector3& v) { float a[]={v.x,v.y,v.z}; if(!ImGui::DragFloat3(label,a,0.01f))return false; v=SVector3(a[0],a[1],a[2]);return true; }
    inline bool field(const char* label, SVector4& v) { float a[]={v.x,v.y,v.z,v.w}; if(!ImGui::DragFloat4(label,a,0.01f))return false; v=SVector4(a[0],a[1],a[2],a[3]);return true; }
    inline bool field(const char* label, SColor& v) { float a[]={v.r,v.g,v.b,v.a}; if(!ImGui::ColorEdit4(label,a))return false; v=SColor(a[0],a[1],a[2],a[3]);return true; }
    inline bool field(const char* label, SQuaternion& v)
    {
        glm::vec3 angles=glm::degrees(glm::eulerAngles(v.getValue()));
        if(!ImGui::DragFloat3(label,glm::value_ptr(angles),0.2f))return false;
        v=SQuaternion(glm::quat(glm::radians(angles)));return true;
    }
    inline bool fields(SSampler2D& v);
    inline bool fields(SSamplerCube& v);
    inline bool fields(SUniform& v);
    inline bool fields(SMaterial& v);
    inline bool fields(SAnimationClipInfo& v);
    inline bool fields(SAnimation& v);
    inline bool fields(SAudioListener& v);
    inline bool fields(SAudioSource& v);
    inline bool fields(SBoxCollider& v);
    inline bool fields(SButton& v);
    inline bool fields(SCamera& v);
    inline bool fields(SCanvas& v);
    inline bool fields(SCapsuleCollider& v);
    inline bool fields(SCloudSystem& v);
    inline bool fields(SComponent& v);
    inline bool fields(SConeTwistJoint& v);
    inline bool fields(SFaceInfo& v);
    inline bool fields(SCSGBrush& v);
    inline bool fields(SCSGModel& v);
    inline bool fields(SDecalRenderer& v);
    inline bool fields(SFixedJoint& v);
    inline bool fields(SFreeJoint& v);
    inline bool fields(SHingeJoint& v);
    inline bool fields(SImage& v);
    inline bool fields(SLight& v);
    inline bool fields(SMask& v);
    inline bool fields(SMeshCollider& v);
    inline bool fields(SMeshRenderer& v);
    inline bool fields(SMonoFieldInfo& v);
    inline bool fields(SMonoScript& v);
    inline bool fields(SNavMeshAgent& v);
    inline bool fields(SNavMeshObstacle& v);
    inline bool fields(SParticleModifier& v);
    inline bool fields(SParticleGravityModifier& v);
    inline bool fields(SParticleColorModifier& v);
    inline bool fields(SParticleSizeModifier& v);
    inline bool fields(SParticleDirectionModifier& v);
    inline bool fields(SParticleRotationModifier& v);
    inline bool fields(SParticleSpeedModifier& v);
    inline bool fields(SParticleEmitter& v);
    inline bool fields(SParticleSystem& v);
    inline bool fields(SRigidBody& v);
    inline bool fields(SSphereCollider& v);
    inline bool fields(SSpline& v);
    inline bool fields(STerrain& v);
    inline bool fields(STerrainCollider& v);
    inline bool fields(SText& v);
    inline bool fields(STextInput& v);
    inline bool fields(SUIElement& v);
    inline bool fields(SWheelInfo& v);
    inline bool fields(SVehicle& v);
    inline bool fields(SVideoPlayer& v);
    inline bool fields(SWater& v);
    template<class T> bool field(const char* label, T& v)
    {
        bool changed=false;
        if(ImGui::TreeNode(label)) { changed=fields(v); ImGui::TreePop(); }
        return changed;
    }
    template<class T> bool field(const char* label, std::vector<T>& values)
    {
        bool changed=false;
        if (ImGui::TreeNode(label,"%s (%d)",label,static_cast<int>(values.size())))
        {
            // Large mesh/terrain arrays are data, not interactive forms.
            if(values.size()>512) ImGui::TextDisabled("Large data array preserved (%d entries)",static_cast<int>(values.size()));
            else
            {
                for(size_t i=0;i<values.size();++i)
                {
                    ImGui::PushID(static_cast<int>(i));
                    changed |= field(label,values[i]);
                    ImGui::PopID();
                }
                ImGui::TextDisabled("Array structure is preserved.");
            }
            ImGui::TreePop();
        }
        return changed;
    }
    inline bool field(const char* label, std::vector<bool>& values)
    {
        bool changed=false;
        if(ImGui::TreeNode(label)) {
            for(size_t i=0;i<values.size();++i) { ImGui::PushID(static_cast<int>(i)); bool v=values[i]; if(field("Value",v)){values[i]=v;changed=true;} ImGui::PopID(); }
            ImGui::TreePop();
        }
        return changed;
    }
    inline bool fields(SSampler2D& v)
    {
        bool changed=false;
        changed |= field("Reg", v.reg);
        changed |= field("Texture name", v.textureName);
        return changed;
    }
    inline bool fields(SSamplerCube& v)
    {
        bool changed=false;
        changed |= field("Reg", v.reg);
        changed |= field("Cubemap name", v.cubemapName);
        return changed;
    }
    inline bool fields(SUniform& v)
    {
        bool changed=false;
        changed |= field("Name", v.name);
        changed |= field("Uniform type", v.uniformType);
        changed |= field("Int val", v.intVal);
        changed |= field("Float val", v.floatVal);
        changed |= field("Bool val", v.boolVal);
        changed |= field("Vec2 val", v.vec2Val);
        changed |= field("Vec3 val", v.vec3Val);
        changed |= field("Vec4 val", v.vec4Val);
        changed |= field("Color val", v.colorVal);
        changed |= field("Sampler2 dval", v.sampler2DVal);
        changed |= field("Sampler cube val", v.samplerCubeVal);
        return changed;
    }
    inline bool fields(SMaterial& v)
    {
        bool changed=false;
        changed |= field("Shader name", v.shaderName);
        changed |= field("Uniforms", v.uniforms);
        return changed;
    }
    inline bool fields(SAnimationClipInfo& v)
    {
        bool changed=false;
        changed |= field("Clip path", v.clipPath);
        changed |= field("Name", v.name);
        changed |= field("Start frame", v.startFrame);
        changed |= field("End frame", v.endFrame);
        changed |= field("Loop", v.loop);
        changed |= field("Speed", v.speed);
        return changed;
    }
    inline bool fields(SAnimation& v)
    {
        bool changed=false;
        changed |= fields(static_cast<SComponent&>(v));
        changed |= field("Animation clips", v.animationClips);
        return changed;
    }
    inline bool fields(SAudioListener& v)
    {
        bool changed=false;
        changed |= fields(static_cast<SComponent&>(v));
        changed |= field("Volume", v.volume);
        return changed;
    }
    inline bool fields(SAudioSource& v)
    {
        bool changed=false;
        changed |= fields(static_cast<SComponent&>(v));
        changed |= field("File name", v.fileName);
        changed |= field("Play on start", v.playOnStart);
        changed |= field("Loop", v.loop);
        changed |= field("Volume", v.volume);
        changed |= field("Min distance", v.minDistance);
        changed |= field("Max distance", v.maxDistance);
        changed |= field("Is2 d", v.is2D);
        changed |= field("Pitch", v.pitch);
        return changed;
    }
    inline bool fields(SBoxCollider& v)
    {
        bool changed=false;
        changed |= fields(static_cast<SComponent&>(v));
        changed |= field("Box size", v.boxSize);
        changed |= field("Offset", v.offset);
        changed |= field("Rotation", v.rotation);
        changed |= field("Is trigger", v.isTrigger);
        return changed;
    }
    inline bool fields(SButton& v)
    {
        bool changed=false;
        changed |= fields(static_cast<SUIElement&>(v));
        changed |= field("Tex normal path", v.texNormalPath);
        changed |= field("Tex hover path", v.texHoverPath);
        changed |= field("Tex active path", v.texActivePath);
        changed |= field("Tex disabled path", v.texDisabledPath);
        changed |= field("Col hover", v.colHover);
        changed |= field("Col active", v.colActive);
        changed |= field("Col disabled", v.colDisabled);
        changed |= field("Interactable", v.interactable);
        changed |= field("Image type", v.imageType);
        return changed;
    }
    inline bool fields(SCamera& v)
    {
        bool changed=false;
        changed |= fields(static_cast<SComponent&>(v));
        changed |= field("Fovy", v.fovy);
        changed |= field("Znear", v.znear);
        changed |= field("Zfar", v.zfar);
        changed |= field("Viewport l", v.viewport_l);
        changed |= field("Viewport t", v.viewport_t);
        changed |= field("Viewport w", v.viewport_w);
        changed |= field("Viewport h", v.viewport_h);
        changed |= field("Depth", v.depth);
        changed |= field("Clear color", v.clearColor);
        changed |= field("Clear flags", v.clearFlags);
        changed |= field("Projection type", v.projectionType);
        changed |= field("Orthographic size", v.orthographicSize);
        changed |= field("Occlusion culling", v.occlusionCulling);
        changed |= field("Culling mask", v.cullingMask);
        return changed;
    }
    inline bool fields(SCanvas& v)
    {
        bool changed=false;
        changed |= fields(static_cast<SComponent&>(v));
        changed |= field("Ref screen width", v.refScreenWidth);
        changed |= field("Ref screen height", v.refScreenHeight);
        changed |= field("Screen match side", v.screenMatchSide);
        changed |= field("Mode", v.mode);
        return changed;
    }
    inline bool fields(SCapsuleCollider& v)
    {
        bool changed=false;
        changed |= fields(static_cast<SComponent&>(v));
        changed |= field("Radius", v.radius);
        changed |= field("Height", v.height);
        changed |= field("Offset", v.offset);
        changed |= field("Rotation", v.rotation);
        changed |= field("Is trigger", v.isTrigger);
        return changed;
    }
    inline bool fields(SCloudSystem& v)
    {
        bool changed=false;
        changed |= fields(static_cast<SComponent&>(v));
        changed |= field("Coverage", v.coverage);
        changed |= field("Density", v.density);
        changed |= field("Wind", v.wind);
        changed |= field("Has material", v.hasMaterial);
        changed |= field("Material", v.material);
        return changed;
    }
    inline bool fields(SComponent& v)
    {
        bool changed=false;
        changed |= field("Enabled", v.enabled);
        return changed;
    }
    inline bool fields(SConeTwistJoint& v)
    {
        bool changed=false;
        changed |= fields(static_cast<SComponent&>(v));
        changed |= field("Connected object guid", v.connectedObjectGuid);
        changed |= field("Anchor", v.anchor);
        changed |= field("Connected anchor", v.connectedAnchor);
        changed |= field("Limits", v.limits);
        changed |= field("Linked bodies collision", v.linkedBodiesCollision);
        return changed;
    }
    inline bool fields(SFaceInfo& v)
    {
        bool changed=false;
        changed |= field("Indices", v.indices);
        changed |= field("Tex coords", v.texCoords);
        changed |= field("Tex coords scale", v.texCoordsScale);
        changed |= field("Tex coords offset", v.texCoordsOffset);
        changed |= field("Tex coords rotation", v.texCoordsRotation);
        changed |= field("Material", v.material);
        changed |= field("Smooth normals", v.smoothNormals);
        return changed;
    }
    inline bool fields(SCSGBrush& v)
    {
        bool changed=false;
        changed |= fields(static_cast<SComponent&>(v));
        changed |= field("Brush type", v.brushType);
        changed |= field("Brush operation", v.brushOperation);
        changed |= field("Cast shadows", v.castShadows);
        changed |= field("Segments", v.segments);
        changed |= field("Stacks", v.stacks);
        changed |= field("Vertices", v.vertices);
        changed |= field("Faces", v.faces);
        return changed;
    }
    inline bool fields(SCSGModel& v)
    {
        bool changed=false;
        changed |= fields(static_cast<SComponent&>(v));
        return changed;
    }
    inline bool fields(SDecalRenderer& v)
    {
        bool changed=false;
        changed |= fields(static_cast<SComponent&>(v));
        changed |= field("Material", v.material);
        return changed;
    }
    inline bool fields(SFixedJoint& v)
    {
        bool changed=false;
        changed |= fields(static_cast<SComponent&>(v));
        changed |= field("Connected object guid", v.connectedObjectGuid);
        changed |= field("Anchor", v.anchor);
        changed |= field("Connected anchor", v.connectedAnchor);
        changed |= field("Linked bodies collision", v.linkedBodiesCollision);
        return changed;
    }
    inline bool fields(SFreeJoint& v)
    {
        bool changed=false;
        changed |= fields(static_cast<SComponent&>(v));
        changed |= field("Connected object guid", v.connectedObjectGuid);
        changed |= field("Anchor", v.anchor);
        changed |= field("Connected anchor", v.connectedAnchor);
        changed |= field("Limit min", v.limitMin);
        changed |= field("Limit max", v.limitMax);
        changed |= field("Linked bodies collision", v.linkedBodiesCollision);
        return changed;
    }
    inline bool fields(SHingeJoint& v)
    {
        bool changed=false;
        changed |= fields(static_cast<SComponent&>(v));
        changed |= field("Connected object guid", v.connectedObjectGuid);
        changed |= field("Anchor", v.anchor);
        changed |= field("Connected anchor", v.connectedAnchor);
        changed |= field("Axis", v.axis);
        changed |= field("Limit min", v.limitMin);
        changed |= field("Limit max", v.limitMax);
        changed |= field("Linked bodies collision", v.linkedBodiesCollision);
        return changed;
    }
    inline bool fields(SImage& v)
    {
        bool changed=false;
        changed |= fields(static_cast<SUIElement&>(v));
        changed |= field("Texture path", v.texturePath);
        changed |= field("Image type", v.imageType);
        return changed;
    }
    inline bool fields(SLight& v)
    {
        bool changed=false;
        changed |= fields(static_cast<SComponent&>(v));
        changed |= field("Color", v.color);
        changed |= field("Light type", v.lightType);
        changed |= field("Radius", v.radius);
        changed |= field("Intensity", v.intensity);
        changed |= field("Inner radius", v.innerRadius);
        changed |= field("Outer radius", v.outerRadius);
        changed |= field("Bias", v.bias);
        changed |= field("Cast shadows", v.castShadows);
        changed |= field("Light render mode", v.lightRenderMode);
        return changed;
    }
    inline bool fields(SMask& v)
    {
        bool changed=false;
        changed |= fields(static_cast<SUIElement&>(v));
        return changed;
    }
    inline bool fields(SMeshCollider& v)
    {
        bool changed=false;
        changed |= fields(static_cast<SComponent&>(v));
        changed |= field("Convex", v.convex);
        changed |= field("Is trigger", v.isTrigger);
        return changed;
    }
    inline bool fields(SMeshRenderer& v)
    {
        bool changed=false;
        changed |= fields(static_cast<SComponent&>(v));
        changed |= field("Materials", v.materials);
        if(ImGui::SmallButton("Add material slot")){v.materials.emplace_back();changed=true;}
        ImGui::SameLine();if(!v.materials.empty() && ImGui::SmallButton("Remove last slot")){v.materials.pop_back();changed=true;}
        ImGui::TextWrapped("Model: %s",v.meshSourceFile.c_str());
        struct ModelChoice { std::string mesh; std::vector<std::string> materials; };
        struct ModelPicker { std::string candidate, loadedSource, message; std::vector<ModelChoice> choices; };
        static std::map<ImGuiID,ModelPicker> pickers;
        auto& picker=pickers[ImGui::GetID("ModelPicker")];
        field("New model source",picker.candidate);
        if(ImGui::SmallButton("Load model") && !picker.candidate.empty())
        {
            picker.choices.clear();picker.message.clear();
            const auto assets=Engine::getSingleton()->getAssetsPath();
            if(IO::FileExists(assets+picker.candidate))
            {
                auto loaded=Model3DLoader::load3DModelMeshes(assets,picker.candidate);
                for(const auto& model:loaded)
                {
                    if(!model.mesh)continue;
                    ModelChoice choice;choice.mesh=model.mesh->getName();
                    for(auto* material:model.materials)choice.materials.push_back(material?material->getName():"");
                    picker.choices.push_back(std::move(choice));
                }
            }
            if(picker.choices.empty())picker.message="No meshes loaded. The previous model is unchanged.";
            else
            {
                picker.loadedSource=picker.candidate;
                v.meshSourceFile=picker.loadedSource;v.mesh=picker.choices.front().mesh;
                v.materials=picker.choices.front().materials;changed=true;
            }
        }
        if(!picker.message.empty())ImGui::TextWrapped("%s",picker.message.c_str());
        if(!picker.choices.empty() && ImGui::BeginCombo("Model mesh",v.mesh.c_str()))
        {
            for(const auto& choice:picker.choices)if(ImGui::Selectable(choice.mesh.c_str(),v.mesh==choice.mesh))
            {v.meshSourceFile=picker.loadedSource;v.mesh=choice.mesh;v.materials=choice.materials;changed=true;}
            ImGui::EndCombo();
        }
        changed |= field("Root object guid", v.rootObjectGuid);
        changed |= field("Cast shadows", v.castShadows);
        changed |= field("Lightmap size", v.lightmapSize);
        changed |= field("Lod max distance", v.lodMaxDistance);
        changed |= field("Cull over max distance", v.cullOverMaxDistance);
        return changed;
    }
    inline bool fields(SMonoFieldInfo& v)
    {
        bool changed=false;
        changed |= field("Field name", v.fieldName);
        changed |= field("Field type", v.fieldType);
        changed |= field("String val", v.stringVal);
        changed |= field("Bool val", v.boolVal);
        changed |= field("Int val", v.intVal);
        changed |= field("Float val", v.floatVal);
        changed |= field("Object val", v.objectVal);
        changed |= field("Vec2 val", v.vec2Val);
        changed |= field("Vec3 val", v.vec3Val);
        changed |= field("Vec4 val", v.vec4Val);
        changed |= field("String arr val", v.stringArrVal);
        return changed;
    }
    inline bool fields(SMonoScript& v)
    {
        bool changed=false;
        changed |= fields(static_cast<SComponent&>(v));
        changed |= field("Class name", v.className);
        changed |= field("Field list", v.fieldList);
        return changed;
    }
    inline bool fields(SNavMeshAgent& v)
    {
        bool changed=false;
        changed |= fields(static_cast<SComponent&>(v));
        changed |= field("Radius", v.radius);
        changed |= field("Height", v.height);
        changed |= field("Speed", v.speed);
        changed |= field("Acceleration", v.acceleration);
        changed |= field("Rotation speed", v.rotationSpeed);
        return changed;
    }
    inline bool fields(SNavMeshObstacle& v)
    {
        bool changed=false;
        changed |= fields(static_cast<SComponent&>(v));
        changed |= field("Size", v.size);
        changed |= field("Offset", v.offset);
        return changed;
    }
    inline bool fields(SParticleModifier& v)
    {
        bool changed=false;
        changed |= field("Start time", v.startTime);
        return changed;
    }
    inline bool fields(SParticleGravityModifier& v)
    {
        bool changed=false;
        changed |= fields(static_cast<SParticleModifier&>(v));
        changed |= field("Gravity", v.gravity);
        changed |= field("Damping", v.damping);
        return changed;
    }
    inline bool fields(SParticleColorModifier& v)
    {
        bool changed=false;
        changed |= fields(static_cast<SParticleModifier&>(v));
        changed |= field("Times", v.times);
        changed |= field("Colors", v.colors);
        return changed;
    }
    inline bool fields(SParticleSizeModifier& v)
    {
        bool changed=false;
        changed |= fields(static_cast<SParticleModifier&>(v));
        changed |= field("Times", v.times);
        changed |= field("Sizes", v.sizes);
        return changed;
    }
    inline bool fields(SParticleDirectionModifier& v)
    {
        bool changed=false;
        changed |= fields(static_cast<SParticleModifier&>(v));
        changed |= field("Random direction min", v.randomDirectionMin);
        changed |= field("Random direction max", v.randomDirectionMax);
        changed |= field("Constant direction", v.constantDirection);
        changed |= field("Change interval", v.changeInterval);
        changed |= field("Direction type", v.directionType);
        return changed;
    }
    inline bool fields(SParticleRotationModifier& v)
    {
        bool changed=false;
        changed |= fields(static_cast<SParticleModifier&>(v));
        changed |= field("Random rotation min", v.randomRotationMin);
        changed |= field("Random rotation max", v.randomRotationMax);
        changed |= field("Constant rotation", v.constantRotation);
        changed |= field("Offset", v.offset);
        changed |= field("Change interval", v.changeInterval);
        changed |= field("Rotation type", v.rotationType);
        return changed;
    }
    inline bool fields(SParticleSpeedModifier& v)
    {
        bool changed=false;
        changed |= fields(static_cast<SParticleModifier&>(v));
        changed |= field("Constant speed", v.constantSpeed);
        changed |= field("Random speed min", v.randomSpeedMin);
        changed |= field("Random speed max", v.randomSpeedMax);
        changed |= field("Change interval", v.changeInterval);
        changed |= field("Speed type", v.speedType);
        return changed;
    }
    inline bool fields(SParticleEmitter& v)
    {
        bool changed=false;
        changed |= field("Material", v.material);
        changed |= field("Simulation space", v.simulationSpace);
        changed |= field("Render type", v.renderType);
        changed |= field("Origin", v.origin);
        changed |= field("Shape", v.shape);
        changed |= field("Radius", v.radius);
        changed |= field("Size", v.size);
        changed |= field("Time scale", v.timeScale);
        changed |= field("Emission rate", v.emissionRate);
        changed |= field("Emission count", v.emissionCount);
        changed |= field("Duration", v.duration);
        changed |= field("Max particles", v.maxParticles);
        changed |= field("Start size min", v.startSizeMin);
        changed |= field("Start size max", v.startSizeMax);
        changed |= field("Life time min", v.lifeTimeMin);
        changed |= field("Life time max", v.lifeTimeMax);
        changed |= field("Start speed", v.startSpeed);
        changed |= field("Start direction", v.startDirection);
        changed |= field("Loop", v.loop);
        changed |= field("Enable physics", v.enablePhysics);
        changed |= field("Bounciness", v.bounciness);
        changed |= field("Friction", v.friction);
        changed |= field("Gravity modifiers", v.gravityModifiers);
        changed |= field("Color modifiers", v.colorModifiers);
        changed |= field("Size modifiers", v.sizeModifiers);
        changed |= field("Direction modifiers", v.directionModifiers);
        changed |= field("Rotation modifiers", v.rotationModifiers);
        changed |= field("Speed modifiers", v.speedModifiers);
        changed |= field("Animated", v.animated);
        changed |= field("Animation grid size", v.animationGridSize);
        changed |= field("Animation time mode", v.animationTimeMode);
        changed |= field("Animation fps", v.animationFps);
        return changed;
    }
    inline bool fields(SParticleSystem& v)
    {
        bool changed=false;
        changed |= fields(static_cast<SComponent&>(v));
        changed |= field("Cast shadows", v.castShadows);
        changed |= field("Play at start", v.playAtStart);
        changed |= field("Emitters", v.emitters);
        return changed;
    }
    inline bool fields(SRigidBody& v)
    {
        bool changed=false;
        changed |= fields(static_cast<SComponent&>(v));
        changed |= field("Mass", v.mass);
        changed |= field("Freeze position x", v.freezePositionX);
        changed |= field("Freeze position y", v.freezePositionY);
        changed |= field("Freeze position z", v.freezePositionZ);
        changed |= field("Freeze rotation x", v.freezeRotationX);
        changed |= field("Freeze rotation y", v.freezeRotationY);
        changed |= field("Freeze rotation z", v.freezeRotationZ);
        changed |= field("Is kinematic", v.isKinematic);
        changed |= field("Is static", v.isStatic);
        changed |= field("Use own gravity", v.useOwnGravity);
        changed |= field("Gravity", v.gravity);
        changed |= field("Friction", v.friction);
        changed |= field("Bounciness", v.bounciness);
        changed |= field("Linear damping", v.linearDamping);
        changed |= field("Angular damping", v.angularDamping);
        return changed;
    }
    inline bool fields(SSphereCollider& v)
    {
        bool changed=false;
        changed |= fields(static_cast<SComponent&>(v));
        changed |= field("Radius", v.radius);
        changed |= field("Offset", v.offset);
        changed |= field("Is trigger", v.isTrigger);
        return changed;
    }
    inline bool fields(SSpline& v)
    {
        bool changed=false;
        changed |= fields(static_cast<SComponent&>(v));
        changed |= field("Closed", v.closed);
        changed |= field("Points", v.points);
        return changed;
    }
    inline bool fields(STerrain& v)
    {
        bool changed=false;
        changed |= fields(static_cast<SComponent&>(v));
        changed |= field("File path", v.filePath);
        return changed;
    }
    inline bool fields(STerrainCollider& v)
    {
        bool changed=false;
        changed |= fields(static_cast<SComponent&>(v));
        changed |= field("Is trigger", v.isTrigger);
        return changed;
    }
    inline bool fields(SText& v)
    {
        bool changed=false;
        changed |= fields(static_cast<SUIElement&>(v));
        changed |= field("Font path", v.fontPath);
        changed |= field("Font size", v.fontSize);
        changed |= field("Text", v.text);
        changed |= field("Horizontal text alignment", v.horizontalTextAlignment);
        changed |= field("Vertical text alignment", v.verticalTextAlignment);
        changed |= field("Line spacing", v.lineSpacing);
        changed |= field("Font resolution", v.fontResolution);
        changed |= field("Word wrap", v.wordWrap);
        return changed;
    }
    inline bool fields(STextInput& v)
    {
        bool changed=false;
        changed |= fields(static_cast<SUIElement&>(v));
        changed |= field("Font path", v.fontPath);
        changed |= field("Tex normal path", v.texNormalPath);
        changed |= field("Tex hover path", v.texHoverPath);
        changed |= field("Tex active path", v.texActivePath);
        changed |= field("Tex disabled path", v.texDisabledPath);
        changed |= field("Col hover", v.colHover);
        changed |= field("Col active", v.colActive);
        changed |= field("Col disabled", v.colDisabled);
        changed |= field("Col text", v.colText);
        changed |= field("Col placeholder", v.colPlaceholder);
        changed |= field("Image type", v.imageType);
        changed |= field("Text", v.text);
        changed |= field("Placeholder", v.placeholder);
        changed |= field("Multiline", v.multiline);
        changed |= field("Font size", v.fontSize);
        changed |= field("Font resolution", v.fontResolution);
        changed |= field("Line spacing", v.lineSpacing);
        changed |= field("Padding", v.padding);
        changed |= field("Horizontal text alignment", v.horizontalTextAlignment);
        changed |= field("Vertical text alignment", v.verticalTextAlignment);
        changed |= field("Word wrap", v.wordWrap);
        changed |= field("Interactable", v.interactable);
        return changed;
    }
    inline bool fields(SUIElement& v)
    {
        bool changed=false;
        changed |= fields(static_cast<SComponent&>(v));
        changed |= field("Horizontal canvas alignment", v.horizontalCanvasAlignment);
        changed |= field("Vertical canvas alignment", v.verticalCanvasAlignment);
        changed |= field("Anchor", v.anchor);
        changed |= field("Color", v.color);
        changed |= field("Size", v.size);
        changed |= field("Raycast target", v.raycastTarget);
        return changed;
    }
    inline bool fields(SWheelInfo& v)
    {
        bool changed=false;
        changed |= field("M connected object guid", v.m_connectedObjectGuid);
        changed |= field("M radius", v.m_radius);
        changed |= field("M width", v.m_width);
        changed |= field("M suspension stiffness", v.m_suspensionStiffness);
        changed |= field("M suspension damping", v.m_suspensionDamping);
        changed |= field("M suspension compression", v.m_suspensionCompression);
        changed |= field("M suspension rest length", v.m_suspensionRestLength);
        changed |= field("M friction", v.m_friction);
        changed |= field("M roll influence", v.m_rollInfluence);
        changed |= field("M direction", v.m_direction);
        changed |= field("M axle", v.m_axle);
        changed |= field("M connection point", v.m_connectionPoint);
        changed |= field("M is front wheel", v.m_isFrontWheel);
        return changed;
    }
    inline bool fields(SVehicle& v)
    {
        bool changed=false;
        changed |= fields(static_cast<SComponent&>(v));
        changed |= field("Vehicle axis", v.vehicleAxis);
        changed |= field("Invert forward", v.invertForward);
        changed |= field("Wheels", v.wheels);
        return changed;
    }
    inline bool fields(SVideoPlayer& v)
    {
        bool changed=false;
        changed |= fields(static_cast<SComponent&>(v));
        changed |= field("Video clip", v.videoClip);
        changed |= field("Play on start", v.playOnStart);
        changed |= field("Loop", v.loop);
        return changed;
    }
    inline bool fields(SWater& v)
    {
        bool changed=false;
        changed |= fields(static_cast<SComponent&>(v));
        changed |= field("Size", v.size);
        changed |= field("Reflections", v.reflections);
        changed |= field("Material", v.material);
        changed |= field("Reflect skybox", v.reflectSkybox);
        changed |= field("Reflect objects", v.reflectObjects);
        changed |= field("Reflections distance", v.reflectionsDistance);
        changed |= field("Reflections quality", v.reflectionsQuality);
        return changed;
    }
}
