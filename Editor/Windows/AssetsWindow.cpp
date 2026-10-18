#include "AssetsWindow.h"
#include "../Classes/AssetWindowGlyphs.h"

#include <imgui.h>
#include <imgui_internal.h>
#include <dear-imgui/misc/cpp/imgui_stdlib.h>
#include <thread>
#include <filesystem>
#include <stdexcept>
#include <algorithm>
#include <cctype>

#include "MainWindow.h"
#include "PrefabEditorWindow.h"
#include "InspectorWindow.h"
#include "ConsoleWindow.h"
#include "AnimationEditorWindow.h"

#include "../PropertyEditors/MaterialEditor.h"
#include "../PropertyEditors/CubemapEditor.h"
#include "../PropertyEditors/TextureEditor.h"
#include "../PropertyEditors/Model3DEditor.h"
#include "../PropertyEditors/AudioClipEditor.h"
#include "../PropertyEditors/VideoClipEditor.h"
#include "../Dialogs/DialogAnimationCrop.h"

#include "../Engine/Core/Engine.h"
#include "../Engine/Core/APIManager.h"
#include "../Engine/Core/Debug.h"
#include "../Engine/Renderer/BatchedGeometry.h"
#include "../Classes/TreeView.h"
#include "../Classes/TreeNode.h"
#include "../Engine/Assets/Scene.h"
#include "../Engine/Assets/Texture.h"
#include "../Engine/Assets/Shader.h"
#include "../Engine/Assets/Cubemap.h"
#include "../Engine/Assets/Model3DLoader.h"
#include "../Engine/Assets/AnimationClip.h"
#include "../Engine/Assets/Material.h"
#include "../Engine/Assets/Mesh.h"
#include "../Engine/Assets/AudioClip.h"
#include "../Engine/Assets/VideoClip.h"
#include "../Engine/Assets/Prefab.h"
#include "../Engine/Classes/Helpers.h"
#include "../Engine/Classes/GUIDGenerator.h"
#include "../Classes/SolutionWorker.h"

#ifndef FREEIMAGE_LIB
#define FREEIMAGE_LIB
#include <FreeImage.h>
#undef FREEIMAGE_LIB
#endif

#ifdef _WIN32
#include <shellapi.h>
#include <ShlObj_core.h>
#else
#include <boost/process.hpp>
#include <sys/types.h>
#include <unistd.h>
#include <sys/inotify.h>
#include <limits.h>
#define EVENT_SIZE  ( sizeof (struct inotify_event) )
#define BUF_LEN     ( 1024 * ( EVENT_SIZE + 255 ) )
#define NOTIFY_FLAGS IN_MODIFY | IN_CREATE | IN_DELETE | IN_MOVED_TO | IN_MOVED_FROM | IN_CLOSE_WRITE
#endif

namespace GX
{
	AssetsWindow* AssetsWindow::singleton = nullptr;
	std::atomic<bool> AssetsWindow::scriptsChanged{ false };
	std::mutex AssetsWindow::changedFilesMutex;
	std::vector<std::string> AssetsWindow::changedFiles;

	AssetsWindow::AssetsWindow()
	{
		singleton = this;

		treeView = new TreeView();
		treeView->setTag("Assets");
		//treeView->setAllowMoveNodes(true);
		treeView->setProcessDragDropInTarget(true);
		treeView->setSelectCallback(onNodesSelected);

		treeView->getRootNode()->setSupportedFormats({ "*.", "::GameObject" });
		treeView->getRootNode()->setOnDropCallback([=](TreeNode* node, TreeNode* from) { onDropNode(node, from); });
	}

	AssetsWindow::~AssetsWindow()
	{
		fswatcher->ExitInstance();
		
		delete fswatcher;
		delete treeView;
	}

	void AssetsWindow::init()
	{
		addIcon = Texture::load(Helper::ExePath(), "Editor/Icons/Toolbar/add.png", false, Texture::CompressionMethod::None, true);
		addFolderIcon = Texture::load(Helper::ExePath(), "Editor/Icons/Toolbar/add_folder.png", false, Texture::CompressionMethod::None, true);
		closeIcon = Texture::load(Helper::ExePath(), "Editor/Icons/Hierarchy/close.png", false, Texture::CompressionMethod::None, true);

		dialogAnimationCrop = new DialogAnimationCrop();

		folderIcon = MainWindow::loadEditorIcon("Assets/folder.png");
		meshIcon = MainWindow::loadEditorIcon("Assets/mesh.png");
		textureIcon = MainWindow::loadEditorIcon("Assets/texture.png");
		materialIcon = MainWindow::loadEditorIcon("Assets/material.png");
		sceneIcon = MainWindow::loadEditorIcon("Assets/scene.png");
		shaderIcon = MainWindow::loadEditorIcon("Assets/shader.png");
		cubemapIcon = MainWindow::loadEditorIcon("Assets/cubemap.png");
		csIcon = MainWindow::loadEditorIcon("Assets/cs.png");
		audioClipIcon = MainWindow::loadEditorIcon("Assets/audio_clip.png");
		fontIcon = MainWindow::loadEditorIcon("Assets/font.png");
		prefabIcon = MainWindow::loadEditorIcon("Assets/prefab.png");
		textIcon = MainWindow::loadEditorIcon("Assets/text.png");
		animationClipIcon = MainWindow::loadEditorIcon("Assets/animation_clip.png");
		terrainIcon = MainWindow::loadEditorIcon("Inspector/terrain.png");
		packageIcon = MainWindow::loadEditorIcon("Other/package.png");
		navmeshIcon = MainWindow::loadEditorIcon("Toolbar/navmesh.png");
		videoClipIcon = MainWindow::loadEditorIcon("Assets/video_clip.png");

		renameIcon = MainWindow::loadEditorIcon("Menu/rename.png");
		deleteIcon = MainWindow::loadEditorIcon("Menu/delete_object.png");
		showInExplorerIcon = MainWindow::loadEditorIcon("Menu/explorer.png");
		reloadIcon = MainWindow::loadEditorIcon("Menu/reload.png");
		extractAnimsIcon = MainWindow::loadEditorIcon("Menu/extract_anim.png");
		extractAudioIcon = MainWindow::loadEditorIcon("Menu/extract_audio.png");
		cropAnimIcon = MainWindow::loadEditorIcon("Menu/crop_anim.png");

		reloadFiles();

		fswatcher = new FileSystemWatcher();
		fswatcher->InitInstance();
	}

    void AssetsWindow::update()
    {
        focused = false;
        updateBrowser();
        importExternalFiles();
        dialogAnimationCrop->update();
    }

	void AssetsWindow::focusOnFile(std::string filepath)
	{
		TreeNode* node = treeView->getNodeByPath(filepath, treeView->getRootNode());
		if (node != nullptr)
		{
			treeView->selectNodes({ node->name }, false);
			treeView->focusOnNode(node);
            navigateBrowser(isDirectory(node) ? node->getPath() : node->parent->getPath());
            browserSearchAll = false;
            browserOpened = true;
            browserFocusId = node->name;
		}
	}

	Texture* AssetsWindow::getIconByExtension(std::string ext)
	{
		Texture * icon = Texture::load(Helper::ExePath(), "Editor/Icons/Assets/file.png", false, Texture::CompressionMethod::None, true);

		std::vector<std::string> imgs = Engine::getImageFileFormats();
		std::vector<std::string> models = Engine::getModel3dFileFormats();
		std::vector<std::string> sounds = Engine::getAudioFileFormats();
		std::vector<std::string> videos = Engine::getVideoFileFormats();

		if (ext.empty())
			icon = folderIcon;
		if (std::find(models.begin(), models.end(), ext) != models.end())
			icon = meshIcon;
		if (std::find(imgs.begin(), imgs.end(), ext) != imgs.end())
			icon = textureIcon;
		if (std::find(sounds.begin(), sounds.end(), ext) != sounds.end())
			icon = audioClipIcon;
		if (std::find(videos.begin(), videos.end(), ext) != videos.end())
			icon = videoClipIcon;
		if (ext == "material")
			icon = materialIcon;
		if (ext == "scene")
			icon = sceneIcon;
		if (ext == "shader")
			icon = shaderIcon;
		if (ext == "cubemap")
			icon = cubemapIcon;
		if (ext == "cs")
			icon = csIcon;
		if (ext == "ttf" || ext == "otf")
			icon = fontIcon;
		if (ext == "prefab")
			icon = prefabIcon;
		if (ext == "txt")
			icon = textIcon;
		if (ext == "animation")
			icon = animationClipIcon;
		if (ext == "terrain")
			icon = terrainIcon;
		if (ext == "package")
			icon = packageIcon;
		if (ext == "navmesh")
			icon = navmeshIcon;

		return icon;
	}

	void AssetsWindow::sortTree()
	{
		MainWindow::addOnEndUpdateCallback([=]() {
			sortTree(treeView->getRootNode());
		});
	}

	void AssetsWindow::sortTree(TreeNode* root)
	{
		std::vector<TreeNode*> vec1;
		std::vector<TreeNode*> vec2;

		for (auto it = root->children.begin(); it != root->children.end(); ++it)
		{
			std::string _a = Engine::getSingleton()->getAssetsPath() + CP_SYS((*it)->getPath());

			if (IO::isDir(_a))
			{
				vec1.push_back(*it);
			}
			else
			{
				vec2.push_back(*it);
			}
		}

		std::sort(vec1.begin(), vec1.end(), [=](TreeNode*& a, TreeNode*& b) -> bool {
			return boost::algorithm::to_lower_copy(a->alias) < boost::algorithm::to_lower_copy(b->alias);
		});

		std::sort(vec2.begin(), vec2.end(), [=](TreeNode*& a, TreeNode*& b) -> bool {
			return boost::algorithm::to_lower_copy(a->alias) < boost::algorithm::to_lower_copy(b->alias);
		});

		vec1.insert(vec1.end(), vec2.begin(), vec2.end());

		root->children.clear();
		root->children = vec1;

		vec1.clear();
		vec2.clear();

		for (auto it = root->children.begin(); it != root->children.end(); ++it)
			sortTree(*it);
	}

