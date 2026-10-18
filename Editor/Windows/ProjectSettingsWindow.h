#pragma once
#include <string>

namespace GX
{
	class ProjectSettingsEditor;

	class ProjectSettingsWindow
	{
	private:
		bool opened = false;
        bool tagsOpened = false;
        bool usedTagsOnly = false;
        bool showDeletedTags = false;
        int editTag = -1;
        int selectedTagSet = -1;
        bool favoriteTagsOnly = false;
        std::string tagSetName;
        std::string tagSetError;
        std::string tagName;
        std::string tagError;
        void updateTags();
		ProjectSettingsEditor* editor = nullptr;

	public:
		ProjectSettingsWindow();
		~ProjectSettingsWindow();

		void init();
		void update();
		void show(bool show);
        void showTags(bool value) { tagsOpened = value; }
        bool getTagsVisible() const { return tagsOpened; }
		bool getVisible() const { return opened; }
	};
}
