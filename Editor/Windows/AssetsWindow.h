#pragma once

#include <string>
#include <vector>
#include <atomic>
#include <mutex>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#endif

#define MAX_DIRS 1
#define MAX_BUFFER 4096

namespace GX
{
	class TreeView;
	class TreeNode;
	class Texture;
	class Shader;
	class DialogAnimationCrop;

	#ifdef _WIN32
	class FileSystemWatcher
	{
	private:
		typedef struct _DIRECTORY_INFO {
			HANDLE hDir;
			wchar_t lpszDirName[260];
			wchar_t lpBuffer[MAX_BUFFER];
			DWORD dwBufLength;
			OVERLAPPED Overlapped;
		} DIRECTORY_INFO, * PDIRECTORY_INFO, * LPDIRECTORY_INFO;

	private:
		void WatchDirectory(const wchar_t* path);

		DIRECTORY_INFO DirInfo[MAX_DIRS];
		DWORD numDirs;
		
		bool work = true;

	public:
		virtual bool InitInstance();
		virtual int ExitInstance()
		{
			work = false;
			return 0;
		}
	};
	#else
	class FileSystemWatcher
	{
	private:
	#ifdef _WIN32
		void WatchDirectory(const wchar_t* path);
	#else
		int fd = 0;
		int wd = 0;

		std::vector<std::pair<std::string, int>> watchDirs;

		void WatchDirectory(const char* path);
	#endif
		bool work = true;
		
	public:
		virtual bool InitInstance();
		virtual int ExitInstance()
		{
			work = false;
			return 0;
		}
	};
	#endif

	class AssetsWindow
	{
	private:
		static AssetsWindow* singleton;

		DialogAnimationCrop* dialogAnimationCrop = nullptr;
		
		FileSystemWatcher* fswatcher = nullptr;

		std::vector<Shader*> shaderList;

        bool browserOpened = true;
        bool browserSearchAll = false;
        bool browserResultsDirty = true;
        bool cachedSearchAll = false;
        bool cachedDescending = false;
        std::string cachedBrowserPath, cachedBrowserSearch;
        std::vector<TreeNode*> browserResults;
        std::string folderSearch;
        float folderWidth = 240.0f;
        std::string pendingEditId;
        std::string pendingEditName;
        bool folderMatches(TreeNode* node) const;
        void updatePendingEdit();
        bool browserIcons = true;
        bool browserDescending = false;
        std::string browserPath;
        std::string browserSearch;
        std::vector<std::string> browserHistory = { "" };
        int browserHistoryIndex = 0;
        std::string browserRenameId;
        std::string browserRenameText;
        std::string browserRenameError;
        std::string browserDeleteId;
        std::vector<std::string> browserDeleteIds;
        std::string browserFocusId;
        struct ExternalFile { std::string file; float x, y; bool handled = false; std::string folder; };
        std::vector<ExternalFile> externalFiles;
        void updateBrowser();
        void navigateBrowser(const std::string& path);
        bool isDirectory(TreeNode* node) const;
        void browserFolders(TreeNode* node);
        void browserItemActions(TreeNode* node);
        void browserDropTarget(TreeNode* node);
        void externalDropTarget(TreeNode* folder, float x1, float y1, float x2, float y2);
        void importExternalFiles();
		bool isWindowDragging = false;

		TreeView* treeView = nullptr;

		Texture * addIcon = nullptr;
		Texture * addFolderIcon = nullptr;
		Texture * closeIcon = nullptr;

		Texture* folderIcon = nullptr;
		Texture* meshIcon = nullptr;
		Texture* textureIcon = nullptr;
		Texture* materialIcon = nullptr;
		Texture* sceneIcon = nullptr;
		Texture* shaderIcon = nullptr;
		Texture* cubemapIcon = nullptr;
		Texture* csIcon = nullptr;
		Texture* audioClipIcon = nullptr;
		Texture* fontIcon = nullptr;
		Texture* prefabIcon = nullptr;
		Texture* textIcon = nullptr;
		Texture* animationClipIcon = nullptr;
		Texture* terrainIcon = nullptr;
		Texture* packageIcon = nullptr;
		Texture* navmeshIcon = nullptr;
		Texture* videoClipIcon = nullptr;

		Texture* renameIcon = nullptr;
		Texture* deleteIcon = nullptr;
		Texture* showInExplorerIcon = nullptr;
		Texture* reloadIcon = nullptr;
		Texture* extractAnimsIcon = nullptr;
		Texture* extractAudioIcon = nullptr;
		Texture* cropAnimIcon = nullptr;

		std::vector<std::string> expandedItems;
		void sortTree();
		void sortTree(TreeNode* root);

		static void onNodesSelected(std::vector<std::string> names, void* userData);
		void onDropNode(TreeNode* node, TreeNode* from);
		bool onNodeRenamed(TreeNode* item, std::string oldName);
		void onNodePopupMenu(TreeNode* node, int val);
		void onNodeDoubleClick(TreeNode* node);

		void updateAssetsPopup(bool itemsOnly = false, TreeNode* selNode = nullptr);
		std::vector<std::string> addNewFiles(std::vector<std::string> files = std::vector<std::string>());
		void addScripts(std::vector<std::string> files = std::vector<std::string>());
		void rememberExpandedItems(TreeNode* root);
		void rememberAllItems(TreeNode* root, std::vector<std::string>& list);
		void createFolder(TreeNode* selNode = nullptr);

		Texture* updateTextureIcon(std::string path, bool recreate = false);

		static std::atomic<bool> scriptsChanged;
		static std::mutex changedFilesMutex;
		static std::vector<std::string> changedFiles;

		bool focused = false;

	public:
		AssetsWindow();
		~AssetsWindow();

		void init();
		void update();
		void show(bool show) { browserOpened = show; }
        void showBrowser(bool value) { browserOpened = value; }
        bool getBrowserVisible() const { return browserOpened; }
        void queueExternalFile(const std::string& path, float x, float y) { externalFiles.push_back({path, x, y}); }
		bool getVisible() { return browserOpened; }
		void focusOnFile(std::string filepath);
		void reloadFiles();
		void reloadShaderList();
		void reloadChangedFiles();
		void reloadChangedFile(std::string path);
		static void setScriptsChanged(bool value) { scriptsChanged = value; }
		static void addChangedFile(std::string value);

		static AssetsWindow* getSingleton() { return singleton; }

		TreeView* getTreeView() { return treeView; }
		Texture* getIconByExtension(std::string ext);

		std::vector<Shader*>& getShaderList() { return shaderList; }

		bool isFocused() { return focused; }
	};
}