	void AssetsWindow::reloadFiles()
	{
        browserResultsDirty = true;
        browserResults.clear();
		std::vector<std::string> allFilesNames;
		std::vector<std::string> allFilesNames1;
		std::vector<std::string> addedFilesNames;
		std::vector<std::string> removeFilesNames;

		//Store all items
		rememberAllItems(treeView->getRootNode(), allFilesNames);

		removeFilesNames = addNewFiles(allFilesNames);
		addedFilesNames = removeFilesNames;

		allFilesNames1 = addedFilesNames;

		addedFilesNames.erase(remove_if(begin(addedFilesNames), end(addedFilesNames), [&](auto x)
		{
			return find(begin(allFilesNames), end(allFilesNames), x) != end(allFilesNames);
		}), end(addedFilesNames));

		allFilesNames.erase(remove_if(begin(allFilesNames), end(allFilesNames), [&](auto x)
		{
			return find(begin(removeFilesNames), end(removeFilesNames), x) != end(removeFilesNames);
		}), end(allFilesNames));

		removeFilesNames = allFilesNames;
		allFilesNames.clear();

		if (removeFilesNames.size() > 0)
		{
			MainWindow::getSingleton()->getInspectorWindow()->setEditor(nullptr);

			auto& models = Engine::getModel3dFileFormats();
			auto& images = Engine::getImageFileFormats();

			std::string location = Engine::getSingleton()->getAssetsPath();

			bool rebuildGeom = false;

			for (auto it = removeFilesNames.begin(); it < removeFilesNames.end(); ++it)
			{
				std::string path = *it;
				std::string ext = IO::GetFileExtension(path);

				if (std::find(models.begin(), models.end(), ext) != models.end())
				{
					for (auto& asset : Asset::getLoadedInstances())
					{
						if (asset.second->getAssetType() == Mesh::ASSET_TYPE)
						{
							Mesh* mesh = (Mesh*)asset.second;
							if (mesh->getSourceFile() == path)
							{
								if (IO::FileExists(mesh->getOrigin()))
									IO::FileDelete(mesh->getOrigin());

								mesh->unload();

								rebuildGeom = true;

								break;
							}
						}
					}
				}
				else if (std::find(images.begin(), images.end(), ext) != images.end())
				{
					for (auto& asset : Asset::getLoadedInstances())
					{
						if (asset.second->getAssetType() == Texture::ASSET_TYPE)
						{
							Texture* texture = (Texture*)asset.second;
							if (texture->getName() == path)
							{
								std::string texPath = texture->getCachedFileName();

								if (IO::FileExists(texPath))
									IO::FileDelete(texPath);

								texture->unload();

								break;
							}
						}
					}
				}
				else if (ext == "animation")
				{
					Asset* inst = Asset::getLoadedInstance(location, path);
					if (inst != nullptr)
					{
						AnimationEditorWindow* animEditor = MainWindow::getAnimationEditorWindow();

						if (inst == animEditor->getOpenedAnimation())
						{
							animEditor->closeAnimation();
						}

						if (inst->isLoaded())
							inst->unload();
					}
				}
				else
				{
					Asset* asset = Asset::getLoadedInstance(location, path);
					if (asset != nullptr && asset->isLoaded())
						asset->unload();
				}

				TreeNode* del = treeView->getNodeByPath(CP_UNI(path), treeView->getRootNode());
				if (del != NULL)
				{
					treeView->deleteNode(del);
				}
			}

			if (rebuildGeom)
			{
				MainWindow::getSingleton()->addOnEndUpdateCallback([]()
					{
						BatchedGeometry::getSingleton()->rebuild();
					}
				);
			}
		}

		//Scripts
		if (addedFilesNames.size() > 0 || removeFilesNames.size() > 0)
		{
			addScripts(allFilesNames1);
			reloadShaderList();
		}

		allFilesNames1.clear();
		addedFilesNames.clear();
		removeFilesNames.clear();
	}

	void AssetsWindow::reloadShaderList()
	{
		shaderList.clear();

		std::string assetsPath = Engine::getSingleton()->getAssetsPath();
		std::string builtinPath = Engine::getSingleton()->getBuiltinResourcesPath();

		IO::listFiles(builtinPath, true, nullptr, [=](std::string dir, std::string file) -> bool
			{
				std::string ext = IO::GetFileExtension(file);

				if (ext == "shader")
				{
					std::string fullPath = dir + file;
					std::string filename = IO::RemovePart(fullPath, builtinPath);
					Shader* sh = Shader::load(builtinPath, filename);
					if (sh != nullptr && !sh->getAlias().empty())
						shaderList.push_back(sh);
				}

				return true;
			}
		);

		IO::listFiles(assetsPath, true, nullptr, [=](std::string dir, std::string file) -> bool
			{
				std::string ext = IO::GetFileExtension(file);

				if (ext == "shader")
				{
					std::string fullPath = dir + file;
					std::string filename = IO::RemovePart(fullPath, assetsPath);
					Shader* sh = Shader::load(assetsPath, filename);
					if (sh != nullptr && !sh->getAlias().empty())
						shaderList.push_back(sh);
				}

				return true;
			}
		);
	}

	void AssetsWindow::reloadChangedFiles()
	{
		// Consume before compiling so watcher notifications arriving during the
		// compilation remain pending for the next refresh.
		if (!Engine::getSingleton()->getIsRuntimeMode()
			&& !MainWindow::getSingleton()->isScriptsCompiling()
			&& scriptsChanged.exchange(false))
		{
			APIManager::getSingleton()->compile(APIManager::CompileConfiguration::Debug);
		}

		// The watcher writes from another thread. Transfer ownership under the
		// lock, then reload without holding it: imports may generate more events.
		std::vector<std::string> pendingFiles;
		{
			std::lock_guard<std::mutex> lock(changedFilesMutex);
			pendingFiles.swap(changedFiles);
		}

		bool rebuildGeom = false;

		auto& models = Engine::getModel3dFileFormats();
		for (const auto& it : pendingFiles)
		{
			reloadChangedFile(it);

			if (!rebuildGeom)
			{
				std::string ext = IO::GetFileExtension(it);
				if (std::find(models.begin(), models.end(), ext) != models.end())
					rebuildGeom = true;
			}
		}

		if (rebuildGeom)
		{
			MainWindow::getSingleton()->addOnEndUpdateCallback([]()
				{
					BatchedGeometry::getSingleton()->rebuild();
				}
			);
		}

	}

	void AssetsWindow::reloadChangedFile(std::string path)
	{
		auto& models = Engine::getModel3dFileFormats();
		std::string ext = IO::GetFileExtension(path);

		//If not a 3d model
		if (std::find(models.begin(), models.end(), ext) == models.end())
		{
			Asset* asset = Asset::getLoadedInstance(Engine::getSingleton()->getAssetsPath(), path);
			if (asset != nullptr)
			{
				asset->reload();

				if (asset->getAssetType() == Texture::ASSET_TYPE)
					updateTextureIcon(path, true);
			}
		}
		else
		{
			for (auto& asset : Asset::getLoadedInstances())
			{
				if (asset.second->getAssetType() == Mesh::ASSET_TYPE)
				{
					Mesh* mesh = (Mesh*)asset.second;
					if (mesh->getSourceFile() == path)
					{
						if (IO::FileExists(mesh->getOrigin()))
							IO::FileDelete(mesh->getOrigin());

						mesh->unload();
					}
				}
			}

			Model3DLoader::cache3DModel(Engine::getSingleton()->getAssetsPath(), path);
		}
	}

	void AssetsWindow::addChangedFile(std::string value)
	{
		std::lock_guard<std::mutex> lock(changedFilesMutex);
		if (std::find(changedFiles.begin(), changedFiles.end(), value) == changedFiles.end())
			changedFiles.push_back(value);
	}

	std::vector<std::string> AssetsWindow::addNewFiles(std::vector<std::string> files)
	{
        browserResultsDirty = true;
        browserResults.clear();
		std::vector<std::string> filesNames;
		std::vector<std::string> filesNames1;

		struct LambdaFunc
		{
			std::vector<std::string>* filesNames;

			bool operator() (std::string dir)
			{
				std::string str = IO::RemovePart(dir, Engine::getSingleton()->getAssetsPath());
				filesNames->push_back(str);

				return true;
			}

			bool operator() (std::string dir, std::string filename)
			{
				std::string str = IO::RemovePart(dir + filename, Engine::getSingleton()->getAssetsPath());
				filesNames->push_back(str);

				return true;
			}
		};

		LambdaFunc func;
		func.filesNames = &filesNames;

		IO::listFiles(Engine::getSingleton()->getAssetsPath(), true, func, func);

		filesNames1 = filesNames;

		for (auto it = files.begin(); it != files.end(); ++it)
		{
			auto _it = std::find(filesNames.begin(), filesNames.end(), *it);
			if (_it != filesNames.end())
				filesNames.erase(_it);
		}

		for (auto it = filesNames.begin(); it != filesNames.end(); ++it)
		{
			std::string path = *it;
			if (treeView->getNodeByPath(CP_UNI(path), treeView->getRootNode()) == nullptr)
			{
				std::vector<std::string> results;
				boost::split(results, path, [](char c) { return c == '/' || c == '\\'; });

				TreeNode* item = treeView->getRootNode();

				for (auto it2 = results.begin(); it2 != results.end(); ++it2)
				{
					TreeNode* newItem = treeView->getNodeByAlias(CP_UNI(*it2), item, false);

					if (newItem == nullptr)
					{
						std::string ext = IO::GetFileExtension(*it2);
						
						Texture* icon = getIconByExtension(ext);

						std::vector<std::string>& imgs = Engine::getSingleton()->getImageFileFormats();
						if (std::find(imgs.begin(), imgs.end(), ext) != imgs.end())
						{
							Texture* ic = updateTextureIcon(path);
							if (ic != nullptr)
								icon = ic;
						}

						bool isDir = IO::isDir(Engine::getSingleton()->getAssetsPath() + CP_SYS(item->getPath()) + (item == treeView->getRootNode() ? "" : "/") + *it2);

						if (isDir)
							icon = Texture::load(Helper::ExePath(), "Editor/Icons/Assets/folder.png", false, Texture::CompressionMethod::None, true);

						std::vector<std::string> popup = { "Rename", "Delete", "Show in Explorer", "Reload" };
						std::vector<Texture*> icons = { renameIcon, deleteIcon, showInExplorerIcon, reloadIcon };
						
						auto& extsModels = Engine::getModel3dFileFormats();
						auto& extsVideo = Engine::getVideoFileFormats();

						if (std::find(extsModels.begin(), extsModels.end(), ext) != extsModels.end())
						{
							popup.push_back("-");
							popup.push_back("Extract animations");

							icons.push_back(extractAnimsIcon);
						}
						else if (ext == "animation")
						{
							popup.push_back("-");
							popup.push_back("Crop animation");

							icons.push_back(cropAnimIcon);
						}
						else if (std::find(extsVideo.begin(), extsVideo.end(), ext) != extsVideo.end())
						{
							popup.push_back("-");
							popup.push_back("Extract audio");

							icons.push_back(extractAudioIcon);
						}

						TreeNode* newNode = new TreeNode(treeView);
						newNode->alias = CP_UNI(*it2);
						newNode->name = GUIDGenerator::genGuid();
						newNode->icon = icon;
                        newNode->assetDirectory = isDir;
						newNode->enableDrag = true;
						newNode->format = ext;
						newNode->setOnEndEditCallback([=](TreeNode* node, std::string prevAlias) -> bool { return onNodeRenamed(node, prevAlias); });
						newNode->setPopupMenu(popup, [=](TreeNode* node, int val) { onNodePopupMenu(node, val); });
						newNode->setPopupMenuIcons(icons);
						newNode->setOnDoubleClickCallback([=](TreeNode* node) { onNodeDoubleClick(node); });
						newNode->setCustomPopupMenuProcedure([=](TreeNode* node) { updateAssetsPopup(true, node); });
						newNode->setCustomPopupMenuPosition(TreeNode::CustomPopupMenuPosition::CPMP_BEGIN);

						if (isDir)
						{
							newNode->setSupportedFormats({ "*.", "::GameObject" });
							newNode->setOnDropCallback([=](TreeNode* node, TreeNode* from) { onDropNode(node, from); });
						}

						item->addChild(newNode);
						item = newNode;
					}
					else
					{
						item = newItem;
					}
				}

				results.clear();
			}
		}

		sortTree();

		return filesNames1;
	}

	void AssetsWindow::addScripts(std::vector<std::string> files)
	{
		std::vector<std::string> scripts = files;

		if (files.size() == 0)
			rememberAllItems(treeView->getRootNode(), scripts);

		scripts.erase(remove_if(begin(scripts), end(scripts), [&](auto x)
		{
			return x.find(".cs") == std::string::npos;
		}), end(scripts));

		std::vector<std::string> _scripts;
		for (std::vector<std::string>::iterator it = scripts.begin(); it != scripts.end(); ++it)
		{
			std::string _str = "Assets\\" + IO::ReplaceFrontSlashes(*it);
			_scripts.push_back(_str);
		}

		files.clear();
		scripts.clear();

		SolutionWorker solutionWorker;
		solutionWorker.CreateSolution(Engine::getSingleton()->getRootPath(), Engine::getSingleton()->getAppName(), _scripts);
	}

	void AssetsWindow::rememberExpandedItems(TreeNode* root)
	{
		if (root->expanded)
			expandedItems.push_back(treeView->getNodePath(root));

		for (auto it = root->children.begin(); it != root->children.end(); ++it)
			rememberExpandedItems(*it);
	}

	void AssetsWindow::rememberAllItems(TreeNode* root, std::vector<std::string>& list)
	{
		std::string fName = CP_SYS(treeView->getNodePath(root));

		if (fName != "")
			list.push_back(fName);

		for (auto it = root->children.begin(); it != root->children.end(); ++it)
			rememberAllItems(*it, list);
	}

	void AssetsWindow::createFolder(TreeNode* selNode)
	{
		if (treeView->getSelectedNodes().size() <= 1)
		{
			TreeNode* root = treeView->getRootNode();

			if (selNode != nullptr)
				root = selNode;
			else
			{
				auto nodes = treeView->getSelectedNodes();

				if (nodes.size() == 1)
					root = nodes[0];
			}

			root->expanded = true;

			if (!IO::isDir(Engine::getSingleton()->getAssetsPath() + CP_SYS(root->getPath())))
			{
				root = root->parent;
			}

			TreeNode * addedAssetNode = new TreeNode(treeView);
			addedAssetNode->name = "##" + GUIDGenerator::genGuid();
			addedAssetNode->icon = getIconByExtension("");
            addedAssetNode->assetDirectory = true;
			addedAssetNode->setEditMode(true);
			addedAssetNode->setOnEndEditCallback([=](TreeNode* node, std::string prevAlias) -> bool {
				std::string name = CP_SYS(node->alias);
				std::string _path = CP_SYS(node->getPath());

				if (name.find("/") != std::string::npos || name.find("\\") != std::string::npos)
					name = "";

				if (!name.empty() && !IO::DirExists(Engine::getSingleton()->getAssetsPath() + _path))
				{
					IO::CreateDir(Engine::getSingleton()->getAssetsPath() + _path);

					MainWindow::addOnEndUpdateCallback([=]()
						{
							treeView->deleteNode(addedAssetNode);
							reloadFiles();
						}
					);
				}
				else
				{
					MainWindow::addOnEndUpdateCallback([=]()
						{
							treeView->deleteNode(addedAssetNode);
						}
					);
				}

				return true;
				}
			);

			root->addChild(addedAssetNode);

			treeView->focusOnNode(addedAssetNode);
		}
	}

	Texture* AssetsWindow::updateTextureIcon(std::string path, bool recreate)
	{
		Texture* icon = nullptr;

		std::string iconCacheDir = Engine::getSingleton()->getRootPath() + "Temp/IconCache/";
		std::string fullPath = Engine::getSingleton()->getAssetsPath() + path;

		if (IO::FileExists(fullPath))
		{
			std::string iconName = IO::GetFileName(path) + ".png";
			std::string outDir = iconCacheDir + IO::GetFilePath(path);
			std::string outPath = outDir + iconName;

			if (!IO::DirExists(IO::GetFilePath(outDir)))
				IO::CreateDir(outDir, true);

			if (recreate)
			{
				if (IO::FileExists(outPath))
					IO::FileDelete(outPath);
			}

			if (!IO::FileExists(outPath))
			{
				FREE_IMAGE_FORMAT formato = FreeImage_GetFileType(fullPath.c_str(), 0);
				FIBITMAP* imagen = FreeImage_Load(formato, fullPath.c_str());
				if (FreeImage_GetBPP(imagen) != 32)
				{
					FIBITMAP* convert = FreeImage_ConvertTo32Bits(imagen);
					FreeImage_Unload(imagen);
					imagen = convert;
				}

				FIBITMAP* scaled = FreeImage_Rescale(imagen, 16, 16, FREE_IMAGE_FILTER::FILTER_BOX);
				FreeImage_Unload(imagen);
				imagen = scaled;

				FreeImage_Save(FREE_IMAGE_FORMAT::FIF_PNG, imagen, CP_UNI(outPath).c_str());
				FreeImage_Unload(imagen);
			}

			if (IO::FileExists(outPath))
			{
				if (recreate)
				{
					Asset* asset = Asset::getLoadedInstance(outDir, iconName);
					if (asset != nullptr)
					{
						asset->setPersistent(false);
						asset->unload();
					}
				}

				icon = Texture::load(outDir, iconName, false, Texture::CompressionMethod::None, true, false);
			}
		}

		return icon;
	}

	void AssetsWindow::onNodesSelected(std::vector<std::string> names, void* userData)
	{
		if (names.size() > 0)
		{
			TreeNode* node = singleton->treeView->getNodeByName(names[0]);
			if (node == nullptr)
				return;

			std::string fileName = node->getPath();
			std::string ext = IO::GetFileExtension(fileName);
			auto& imgs = Engine::getImageFileFormats();
			auto& models = Engine::getModel3dFileFormats();
			auto& audios = Engine::getAudioFileFormats();
			auto& videos = Engine::getVideoFileFormats();

			if (ext == "material")
			{
				Material* mat = Material::load(Engine::getSingleton()->getAssetsPath(), fileName);

				if (mat != nullptr)
				{
					MaterialEditor* ed = new MaterialEditor();
					ed->init(mat, true);

					MainWindow::getInspectorWindow()->setEditor(ed);
				}
			}

			if (ext == "cubemap")
			{
				Cubemap* cubemap = Cubemap::load(Engine::getSingleton()->getAssetsPath(), fileName);

				if (cubemap != nullptr)
				{
					CubemapEditor* ed = new CubemapEditor();
					ed->init(cubemap);

					MainWindow::getInspectorWindow()->setEditor(ed);
				}
			}

			if (std::find(imgs.begin(), imgs.end(), ext) != imgs.end())
			{
				std::vector<Texture*> textures;
				for (auto& it : names)
				{
					TreeNode* n = singleton->treeView->getNodeByName(it);
					if (n != nullptr)
					{
						Texture* texture = Texture::load(Engine::getSingleton()->getAssetsPath(), n->getPath(), true, Texture::CompressionMethod::Default);
						if (texture != nullptr && texture->isLoaded())
							textures.push_back(texture);
					}
				}

				TextureEditor* ed = new TextureEditor();
				ed->init(textures);
				textures.clear();

				MainWindow::getInspectorWindow()->setEditor(ed);
			}

			if (std::find(models.begin(), models.end(), ext) != models.end() || ext == "prefab")
			{
				std::vector<std::string> paths;
				for (auto& it : names)
				{
					TreeNode* n = singleton->treeView->getNodeByName(it);
					if (n != nullptr)
						paths.push_back(n->getPath());
				}

				Model3DEditor* ed = new Model3DEditor();
				ed->init(paths);

				MainWindow::getInspectorWindow()->setEditor(ed);

				singleton->reloadFiles();
			}

			if (std::find(audios.begin(), audios.end(), ext) != audios.end())
			{
				AudioClip* audio = AudioClip::load(Engine::getSingleton()->getAssetsPath(), fileName);

				if (audio != nullptr)
				{
					AudioClipEditor* ed = new AudioClipEditor();
					ed->init(audio);

					MainWindow::getInspectorWindow()->setEditor(ed);
				}
			}

			if (std::find(videos.begin(), videos.end(), ext) != videos.end())
			{
				VideoClip* video = VideoClip::load(Engine::getSingleton()->getAssetsPath(), fileName);

				if (video != nullptr)
				{
					VideoClipEditor* ed = new VideoClipEditor();
					ed->init(video);

					MainWindow::getInspectorWindow()->setEditor(ed);
				}
			}
		}
	}

	void AssetsWindow::onDropNode(TreeNode* node, TreeNode* from)
	{
		if (from->treeView == treeView)
		{
			auto items = treeView->getSelectedNodes();
			std::vector<std::string> delItems;
			std::vector<std::string> moveItems;

			for (auto& item : items)
			{
				std::string path = Engine::getSingleton()->getAssetsPath() + item->getPath();
				if (IO::isDir(path))
				{
					for (auto& item2 : items)
					{
						std::string p = item2->getPath();
						std::string path2 = Engine::getSingleton()->getAssetsPath() + p;
						if (path != path2)
						{
							if (path2.find(path) != std::string::npos)
								delItems.push_back(p);
						}
					}
				}
			}

			for (auto& item : items)
			{
				std::string path = item->getPath();
				auto it = std::find(delItems.begin(), delItems.end(), path);
				if (it == delItems.end())
					moveItems.push_back(path);
			}

			delItems.clear();

			for (auto& item : moveItems)
			{
				std::string path = Engine::getSingleton()->getAssetsPath() + item;
				std::string name = IO::GetFileNameWithExt(item);
				std::string nodePath = node->getPath();

				if (!nodePath.empty())
					nodePath += "/";

				std::string newPath = Engine::getSingleton()->getAssetsPath() + nodePath + name;

				std::string srchOldPath = path;
				if (IO::GetFileExtension(srchOldPath).empty())
					srchOldPath += "/";

				std::string srchNewPath = newPath;
				if (IO::GetFileExtension(srchNewPath).empty())
					srchNewPath += "/";

				if (srchNewPath.find(srchOldPath) != std::string::npos)
					continue;

				if (newPath != path)
				{
					if (IO::isDir(path))
					{
						if (!IO::DirExists(newPath))
						{
							IO::DirCopy(path, newPath);
							IO::DirDeleteRecursive(path);
						}
					}
					else
					{
						if (!IO::FileExists(newPath))
						{
							IO::FileCopy(path, newPath);
							IO::FileDelete(path);
						}
					}
				}
			}

			treeView->selectNodes({}, false);

			reloadFiles();
		}
		else
		{
			if (from->treeView->getTag() == "Hierarchy")
			{
				std::string nodePath = "";
				if (node != nullptr)
					nodePath = CP_SYS(node->getPath());

				if (!nodePath.empty())
					nodePath += "/";

				std::string fpath = Engine::getSingleton()->getAssetsPath() + nodePath;

				if (IO::DirExists(fpath))
				{
					GameObject* prefabNode = Engine::getSingleton()->getGameObject(from->name);

					std::string prefabLocation = Engine::getSingleton()->getAssetsPath();
					std::string prefabName = nodePath + from->alias + ".prefab";

					if (IO::FileExists(prefabLocation + prefabName))
					{
						Prefab* loadedPrefab = Prefab::load(prefabLocation, prefabName);
						if (loadedPrefab != nullptr && loadedPrefab->isLoaded())
							loadedPrefab->unload();
					}

					Scene::savePrefab(prefabLocation, prefabName, prefabNode);

					addNewFiles();
				}
			}
		}
	}

	bool AssetsWindow::onNodeRenamed(TreeNode* item, std::string oldName)
	{
        browserResultsDirty = true;
        browserResults.clear();
		std::string newName = Engine::getSingleton()->getAssetsPath() + item->getPath();
		std::string path = IO::GetFilePath(newName);
		std::string prevName = path + oldName;

		if (IO::FileExists(newName))
			return false;

		boost::system::error_code err = IO::FileRename(prevName, newName);
		
		if (err.value() != 0)
		{
			std::string errMsg = err.message();
	#ifdef _WIN32
			MessageBoxW(0, StringConvert::s2ws(errMsg).c_str(), L"Error", MB_OK);
	#else
			std::string cmd = "zenity --icon-name='dialog-warning' --no-wrap --info --title='Error' --text='" + errMsg + "'";
			FILE *ff = popen(cmd.c_str(), "r");
			pclose(ff);
	#endif
			return false;
		}

		return true;
	}

	void AssetsWindow::onNodePopupMenu(TreeNode* node, int val)
	{
		if (val == 0)
		{
			if (treeView->getSelectedNodes().size() <= 1)
			{
				node->setEditMode(true);
			}
		}

		if (val == 1)
		{
			MainWindow::getSingleton()->getInspectorWindow()->setEditor(nullptr);

			std::vector<TreeNode*>& nodes = treeView->getSelectedNodes();

			if (nodes.size() == 0)
				nodes = { node };
			else if (find(nodes.begin(), nodes.end(), node) == nodes.end())
				nodes = { node };

			bool recompileScripts = false;

			for (auto it = nodes.begin(); it != nodes.end(); ++it)
			{
				TreeNode* curNode = *it;
				TreeNode* par = curNode->parent;
				if (find(nodes.begin(), nodes.end(), par) != nodes.end())
					continue;

				std::string location = Engine::getSingleton()->getAssetsPath();

				std::string path = CP_SYS(curNode->getPath());
				std::string fileName = location + path;

				if (fileName != location)
				{
					if (IO::isDir(fileName))
					{
						IO::listFiles(fileName + "/", true, nullptr, [&recompileScripts](std::string str1, std::string str2) -> bool
							{
								std::string _fn = str1 + str2;
								if (IO::GetFileExtension(_fn) == "cs")
								{
									recompileScripts = true;

									return false;
								}

								return true;
							}
						);
					}
					else
					{
						if (IO::GetFileExtension(fileName) == "cs")
							recompileScripts = true;
					}
	#ifdef _WIN32
					//Delete file
					std::string str1 = fileName;
					fileName = boost::replace_all_copy(fileName, "/", "\\");
					std::wstring wstr = StringConvert::s2ws(fileName);
					wstr.append(L"00");
					wstr[wstr.size() - 2] = '\0';
					wstr[wstr.size() - 1] = '\0';
					const wchar_t * _strfn = wstr.c_str();

					SHFILEOPSTRUCTW f = { 0 };
					ZeroMemory(&f, sizeof(SHFILEOPSTRUCTW));
					f.hwnd = MainWindow::getSingleton()->getHwnd();
					f.wFunc = FO_DELETE;
					f.fFlags = FOF_ALLOWUNDO | FOF_SILENT | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_NOCONFIRMMKDIR;
					f.pFrom = _strfn;
					f.pTo = NULL;
					f.fAnyOperationsAborted = false;
					f.hNameMappings = NULL;
					f.lpszProgressTitle = NULL;
					f.hNameMappings = NULL;

					int result = SHFileOperationW(&f);

					if (IO::FileExists(str1))
						MainWindow::getSingleton()->getConsoleWindow()->log("Failed to delete asset(s)", LogMessageType::LMT_ERROR);

					//Delete resource
					//deleteResource(path, true);
					//deleteNodes.push_back(curNode);
	#else
					std::string _path = Engine::getSingleton()->getAssetsPath() + curNode->getPath();
					std::string cmd = "gio trash \"" + _path + "\"";

					boost::process::child c(cmd);
					c.wait();
	#endif
				}
			}

			MainWindow::addOnEndUpdateCallback([=]()
				{
					setScriptsChanged(false);
					reloadFiles();

					if (recompileScripts)
						APIManager::getSingleton()->compile(APIManager::CompileConfiguration::Debug);
				}
			);
			
			nodes.clear();
		}

		if (val == 2)
		{
			auto nodes = treeView->getSelectedNodes();

			if (nodes.size() == 0)
				nodes = { node };
			else if (find(nodes.begin(), nodes.end(), node) == nodes.end())
				nodes = { node };

			for (auto it = nodes.begin(); it != nodes.end(); ++it)
			{
				TreeNode* node = *it;

	#ifdef _WIN32
				std::string _path = boost::replace_all_copy(Engine::getSingleton()->getAssetsPath() + node->getPath(), "/", "\\");

				ITEMIDLIST* pidl = ILCreateFromPathW(StringConvert::s2ws(_path).c_str());
				if (pidl)
				{
					SHOpenFolderAndSelectItems(pidl, 0, 0, 0);
					ILFree(pidl);
				}
	#else
				std::string _path = Engine::getSingleton()->getAssetsPath() + node->getPath();
				std::string cmd = "nautilus \"" + _path + "\"";

				boost::process::child c(cmd);
				c.detach();
	#endif
			}
		}

		if (val == 3)
		{
			MainWindow::addOnEndUpdateCallback([=]()
				{
					reloadChangedFile(node->getPath());

					auto& models = Engine::getModel3dFileFormats();
					std::string ext = IO::GetFileExtension(node->getPath());
					if (std::find(models.begin(), models.end(), ext) != models.end())
					{
						BatchedGeometry::getSingleton()->rebuild();
					}
				}
			);
		}

		if (val == 4)
		{
			auto& modelExts = Engine::getModel3dFileFormats();
			auto& videoExts = Engine::getVideoFileFormats();
			std::string ext = IO::GetFileExtension(node->alias);
			if (std::find(modelExts.begin(), modelExts.end(), ext) != modelExts.end()) // Extract animations
			{
				Model3DLoader::extractAnimations(Engine::getSingleton()->getAssetsPath(), node->getPath());
				MainWindow::addOnEndUpdateCallback([=]()
					{
						reloadFiles();
					}
				);
			}
			else if (IO::GetFileExtension(node->alias) == "animation") // Crop animation
			{
				AnimationClip* animClip = AnimationClip::load(Engine::getSingleton()->getAssetsPath(), node->getPath());
				dialogAnimationCrop->show(animClip, [=]()
					{
						MainWindow::addOnEndUpdateCallback([=]()
							{
								reloadFiles();
							}
						);
					}
				);
			}
			else if (std::find(videoExts.begin(), videoExts.end(), ext) != videoExts.end()) // Extract audio
			{
				VideoClip* clip = VideoClip::load(Engine::getSingleton()->getAssetsPath(), node->getPath());
				if (clip != nullptr && clip->isLoaded())
					clip->extractAudio();

				MainWindow::addOnEndUpdateCallback([=]()
					{
						reloadFiles();
					}
				);
			}
		}
	}

	void AssetsWindow::onNodeDoubleClick(TreeNode* node)
	{
        if (IO::GetFileExtension(node->getPath()) == "prefab")
        {
            const std::string prefabPath = node->getPath();
            MainWindow::addOnEndUpdateCallback([prefabPath]() { PrefabEditorWindow::open(prefabPath); });
            return;
        }
		std::string path = node->getPath();
		std::string ext = IO::GetFileExtension(path);

		MainWindow* mainWnd = MainWindow::getSingleton();

		if (ext == "scene")
		{
			if (mainWnd->checkSceneIsDirty())
				mainWnd->openScene(path);
		}

		if (ext == "cs")
		{
	#ifdef _WIN32
			std::string slnDir = Engine::getSingleton()->getRootPath() + Engine::getSingleton()->getAppName();
			std::string cmd = "\"" + slnDir + ".sln\" \"" + slnDir + ".csproj\" \"" + Engine::getSingleton()->getAssetsPath() + CP_SYS(path) + "\"";
			::ShellExecuteW(NULL, L"open", L"devenv", StringConvert::s2ws(cmd).c_str(), NULL, 1);
	#else
			std::string cmd = "gedit \"" + Engine::getSingleton()->getAssetsPath() + path + "\"";

			boost::process::child c(cmd);
			c.detach();
	#endif
		}

		if (ext == "animation")
		{
			AnimationClip* anim = AnimationClip::load(Engine::getSingleton()->getAssetsPath(), node->getPath());

			if (anim != nullptr && anim->isLoaded())
			{
				AnimationEditorWindow* animEditor = MainWindow::getAnimationEditorWindow();
				animEditor->openAnimation(anim);
			}
		}
	}

	void AssetsWindow::updateAssetsPopup(bool itemsOnly, TreeNode* selNode)
	{
		bool contextMenu = false;

		if (!itemsOnly)
			contextMenu = ImGui::BeginPopupContextItem("add_asset_popup", 0);
		else
		{
			MainWindow::MenuItemIcon(addIcon);
			contextMenu = ImGui::BeginMenu("Create");
		}

		if (contextMenu)
		{
			MainWindow::MenuItemIcon(folderIcon);
			if (ImGui::Selectable("Folder", false))
				createFolder(selNode);

			ImGui::Separator();

			MainWindow::MenuItemIcon(csIcon);
			if (ImGui::Selectable("C# Script", false))
			{
				if (treeView->getSelectedNodes().size() <= 1)
				{
					TreeNode* root = treeView->getRootNode();

					if (selNode != nullptr)
						root = selNode;
					else
					{
						auto nodes = treeView->getSelectedNodes();

						if (nodes.size() == 1)
							root = nodes[0];
					}

					root->expanded = true;

					if (!IO::isDir(Engine::getSingleton()->getAssetsPath() + CP_SYS(root->getPath())))
					{
						root = root->parent;
					}

					TreeNode * addedAssetNode = new TreeNode(treeView);
					addedAssetNode->name = "##" + GUIDGenerator::genGuid();
					addedAssetNode->icon = getIconByExtension("cs");
					addedAssetNode->setEditMode(true);
					
					addedAssetNode->setOnEndEditCallback([=](TreeNode* node, std::string prevAlias) -> bool
						{
							std::string name = CP_SYS(node->alias);

							std::string _path = CP_SYS(root->getPath());
							if (!_path.empty()) _path = _path + "/";

							if (!name.empty() && !IO::FileExists(Engine::getSingleton()->getAssetsPath() + _path + name + ".cs"))
							{
								std::string _name = boost::replace_all_copy(name, " ", "");

								std::string content =
									"using FalcoEngine;\n"
									"using System;\n"
									"using System.Collections;\n"
									"using System.Collections.Generic;\n"
									"\n"
									"public class " + _name + " : MonoBehaviour" + "\n"
									"{\n"
									"	void Start()\n"
									"	{\n"
									"		\n"
									"	}\n"
									"	\n"
									"	void Update()\n"
									"	{\n"
									"		\n"
									"	}\n"
									"}\n";

								IO::WriteText(Engine::getSingleton()->getAssetsPath() + _path + name + ".cs", CP_UNI(content));

								MainWindow::addOnEndUpdateCallback([=]()
									{
										treeView->deleteNode(addedAssetNode);
										setScriptsChanged(false);
										reloadFiles();
										APIManager::getSingleton()->compile(APIManager::CompileConfiguration::Debug);
									}
								);
							}
							else
							{
								MainWindow::addOnEndUpdateCallback([=]()
									{
										treeView->deleteNode(addedAssetNode);
									}
								);
							}

							return true;
						}
					);

					root->addChild(addedAssetNode);

					treeView->focusOnNode(addedAssetNode);
				}
			}

			ImGui::Separator();

			MainWindow::MenuItemIcon(materialIcon);
			if (ImGui::Selectable("Material"))
			{
				if (treeView->getSelectedNodes().size() <= 1)
				{
					TreeNode* root = treeView->getRootNode();

					if (selNode != nullptr)
						root = selNode;
					else
					{
						auto nodes = treeView->getSelectedNodes();

						if (nodes.size() == 1)
							root = nodes[0];
					}

					root->expanded = true;

					if (!IO::isDir(Engine::getSingleton()->getAssetsPath() + CP_SYS(root->getPath())))
					{
						root = root->parent;
					}

					TreeNode* addedAssetNode = new TreeNode(treeView);
					addedAssetNode->name = "##" + GUIDGenerator::genGuid();
					addedAssetNode->icon = getIconByExtension("material");
					addedAssetNode->setEditMode(true);

					addedAssetNode->setOnEndEditCallback([=](TreeNode* node, std::string prevAlias) -> bool
						{
							std::string name = CP_SYS(node->alias) + ".material";

							std::string _path = CP_SYS(root->getPath());
							if (!_path.empty()) _path = _path + "/";

							if (!node->alias.empty() && !IO::FileExists(Engine::getSingleton()->getAssetsPath() + _path + name))
							{
								Material* newMaterial = Material::create(Engine::getSingleton()->getAssetsPath(), _path + name);
								newMaterial->load();
								newMaterial->save();

								MainWindow::addOnEndUpdateCallback([=]()
									{
										treeView->deleteNode(addedAssetNode);
										reloadFiles();
									}
								);
							}
							else
							{
								MainWindow::addOnEndUpdateCallback([=]()
									{
										treeView->deleteNode(addedAssetNode);
									}
								);
							}

							return true;
						}
					);

					root->addChild(addedAssetNode);

					treeView->focusOnNode(addedAssetNode);
				}
			}

			MainWindow::MenuItemIcon(cubemapIcon);
			if (ImGui::Selectable("Cubemap", false))
			{
				if (treeView->getSelectedNodes().size() <= 1)
				{
					TreeNode* root = treeView->getRootNode();

					if (selNode != nullptr)
						root = selNode;
					else
					{
						auto nodes = treeView->getSelectedNodes();

						if (nodes.size() == 1)
							root = nodes[0];
					}

					root->expanded = true;

					if (!IO::isDir(Engine::getSingleton()->getAssetsPath() + CP_SYS(root->getPath())))
					{
						root = root->parent;
					}

					TreeNode* addedAssetNode = new TreeNode(treeView);
					addedAssetNode->name = "##" + GUIDGenerator::genGuid();
					addedAssetNode->icon = getIconByExtension("cubemap");
					addedAssetNode->setEditMode(true);

					addedAssetNode->setOnEndEditCallback([=](TreeNode* node, std::string prevAlias) -> bool
						{
							std::string name = CP_SYS(node->alias) + ".cubemap";

							std::string _path = CP_SYS(root->getPath());
							if (!_path.empty()) _path = _path + "/";

							if (!node->alias.empty() && !IO::FileExists(Engine::getSingleton()->getAssetsPath() + _path + name))
							{
								Cubemap* newCubemap = Cubemap::create(Engine::getSingleton()->getAssetsPath(), _path + name);
								newCubemap->save();

								MainWindow::addOnEndUpdateCallback([=]()
									{
										treeView->deleteNode(addedAssetNode);
										reloadFiles();
									}
								);
							}
							else
							{
								MainWindow::addOnEndUpdateCallback([=]()
									{
										treeView->deleteNode(addedAssetNode);
									}
								);
							}

							return true;
						}
					);

					root->addChild(addedAssetNode);

					treeView->focusOnNode(addedAssetNode);
				}
			}

			MainWindow::MenuItemIcon(shaderIcon);
			if (ImGui::Selectable("Shader", false))
			{
				if (treeView->getSelectedNodes().size() <= 1)
				{
					TreeNode* root = treeView->getRootNode();

					if (selNode != nullptr)
						root = selNode;
					else
					{
						auto nodes = treeView->getSelectedNodes();

						if (nodes.size() == 1)
							root = nodes[0];
					}

					root->expanded = true;

					if (!IO::isDir(Engine::getSingleton()->getAssetsPath() + CP_SYS(root->getPath())))
					{
						root = root->parent;
					}

					TreeNode* addedAssetNode = new TreeNode(treeView);
					addedAssetNode->name = "##" + GUIDGenerator::genGuid();
					addedAssetNode->icon = getIconByExtension("shader");
					addedAssetNode->setEditMode(true);

					addedAssetNode->setOnEndEditCallback([=](TreeNode* node, std::string prevAlias) -> bool
						{
							std::string name = CP_SYS(node->alias);

							std::string _path = CP_SYS(root->getPath());
							if (!_path.empty()) _path = _path + "/";

							if (!node->alias.empty() && !IO::FileExists(Engine::getSingleton()->getAssetsPath() + _path + name + ".shader"))
							{
								std::string _name = boost::replace_all_copy(name, " ", "");

								std::string content =
									"name \"Forward/" + _name + "\"\n"
									"render_mode forward\n"
									"\n"
									"params\n"
									"{\n"
									"	\n"
									"}\n"
									"\n"
									"pass\n"
									"{\n"
									"	tags\n"
									"	{\n"
									"		\n"
									"	}\n"
									"	\n"
									"	varying\n"
									"	{\n"
									"		vec3 a_position  : POSITION;\n"
									"		vec2 a_texcoord0 : TEXCOORD0;\n"
									"		\n"
									"		vec2 v_texcoord0 : TEXCOORD0 = vec2(0.0, 0.0);\n"
									"		vec4 v_color0 : COLOR0 = vec4(1.0, 1.0, 1.0, 1.0);\n"
									"	}\n"
									"	\n"
									"	vertex\n"
									"	{\n"
									"		$input a_position, a_texcoord0\n"
									"		$output v_texcoord0\n"
									"		\n"
									"		#include \"common.sh\"\n"
									"		\n"
									"		void main()\n"
									"		{\n"
									"			gl_Position = mul(u_modelViewProj, vec4(a_position, 1.0));\n"
									"			v_texcoord0 = a_texcoord0;\n"
									"		}\n"
									"	}\n"
									"	\n"
									"	fragment\n"
									"	{\n"
									"		$input v_texcoord0\n"
									"		\n"
									"		#include \"common.sh\"\n"
									"		\n"
									"		void main()\n"
									"		{\n"
									"			gl_FragColor = vec4(1.0, 0.0, 0.0, 1.0);\n"
									"		}\n"
									"	}\n"
									"}\n";

								IO::WriteText(Engine::getSingleton()->getAssetsPath() + _path + name + ".shader", CP_UNI(content));

								Shader* newShader = Shader::create(Engine::getSingleton()->getAssetsPath() + _path, _path + name + ".shader");
								newShader->loadFromSource(content);

								for (auto& asset : Asset::getLoadedInstances())
								{
									if (asset.second->getAssetType() == Material::ASSET_TYPE)
									{
										Material* mat = (Material*)asset.second;
										Shader* ms = mat->getShader();
										if (ms != nullptr && ms->getName() == _path + name + ".shader")
										{
											mat->setShader(nullptr);
											mat->setShader(newShader);
										}
									}
								}

								MainWindow::addOnEndUpdateCallback([=]()
									{
										treeView->deleteNode(addedAssetNode);
										reloadFiles();
									}
								);
							}
							else
							{
								MainWindow::addOnEndUpdateCallback([=]()
									{
										treeView->deleteNode(addedAssetNode);
									}
								);
							}

							return true;
						}
					);

					root->addChild(addedAssetNode);

					treeView->focusOnNode(addedAssetNode);
				}
			}

			ImGui::Separator();

			MainWindow::MenuItemIcon(animationClipIcon);
			if (ImGui::Selectable("Animation", false))
			{
				if (treeView->getSelectedNodes().size() <= 1)
				{
					TreeNode* root = treeView->getRootNode();

					if (selNode != nullptr)
						root = selNode;
					else
					{
						auto nodes = treeView->getSelectedNodes();

						if (nodes.size() == 1)
							root = nodes[0];
					}

					root->expanded = true;

					if (!IO::isDir(Engine::getSingleton()->getAssetsPath() + CP_SYS(root->getPath())))
					{
						root = root->parent;
					}

					TreeNode* addedAssetNode = new TreeNode(treeView);
					addedAssetNode->name = "##" + GUIDGenerator::genGuid();
					addedAssetNode->icon = getIconByExtension("animation");
					addedAssetNode->setEditMode(true);

					addedAssetNode->setOnEndEditCallback([=](TreeNode* node, std::string prevAlias) -> bool
						{
							std::string name = CP_SYS(node->alias);

							std::string _path = CP_SYS(root->getPath());
							if (!_path.empty()) _path = _path + "/";

							if (!node->alias.empty() && !IO::FileExists(Engine::getSingleton()->getAssetsPath() + _path + name))
							{
								AnimationClip* newAnim = AnimationClip::create(Engine::getSingleton()->getAssetsPath(), _path + name);
								newAnim->save();

								MainWindow::addOnEndUpdateCallback([=]()
									{
										treeView->deleteNode(addedAssetNode);
										reloadFiles();
									}
								);
							}
							else
							{
								MainWindow::addOnEndUpdateCallback([=]()
									{
										treeView->deleteNode(addedAssetNode);
									}
								);
							}

							return true;
						}
					);

					root->addChild(addedAssetNode);

					treeView->focusOnNode(addedAssetNode);
				}
			}

			if (!itemsOnly)
				ImGui::EndPopup();
			else
				ImGui::EndMenu();
		}

		if (itemsOnly)
			ImGui::Separator();
	}

	#ifdef _WIN32
	void FileSystemWatcher::WatchDirectory(const wchar_t* path)
	{

		char buf[2048];
		DWORD nRet;
		BOOL result = TRUE;
		char filename[MAX_PATH];
		DirInfo[0].hDir = CreateFileW(path, GENERIC_READ | FILE_LIST_DIRECTORY,
			FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
			NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
			NULL);

		if (DirInfo[0].hDir == INVALID_HANDLE_VALUE)
		{
			return; //cannot open folder
		}

		lstrcpyW(DirInfo[0].lpszDirName, path);
		OVERLAPPED PollingOverlap;

		FILE_NOTIFY_INFORMATION* pNotify;
		int offset;
		PollingOverlap.OffsetHigh = 0;
		PollingOverlap.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
		while (result)
		{
			result = ReadDirectoryChangesW(
				DirInfo[0].hDir,// handle to the directory to be watched
				&buf,// pointer to the buffer to receive the read results
				sizeof(buf),// length of lpBuffer
				TRUE,// flag for monitoring directory or directory tree
				FILE_NOTIFY_CHANGE_FILE_NAME |
				//FILE_NOTIFY_CHANGE_DIR_NAME |
				//FILE_NOTIFY_CHANGE_SIZE |
				FILE_NOTIFY_CHANGE_LAST_WRITE,
				//FILE_NOTIFY_CHANGE_LAST_ACCESS |
				//FILE_NOTIFY_CHANGE_CREATION,
				&nRet,// number of bytes returned
				&PollingOverlap,// pointer to structure needed for overlapped I/O
				NULL);

			WaitForSingleObject(PollingOverlap.hEvent, INFINITE);
			offset = 0;
			int rename = 0;
			char oldName[260];
			char newName[260];
			do
			{
				pNotify = (FILE_NOTIFY_INFORMATION*)((char*)buf + offset);
				strcpy(filename, "");
				int filenamelen = WideCharToMultiByte(CP_ACP, 0, pNotify->FileName, pNotify->FileNameLength / 2, filename, sizeof(filename), NULL, NULL);
				filename[pNotify->FileNameLength / 2];
				std::string fn = std::string(filename, pNotify->FileNameLength / 2);
				fn = IO::ReplaceBackSlashes(fn);

				std::string ext = IO::GetFileExtension(fn);

				switch (pNotify->Action)
				{
				case FILE_ACTION_ADDED:
					if (ext == "cs" || ext.find("~") != std::string::npos)
					{
						AssetsWindow::setScriptsChanged(true);
					}

					break;

				case FILE_ACTION_REMOVED:
					if (ext == "cs" || ext.find("~") != std::string::npos)
					{
						AssetsWindow::setScriptsChanged(true);
					}

					break;

				case FILE_ACTION_MODIFIED:
					if (ext == "cs" || ext.find("~") != std::string::npos)
					{
						AssetsWindow::setScriptsChanged(true);
					}
					else
					{
						AssetsWindow::addChangedFile(fn);
					}

					break;
				case FILE_ACTION_RENAMED_OLD_NAME:
					break;
				case FILE_ACTION_RENAMED_NEW_NAME:
					if (ext != "cs" && ext.find("~") == std::string::npos)
					{
						AssetsWindow::addChangedFile(fn);
					}
					break;
				default:
					break;
				}

				offset += pNotify->NextEntryOffset;

			} while (pNotify->NextEntryOffset);
		}

		CloseHandle(DirInfo[0].hDir);
	}
	#else
	void FileSystemWatcher::WatchDirectory(const char* path)
	{
		char buffer[BUF_LEN];

		ssize_t length = read(fd, buffer, BUF_LEN);

		if (length < 0)
		{
			Debug::log("FileSystemWatcher error: read", Debug::DbgColorRed);
			return;
		}

		int i = 0;

		while (i < length)
		{
			struct inotify_event* event = (struct inotify_event*)&buffer[i];
			if (event->len)
			{
				char* _n = new char[event->len];
				memcpy(_n, &event->name, event->len);
				std::string name = _n;
				delete[] _n;

				std::string ext = IO::GetFileExtension(name);
				std::string currentDir = "";
				auto kt = std::find_if(watchDirs.begin(), watchDirs.end(), [=] (std::pair<std::string, int>& a) -> bool
				{
					return a.second == event->wd;
				});

				if (kt != watchDirs.end())
					currentDir = kt->first;

				if (event->mask & IN_CREATE)
				{
					if (!(event->mask & IN_ISDIR))
					{
						if (ext == "cs")
						{
							AssetsWindow::setScriptsChanged(true);
						}
					}
					if (event->mask & IN_ISDIR)
					{
						std::string new_dir = currentDir + name + "/";

						auto it = std::find_if(watchDirs.begin(), watchDirs.end(), [=] (std::pair<std::string, int>& a) -> bool
						{
							return a.first == new_dir;
						});
						
						if (it == watchDirs.end())
						{
							int new_wd = inotify_add_watch( fd, new_dir.c_str(), NOTIFY_FLAGS);
							watchDirs.push_back(std::make_pair(new_dir, new_wd));
						}
					}
				}
				else if (event->mask & IN_DELETE)
				{
					if (!(event->mask & IN_ISDIR))
					{
						if (ext == "cs")
						{
							AssetsWindow::setScriptsChanged(true);
						}
					}
					if (event->mask & IN_ISDIR)
					{
						std::string dir = currentDir + name + "/";

						auto it = std::find_if(watchDirs.begin(), watchDirs.end(), [=] (std::pair<std::string, int>& a) -> bool
						{
							return a.first == dir;
						});

						if (it != watchDirs.end())
						{
							inotify_rm_watch(fd, it->second);
							watchDirs.erase(it);
						}
					}
				}
				else if (event->mask & IN_MODIFY)
				{
					if (!(event->mask & IN_ISDIR))
					{
						if (ext == "cs")
						{
							AssetsWindow::setScriptsChanged(true);
						}
						else
						{
							std::string fn = currentDir + name;
							AssetsWindow::addChangedFile(fn);
						}
					}
				}
				else if ((event->mask & IN_MOVED_TO) || (event->mask & IN_MOVED_FROM))
				{
					if (!(event->mask & IN_ISDIR))
					{
						if (ext == "cs")
						{
							AssetsWindow::setScriptsChanged(true);
						}
						else
						{
							std::string fn = currentDir + name;
							AssetsWindow::addChangedFile(fn);
						}
					}
					if (event->mask & IN_ISDIR)
					{
						std::string dir = currentDir + name + "/";

						if (IO::DirExists(dir))
						{
							IO::listFiles(dir, true, nullptr, [=](std::string d, std::string f) -> bool
							{
								std::string fn = d + f;

								if (IO::GetFileExtension(fn) == "cs")
								{
									AssetsWindow::setScriptsChanged(true);
								}
								else
								{
									AssetsWindow::addChangedFile(fn);
								}

								return true;
							});
						}
						else
						{
							AssetsWindow::setScriptsChanged(true);
						}
					}
				}
				else if (event->mask & IN_CLOSE_WRITE)
				{
					if (!(event->mask & IN_ISDIR))
					{
						if (ext == "cs")
						{
							AssetsWindow::setScriptsChanged(true);
						}
						else
						{
							std::string fn = currentDir + name;
							AssetsWindow::addChangedFile(fn);
						}
					}
				}
			}

			i += EVENT_SIZE + event->len;
		}
	}
	#endif

	bool FileSystemWatcher::InitInstance()
	{
	#ifndef _WIN32
		std::string assetsPath = Engine::getSingleton()->getAssetsPath();

		fd = inotify_init();

		if (fd < 0)
		{
			Debug::log("FileSystemWatcher error: inotify_init", Debug::DbgColorRed);
			return false;
		}

		wd = inotify_add_watch(fd, assetsPath.c_str(), NOTIFY_FLAGS);
		watchDirs.push_back(std::make_pair(assetsPath, wd));

		IO::listFiles(assetsPath, true, [=](std::string dir) -> bool
		{
			if (!dir.empty() && IO::DirExists(dir))
			{
				int new_wd = inotify_add_watch(fd, dir.c_str(), NOTIFY_FLAGS);
				watchDirs.push_back(std::make_pair(dir + "/", new_wd));
			}

			return true;
		}, nullptr);
	#endif

		std::thread thr([=]()
			{
				while (work)
				{
	#ifdef _WIN32
					std::wstring assetsPath = StringConvert::s2ws(Engine::getSingleton()->getAssetsPath());
					WatchDirectory(assetsPath.c_str());
	#else
					WatchDirectory(assetsPath.c_str());
	#endif
				}

#ifndef _WIN32
				(void)inotify_rm_watch(fd, wd);
				(void)close(fd);

				watchDirs.clear();
#endif
			}
		);

		thr.detach();

		return true;
	}
}namespace GX
{
    bool AssetsWindow::isDirectory(TreeNode* node) const
    {
        return node && (node == treeView->getRootNode() || node->assetDirectory);
    }

    void AssetsWindow::navigateBrowser(const std::string& path)
    {
        browserSearchAll = false;
        browserSearch.clear();
        if (path == browserPath) return;
        browserPath = path;
        if (auto* node = treeView->getNodeByPath(path, treeView->getRootNode())) treeView->expandParent(node);
        browserHistory.resize(browserHistoryIndex + 1);
        browserHistory.push_back(path);
        browserHistoryIndex = static_cast<int>(browserHistory.size()) - 1;
        browserSearch.clear();
    }

    void AssetsWindow::externalDropTarget(TreeNode* folder, float x1, float y1, float x2, float y2)
    {
        if (ImGui::GetTopMostPopupModal() != nullptr || !ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)) return;
        for (auto& drop : externalFiles)
            if (!drop.handled && drop.x >= x1 && drop.x < x2 && drop.y >= y1 && drop.y < y2)
            {
                drop.handled = true;
                drop.folder = folder->getPath();
            }
    }

    void AssetsWindow::browserDropTarget(TreeNode* node)
    {
        if (!isDirectory(node)) return;
        const auto lo = ImGui::GetItemRectMin(), hi = ImGui::GetItemRectMax();
        if (ImGui::IsItemVisible()) externalDropTarget(node, lo.x, lo.y, hi.x, hi.y);
        if (ImGui::BeginDragDropTarget())
        {
            if (const auto* payload = ImGui::AcceptDragDropPayload("DND_TreeView"))
            {
                auto* from = static_cast<TreeNode*>(payload->Data);
                const std::string targetId = node->name, sourceId = from->name;
                TreeView* sourceTree = from->treeView;
                // Resolve IDs after rendering: the existing operation can rebuild the tree.
                MainWindow::addOnEndUpdateCallback([this, targetId, sourceId, sourceTree]() {
                    TreeNode* target = targetId.empty() ? treeView->getRootNode() : treeView->getNodeByName(targetId);
                    if (!target && treeView->getRootNode()->name == targetId) target = treeView->getRootNode();
                    TreeNode* source = sourceTree->getNodeByName(sourceId);
                    if (!target || !source || !isDirectory(target) || target == source || target->isChildOf(source)) return;
                    if (sourceTree == treeView && treeView->getSelectedNodes().empty()) treeView->selectNodes({sourceId});
                    for (auto* selected : treeView->getSelectedNodes())
                        if (target == selected || target->isChildOf(selected)) return;
                    onDropNode(target, source);
                });
            }
            ImGui::EndDragDropTarget();
        }
    }

    bool AssetsWindow::folderMatches(TreeNode* node) const
    {
        if (!isDirectory(node)) return false;
        if (folderSearch.empty() || boost::icontains(node->alias, folderSearch)) return true;
        for (auto* child : node->children) if (folderMatches(child)) return true;
        return false;
    }

    void AssetsWindow::browserFolders(TreeNode* node)
    {
        for (auto* child : node->children)
        {
            if (!folderMatches(child)) continue;
            ImGui::PushID(child->name.c_str());
            ImGui::SetNextItemOpen(folderSearch.empty() ? child->expanded : true);
            const bool open = ImGui::TreeNodeEx(("   " + child->alias).c_str(), ImGuiTreeNodeFlags_OpenOnArrow |
                (child->getPath() == browserPath ? ImGuiTreeNodeFlags_Selected : 0));
            if (folderIcon)
            {
                const auto p = ImGui::GetItemRectMin();
                ImGui::GetWindowDrawList()->AddImage((void*)(intptr_t)folderIcon->getHandle().idx,
                    ImVec2(p.x + 17, p.y), ImVec2(p.x + 31, p.y + 14), ImVec2(0,1), ImVec2(1,0));
            }
            if (folderSearch.empty()) child->expanded = open;
            if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
            {
                navigateBrowser(child->getPath());
                treeView->selectNodes({child->name});
            }
            browserItemActions(child);
            if (open) { browserFolders(child); ImGui::TreePop(); }
            ImGui::PopID();
        }
    }

    void AssetsWindow::updatePendingEdit()
    {
        if (pendingEditId.empty())
        {
            std::vector<TreeNode*> nodes = treeView->getRootNode()->children;
            while (!nodes.empty())
            {
                TreeNode* node = nodes.back(); nodes.pop_back();
                if (node->isEditing())
                {
                    browserResultsDirty = true;
                    pendingEditId = node->name;
                    pendingEditName = node->alias;
                    navigateBrowser(node->parent->getPath());
                    ImGui::OpenPopup("Asset name");
                    break;
                }
                nodes.insert(nodes.end(), node->children.begin(), node->children.end());
            }
        }
        if (ImGui::BeginPopupModal("Asset name", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
            ImGui::InputText("Name", &pendingEditName);
            auto* node = treeView->getNodeByName(pendingEditId);
            const bool valid = !pendingEditName.empty() && pendingEditName != "." && pendingEditName != ".." &&
                pendingEditName.find_first_of("/\\:*?\"<>|") == std::string::npos;
            if (ImGui::Button("Create / apply") && valid && node)
            {
                browserResultsDirty = true;
                node->finishEdit(pendingEditName);
                pendingEditId.clear(); ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel") || !node)
            {
                browserResultsDirty = true;
                if (node) node->finishEdit(node->alias, true);
                pendingEditId.clear(); ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }

    void AssetsWindow::browserItemActions(TreeNode* node)
    {
        if (ImGui::BeginDragDropSource())
        {
            const auto& selected = treeView->getSelectedNodes();
            // Drag selection must preserve the Inspector target (e.g. a material texture slot).
            if (std::find(selected.begin(), selected.end(), node) == selected.end()) treeView->selectNodes({node->name}, false);
            ImGui::SetDragDropPayload("DND_TreeView", node, sizeof(TreeNode));
            ImGui::TextUnformatted(node->alias.c_str());
            ImGui::EndDragDropSource();
        }
        browserDropTarget(node);
        if (ImGui::BeginPopupContextItem("asset_actions"))
        {
            const auto selection = treeView->getSelectedNodes();
            if (std::find(selection.begin(), selection.end(), node) == selection.end()) treeView->selectNodes({node->name});
            updateAssetsPopup(true, isDirectory(node) ? node : node->parent);
            if (ImGui::MenuItem("Show in folder"))
            {
                navigateBrowser(node->parent->getPath());
                browserSearchAll = false;
                browserSearch.clear();
                treeView->selectNodes({node->name});
                browserFocusId = node->name;
            }
            if (ImGui::MenuItem("Open"))
            {
                if (isDirectory(node)) navigateBrowser(node->getPath());
                else onNodeDoubleClick(node);
            }
            if (ImGui::MenuItem("Rename"))
            {
                browserRenameId = node->name;
                browserRenameText = node->alias;
                browserRenameError.clear();
            }
            if (ImGui::MenuItem("Delete"))
            {
                browserDeleteId = node->name;
                browserDeleteIds.clear();
                for (auto* selected : treeView->getSelectedNodes()) if (selected) browserDeleteIds.push_back(selected->name);
            }
            if (ImGui::MenuItem("Show in Explorer")) onNodePopupMenu(node, 2);
            if (ImGui::MenuItem("Reload"))
            {
                const auto id = node->name;
                MainWindow::addOnEndUpdateCallback([this, id]() {
                    if (auto* current = treeView->getNodeByName(id)) onNodePopupMenu(current, 3);
                });
            }
            const auto ext = IO::GetFileExtension(node->alias);
            const auto& models = Engine::getModel3dFileFormats();
            const auto& videos = Engine::getVideoFileFormats();
            const char* extra = std::find(models.begin(), models.end(), ext) != models.end() ? "Extract animations" :
                (ext == "animation" ? "Crop animation" : (std::find(videos.begin(), videos.end(), ext) != videos.end() ? "Extract audio" : nullptr));
            if (extra && ImGui::MenuItem(extra))
            {
                const auto id = node->name;
                MainWindow::addOnEndUpdateCallback([this, id]() { if (auto* current = treeView->getNodeByName(id)) onNodePopupMenu(current, 4); });
            }
            ImGui::EndPopup();
        }
    }

    void AssetsWindow::updateBrowser()
    {
        if (!browserOpened) return;
        restoreAssetWindowLayout("\xee\x80\x80 Asset Browser###Assets", "Assets");
        if (ImGui::Begin("\xee\x80\x80 Asset Browser###Assets", &browserOpened, ImGuiWindowFlags_NoScrollbar))
        {
            focused = focused || ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
            auto* root = treeView->getRootNode();
            TreeNode* folder = browserPath.empty() ? root : treeView->getNodeByPath(browserPath, root);
            if (!folder || !isDirectory(folder)) { navigateBrowser(""); folder = root; }
            // Capture height before the full-height folder child advances the layout cursor.
            const float paneHeight = std::max(1.0f, ImGui::GetContentRegionAvail().y);
            const float available = ImGui::GetContentRegionAvail().x;
            const float maxLeft = std::max(100.0f, available - 160.0f);
            folderWidth = std::max(100.0f, std::min(folderWidth, maxLeft));
            ImGui::BeginChild("Folders", ImVec2(folderWidth, 0), true);
            ImGui::TextUnformatted("Assets");
            if (ImGui::Button("Create"))
            {
                treeView->selectNodes({}, false);
                ImGui::OpenPopup("add_asset_popup");
            }
            updateAssetsPopup(false, folder);
            ImGui::SameLine();
            if (ImGui::Button("Collapse all")) { folderSearch.clear(); treeView->collapseAll(); }
            ImGui::SetNextItemWidth(-1);
            ImGui::InputTextWithHint("##folders_search", "Find folder...", &folderSearch);
            if (ImGui::Selectable("Assets##root_folder", browserPath.empty())) { navigateBrowser(""); treeView->selectNodes({}); }
            browserDropTarget(root);
            browserFolders(root);
            ImGui::EndChild();
            ImGui::SameLine(0, 2);
            ImGui::InvisibleButton("##asset_splitter", ImVec2(5, paneHeight));
            if (ImGui::IsItemHovered() || ImGui::IsItemActive()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
            if (ImGui::IsItemActive()) folderWidth = std::max(100.0f, std::min(folderWidth + ImGui::GetIO().MouseDelta.x, maxLeft));
            ImGui::SameLine(0, 2);
            ImGui::BeginChild("Contents", ImVec2(0, 0), true);
            folder = browserPath.empty() ? root : treeView->getNodeByPath(browserPath, root);
            if (!folder || !isDirectory(folder)) folder = root;
            if (ImGui::Button("Back") && browserHistoryIndex > 0) { browserPath = browserHistory[--browserHistoryIndex]; browserSearch.clear(); browserSearchAll = false; }
            ImGui::SameLine();
            if (ImGui::Button("Forward") && browserHistoryIndex + 1 < static_cast<int>(browserHistory.size())) { browserPath = browserHistory[++browserHistoryIndex]; browserSearch.clear(); browserSearchAll = false; }
            ImGui::SameLine();
            if (ImGui::Button("Up") && folder->parent) navigateBrowser(folder->parent->getPath());
            ImGui::SameLine();
            ImGui::Checkbox("Icons", &browserIcons);
            ImGui::SameLine();
            ImGui::Checkbox("Z-A", &browserDescending);
            if (ImGui::Button("Assets##root")) navigateBrowser("");
            browserDropTarget(root);
            std::vector<TreeNode*> crumbs;
            for (auto* n = folder; n && n != root; n = n->parent) crumbs.push_back(n);
            for (auto it = crumbs.rbegin(); it != crumbs.rend(); ++it)
            {
                ImGui::SameLine(); ImGui::TextUnformatted("/"); ImGui::SameLine();
                ImGui::PushID((*it)->name.c_str());
                if (ImGui::Button((*it)->alias.c_str())) navigateBrowser((*it)->getPath());
                browserDropTarget(*it);
                ImGui::PopID();
            }
            ImGui::SetNextItemWidth(-1);
            ImGui::InputTextWithHint("##browser_search", browserSearchAll ? "Search all Assets..." : "Search this folder...", &browserSearch);
            if (ImGui::RadioButton("Current folder", !browserSearchAll)) browserSearchAll = false;
            ImGui::SameLine();
            if (ImGui::RadioButton("All Assets", browserSearchAll)) browserSearchAll = true;
            // Navigation above may have changed the folder this frame.
            folder = browserPath.empty() ? root : treeView->getNodeByPath(browserPath, root);
            if (!folder || !isDirectory(folder)) folder = root;
            const ImVec2 areaMin = ImGui::GetWindowPos(), areaSize = ImGui::GetWindowSize();
            // Search/sort only when the query, scope, ordering or asset tree changes.
            // Directory metadata comes from the asset scan: no filesystem calls in rendering.
            if (browserResultsDirty || cachedBrowserPath != browserPath || cachedBrowserSearch != browserSearch ||
                cachedSearchAll != browserSearchAll || cachedDescending != browserDescending)
            {
                auto lower = [](std::string value) {
                    for (auto& c : value) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    return value;
                };
                const auto query = lower(browserSearch);
                auto candidates = browserSearchAll ? root->children : folder->children;
                struct Match { TreeNode* node; std::string key; };
                std::vector<Match> matches;
                for (size_t i = 0; i < candidates.size(); ++i)
                {
                    auto* node = candidates[i];
                    if (browserSearchAll) candidates.insert(candidates.end(), node->children.begin(), node->children.end());
                    if (node->isEditing()) continue;
                    auto key = lower(node->alias);
                    if (key.find(query) != std::string::npos) matches.push_back({node, std::move(key)});
                }
                std::sort(matches.begin(), matches.end(), [&](const Match& a, const Match& b) {
                    const bool ad = isDirectory(a.node), bd = isDirectory(b.node);
                    if (ad != bd) return ad;
                    return browserDescending ? a.key > b.key : a.key < b.key;
                });
                browserResults.clear();
                browserResults.reserve(matches.size());
                for (const auto& match : matches) browserResults.push_back(match.node);
                cachedBrowserPath = browserPath;
                cachedBrowserSearch = browserSearch;
                cachedSearchAll = browserSearchAll;
                cachedDescending = browserDescending;
                browserResultsDirty = false;
            }
            const int columns = browserIcons ? std::max(1, static_cast<int>(ImGui::GetContentRegionAvail().x / 112.0f)) : 1;
            int visibleCount = 0;
            ImGui::Columns(columns, "asset_columns", false);
            for (auto* node : browserResults)
            {
                if (node->isEditing()) continue;
                ++visibleCount;
                ImGui::PushID(node->name.c_str());
                const auto& selected = treeView->getSelectedNodes();
                const bool active = std::find(selected.begin(), selected.end(), node) != selected.end();
                const ImVec2 pos = ImGui::GetCursorScreenPos();
                if (ImGui::Selectable("##asset", active, ImGuiSelectableFlags_AllowDoubleClick,
                    ImVec2(0, browserIcons ? (browserSearchAll ? 106.0f : 86.0f) : (browserSearchAll ? 44.0f : 24.0f))))
                {
                    std::vector<std::string> names;
                    if (ImGui::GetIO().KeyCtrl)
                        for (auto* n : selected) if (n != node) names.push_back(n->name);
                    if (!ImGui::GetIO().KeyCtrl || !active) names.push_back(node->name);
                    treeView->selectNodes(names);
                    if (ImGui::IsMouseDoubleClicked(0))
                    {
                        if (isDirectory(node)) navigateBrowser(node->getPath());
                        else onNodeDoubleClick(node);
                    }
                }
                if (browserFocusId == node->name) { ImGui::SetScrollHereY(); browserFocusId.clear(); }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", node->getPath().c_str());
                const bool itemVisible = ImGui::IsItemVisible();
                browserItemActions(node);
                if (!itemVisible) { ImGui::NextColumn(); ImGui::PopID(); continue; }
                const float size = browserIcons ? 48.0f : 20.0f;
                auto* draw = ImGui::GetWindowDrawList();
                draw->PushClipRect(pos, ImVec2(pos.x + ImGui::GetColumnWidth() - 8, pos.y + (browserIcons ? (browserSearchAll ? 106 : 86) : (browserSearchAll ? 44 : 24))), true);
                if (node->icon)
                    draw->AddImage((void*)(intptr_t)node->icon->getHandle().idx, pos, ImVec2(pos.x + size, pos.y + size), ImVec2(0,1), ImVec2(1,0));
                draw->AddText(ImVec2(pos.x + (browserIcons ? 0 : 26), pos.y + (browserIcons ? 52 : 3)), ImGui::GetColorU32(ImGuiCol_Text), node->alias.c_str());
                if (browserSearchAll)
                    draw->AddText(ImVec2(pos.x, pos.y + (browserIcons ? 74 : 24)), ImGui::GetColorU32(ImGuiCol_TextDisabled), node->getPath().c_str());
                draw->PopClipRect();
                ImGui::NextColumn();
                ImGui::PopID();
            }
            ImGui::Columns(1);
            if (visibleCount == 0) ImGui::TextDisabled(browserSearch.empty() ? "Empty folder" : "No matching assets");
            ImGui::Dummy(ImVec2(ImGui::GetContentRegionAvail().x, std::max(24.0f, ImGui::GetContentRegionAvail().y)));
            browserDropTarget(folder);
            externalDropTarget(folder, areaMin.x, areaMin.y, areaMin.x + areaSize.x, areaMin.y + areaSize.y);
            if (ImGui::BeginPopupContextWindow("folder_actions", 1, false))
            {
                updateAssetsPopup(true, folder);
                ImGui::EndPopup();
            }
            ImGui::EndChild();
            updatePendingEdit();
            if (!browserRenameId.empty() && !ImGui::IsPopupOpen("Rename asset")) ImGui::OpenPopup("Rename asset");
            if (ImGui::BeginPopupModal("Rename asset", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
            {
                ImGui::InputText("Name", &browserRenameText);
                ImGui::TextUnformatted(browserRenameError.c_str());
                if (ImGui::Button("Rename") && !browserRenameText.empty() && browserRenameText != "." && browserRenameText != ".." &&
                    browserRenameText.find_first_of("/\\:*?\"<>|") == std::string::npos)
                {
                    auto* n = treeView->getNodeByName(browserRenameId);
                    if (n)
                    {
                        auto old = n->alias; n->alias = browserRenameText;
                        if (!onNodeRenamed(n, old))
                        {
                            n->alias = old;
                            browserRenameError = "Rename failed; check the name and existing files.";
                        }
                        else { browserRenameId.clear(); ImGui::CloseCurrentPopup(); }
                    }
                    else { browserRenameId.clear(); ImGui::CloseCurrentPopup(); }
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel")) { browserRenameId.clear(); ImGui::CloseCurrentPopup(); }
                ImGui::EndPopup();
            }
            if (!browserDeleteId.empty() && !ImGui::IsPopupOpen("Delete asset")) ImGui::OpenPopup("Delete asset");
            if (ImGui::BeginPopupModal("Delete asset", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
            {
                ImGui::Text("Delete %d selected asset(s), including folder contents?", static_cast<int>(browserDeleteIds.size()));
                if (ImGui::Button("Delete"))
                {
                    const auto ids = browserDeleteIds;
                    MainWindow::addOnEndUpdateCallback([this, ids]() {
                        std::vector<std::string> existing;
                        for (const auto& id : ids) if (treeView->getNodeByName(id)) existing.push_back(id);
                        if (!existing.empty())
                        {
                            treeView->selectNodes(existing, false);
                            onNodePopupMenu(treeView->getNodeByName(existing.front()), 1);
                        }
                    });
                    browserDeleteId.clear(); ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel")) { browserDeleteId.clear(); ImGui::CloseCurrentPopup(); }
                ImGui::EndPopup();
            }
        }
        ImGui::End();
    }

    void AssetsWindow::importExternalFiles()
    {
        if (externalFiles.empty()) return;
        namespace fs = std::filesystem;
        bool copied = false;
        for (const auto& drop : externalFiles)
        {
            if (!drop.handled)
            {
                Debug::logWarning("Drop ignored: place files over Assets or Asset Browser: " + drop.file);
                continue;
            }
            try
            {
                const auto source = fs::weakly_canonical(fs::u8path(drop.file));
                const auto root = fs::weakly_canonical(fs::u8path(CP_UNI(Engine::getSingleton()->getAssetsPath())));
                const auto target = fs::weakly_canonical(root / fs::u8path(drop.folder));
                auto inside = [](const fs::path& child, const fs::path& parent) {
                    auto folded = [](const fs::path& path) {
                        std::wstring value = path.wstring();
#ifdef _WIN32
                        if (!value.empty()) CharLowerBuffW(&value[0], static_cast<DWORD>(value.size()));
#endif
                        return fs::path(value);
                    };
                    const auto relative = folded(child).lexically_relative(folded(parent));
                    return !relative.empty() && !relative.is_absolute() && *relative.begin() != "..";
                };
                if (target != root && !inside(target, root)) throw std::runtime_error("Target is outside Assets");
                if (!fs::is_directory(target)) throw std::runtime_error("Target folder no longer exists");
                if (!fs::is_regular_file(source)) throw std::runtime_error("Drop files only; directory import is not supported");
                if (inside(source, root)) throw std::runtime_error("Use internal asset drag-and-drop for files already in this project");
                const auto destination = target / source.filename();
                // copy_options::none fails on collisions; never replace a resource or its GUID.
                if (!fs::copy_file(source, destination, fs::copy_options::none)) throw std::runtime_error("File was not copied");
                copied = true;
                Debug::log("Imported: " + drop.file + " -> " + drop.folder);
            }
            catch (const std::exception& error)
            {
                Debug::logWarning("Asset import skipped: " + drop.file + " - " + error.what());
            }
        }
        externalFiles.clear();
        if (copied) reloadFiles();
    }
}
