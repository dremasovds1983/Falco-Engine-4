#include "DialogAssetExporter.h"

#include <imgui.h>
#include <imgui_internal.h>
#include <dear-imgui/misc/cpp/imgui_stdlib.h>
#include <boost/algorithm/string.hpp>

#include <zip.h>

#include "../Engine/Core/Engine.h"
#include "../Engine/Classes/StringConverter.h"
#include "../Engine/Classes/IO.h"
#include "../Engine/Classes/GUIDGenerator.h"

#include "../Windows/MainWindow.h"
#include "../Windows/ConsoleWindow.h"
#include "../Windows/AssetsWindow.h"
#include "../Classes/TreeView.h"
#include "../Classes/TreeNode.h"
#include "DialogProgress.h"

#include <fstream>
#include <fcntl.h>
#include <cerrno>
#include <cstring>
#include <filesystem>

#ifdef _WIN32
#include <io.h>
#include <combaseapi.h>
#include <shobjidl_core.h>
#endif

namespace GX
{
	namespace
	{
		bool validPackagePath(const std::string& path)
		{
			if (path.empty() || path.front() == '/' || path.find_first_of(":\\") != std::string::npos)
				return false;
			std::vector<std::string> parts;
			boost::split(parts, path, [](char c) { return c == '/'; });
			for (const auto& part : parts)
			{
				if (part.empty() || part == "." || part == ".." || part.back() == '.' || part.back() == ' '
					|| part.find_first_of("<>\"|?*") != std::string::npos)
					return false;
				for (unsigned char ch : part)
					if (ch < 32) return false;
				const std::string stem = boost::to_upper_copy(part.substr(0, part.find('.')));
				if (stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL"
					|| (stem.size() == 4 && (stem.substr(0, 3) == "COM" || stem.substr(0, 3) == "LPT")
						&& stem[3] >= '1' && stem[3] <= '9'))
					return false;
			}
			return true;
		}
	}

	DialogProgress* DialogAssetExporter::progressDialog = nullptr;

	DialogAssetExporter::DialogAssetExporter()
	{
		treeView = new TreeView();

		progressDialog = new DialogProgress();
		progressDialog->setTitle("Exporting package");
	}

	DialogAssetExporter::~DialogAssetExporter()
	{
		delete progressDialog;
		delete treeView;
	}

	void DialogAssetExporter::show(Mode m)
	{
		allSelected = true;
		setMode(m);

		treeView->clear();
		importEntries.clear();
		visible = false;

		if (mode == Mode::Import)
		{
	#ifdef _WIN32
			IFileDialog* pfd;
			if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pfd))))
			{
				DWORD dwOptions;
				if (SUCCEEDED(pfd->GetOptions(&dwOptions)))
				{
					pfd->SetOptions(dwOptions);
				}

				COMDLG_FILTERSPEC ComDlgFS[1] = { {L"Package files", L"*.package"} };
				pfd->SetFileTypes(1, ComDlgFS);

				if (SUCCEEDED(pfd->Show(NULL)))
				{
					IShellItem* psi;
					if (SUCCEEDED(pfd->GetResult(&psi)))
					{
						LPWSTR g_path = NULL;
						LPWSTR g_name = NULL;

						if (!SUCCEEDED(psi->GetDisplayName(SIGDN_DESKTOPABSOLUTEPARSING, &g_path)))
						{
							MessageBox(NULL, "SIGDN_DESKTOPABSOLUTEPARSING failed", NULL, NULL);
						}
						else
						{
							if (!SUCCEEDED(psi->GetDisplayName(SIGDN_NORMALDISPLAY, &g_name)))
							{
								MessageBox(NULL, "SIGDN_NORMALDISPLAY failed", NULL, NULL);
							}
							else
							{
								std::string fileName = StringConvert::ws2s(g_name);
								std::string filePath = StringConvert::ws2s(g_path);
	#else
			char filename[1024];
			FILE *f = popen("zenity --file-selection --file-filter='Package files | *.package'", "r");
			fgets(filename, 1024, f);
			int r = pclose(f);

			if (r == 0)
			{
				std::string filePath = filename;
				int idx = filePath.find("\n");
				if (idx != std::string::npos)
					filePath = filePath.substr(0, idx);
	#endif

								filePath = IO::ReplaceBackSlashes(filePath);
								
								if (IO::FileExists(filePath))
								{
									visible = true;

									struct zip* za;
									int err = 0;
									struct zip_stat sb;

									if ((za = zip_open_z(CP_UNI(filePath).c_str(), ZIP_RDONLY, &err)) == NULL) {
										zip_error_t zerr;
										zip_error_init_with_code(&zerr, err);
										MainWindow::getConsoleWindow()->log("Failed to open package: " + std::string(zip_error_strerror(&zerr)), LogMessageType::LMT_ERROR);
										zip_error_fini(&zerr);
										return;
									}

									openedPackage = filePath;

									for (zip_int64_t i = 0; i < zip_get_num_entries(za, 0); ++i)
									{
										if (zip_stat_index(za, i, 0, &sb) != 0 || sb.name == nullptr)
										{
											visible = false;
											MainWindow::getConsoleWindow()->log("Cannot read package directory", LogMessageType::LMT_ERROR);
											break;
										}
										std::string path = IO::ReplaceBackSlashes(sb.name);
										bool directory = !path.empty() && path.back() == '/';
										if (directory) path.pop_back();
										bool conflict = false;
										const auto key = boost::to_lower_copy(path);
										for (const auto& existing : importEntries)
										{
											const auto other = boost::to_lower_copy(existing.first);
											if (key == other || key.find(other + "/") == 0
												|| (!directory && other.find(key + "/") == 0))
												conflict = true;
										}
										if (!validPackagePath(path) || conflict)
										{
											visible = false;
											MainWindow::getConsoleWindow()->log("Invalid or duplicate package path: " + path, LogMessageType::LMT_ERROR);
											break;
										}

										if (!directory) importEntries.emplace(path, sb.name);

										if (treeView->getNodeByPath(path, treeView->getRootNode()) == nullptr)
										{
											std::vector<std::string> results;
											boost::split(results, path, [](char c) { return c == '/' || c == '\\'; });

											TreeNode* item = treeView->getRootNode();

											for (std::vector<std::string>::iterator it2 = results.begin(); it2 != results.end(); ++it2)
											{
												TreeNode* newItem = treeView->getNodeByPath(*it2, item);
												if (newItem == nullptr)
												{
													Texture* icon = MainWindow::getAssetsWindow()->getIconByExtension(IO::GetFileExtension(*it2));

													TreeNode* node = new TreeNode(treeView);
													node->alias = *it2;
													node->name = GUIDGenerator::genGuid();
													node->icon = icon;
													node->enableDrag = false;
													node->setChecked(true);
													node->setCheckboxEnabled(true);
													node->setEditable(false);
													node->setCheckChildren(true);
													item->addChild(node);

													item = node;

													std::string ext = IO::GetFileExtension(*it2);
													std::string _str = CP_SYS(item->getPath());

													std::string assetsPath = Engine::getSingleton()->getAssetsPath();
													if (IO::FileExists(assetsPath + _str) || IO::DirExists(assetsPath + _str))
													{
														item->setChecked(true);
														item->setCheckboxEnabled(false);
													}
												}
												else
												{
													item = newItem;
												}
											}

											results.clear();
										}
									}

									zip_close_z(za);
								}
								else
									visible = false;
	#ifdef _WIN32
							}
						}

						CoTaskMemFree(g_path);
						CoTaskMemFree(g_name);

						psi->Release();
					}
				}
				pfd->Release();
			}
	#else
			}
	#endif
		}
		else
		{
			visible = true;
			copyTreeView(MainWindow::getAssetsWindow()->getTreeView()->getRootNode(), treeView->getRootNode());
		}
	}

	void DialogAssetExporter::update()
	{
		std::string caption = "";
		std::string btnImportExport = "";

		if (mode == Mode::Export)
		{
			caption = "Export Assets";
			btnImportExport = "Export";
		}
		else
		{
			caption = "Import Assets";
			btnImportExport = "Import";
		}

		bool closed = false;
		bool ok = false;

		if (visible)
		{
			ImGui::SetNextWindowSize(ImVec2(300.0f, 500.0f), ImGuiCond_Appearing);
			ImGui::SetNextWindowPos(ImVec2(MainWindow::getSingleton()->getWidth() / 2, MainWindow::getSingleton()->getHeight() / 2), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
			if (ImGui::Begin(caption.c_str(), nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking))
			{
				if (ImGui::Checkbox("", &allSelected))
				{
					TreeNode* root = treeView->getRootNode();

					for (auto it = root->children.begin(); it != root->children.end(); ++it)
						checkNodes(*it, allSelected);
				}

				ImGui::SeparatorEx(ImGuiSeparatorFlags_Horizontal);

				ImGuiWindow* window = GImGui->CurrentWindow;
				ImGuiID id = window->GetIDNoKeepAlive("##AssetsExporterVS");
				ImGui::BeginChild(id, ImVec2(0, ImGui::GetWindowSize().y - 95));
				treeView->update();
				ImGui::EndChild();

				ImGui::SeparatorEx(ImGuiSeparatorFlags_Horizontal);

				if (ImGui::Button("Cancel"))
				{
					closed = true;
					ok = false;
				}

				ImGui::SameLine();

				if (ImGui::Button(btnImportExport.c_str()))
				{
					closed = true;
					ok = true;
				}
			}

			ImGui::End();
		}

		if (closed && ok)
		{
			if (mode == Mode::Export)
			{
	#ifdef _WIN32
				IFileDialog* pfd;
				if (SUCCEEDED(CoCreateInstance(CLSID_FileSaveDialog, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pfd))))
				{
					DWORD dwOptions;
					if (SUCCEEDED(pfd->GetOptions(&dwOptions)))
					{
						pfd->SetOptions(dwOptions);
					}

					COMDLG_FILTERSPEC ComDlgFS[1] = { {L"Package files", L"*.package"} };
					pfd->SetFileTypes(1, ComDlgFS);

					if (SUCCEEDED(pfd->Show(NULL)))
					{
						IShellItem* psi;
						if (SUCCEEDED(pfd->GetResult(&psi)))
						{
							LPWSTR g_path = NULL;
							LPWSTR g_name = NULL;

							if (!SUCCEEDED(psi->GetDisplayName(SIGDN_DESKTOPABSOLUTEPARSING, &g_path)))
							{
								MessageBox(NULL, "SIGDN_DESKTOPABSOLUTEPARSING failed", NULL, NULL);
							}
							else
							{
								if (!SUCCEEDED(psi->GetDisplayName(SIGDN_NORMALDISPLAY, &g_name)))
								{
									MessageBox(NULL, "SIGDN_NORMALDISPLAY failed", NULL, NULL);
								}
								else
								{
									std::string fileName = StringConvert::ws2s(g_name);
									std::string filePath = StringConvert::ws2s(g_path);

									filePath = IO::ReplaceBackSlashes(filePath);

									if (!boost::iequals(IO::GetFileExtension(filePath), "package"))
										filePath += ".package";

									MainWindow::addOnEndUpdateCallback([=]() {
											exportAssets(filePath);
										}
									);
								}
							}

							CoTaskMemFree(g_path);
							CoTaskMemFree(g_name);

							psi->Release();
						}
					}
					pfd->Release();
				}
	#else
				char filename[1024];
				FILE *f = popen("zenity --file-selection --save --file-filter='Package files | *.package'", "r");
				fgets(filename, 1024, f);
				int r = pclose(f);

				if (r == 0)
				{
					std::string filePath = filename;
					int idx = filePath.find("\n");
					if (idx != std::string::npos)
						filePath = filePath.substr(0, idx);

					filePath = IO::ReplaceBackSlashes(filePath);

					if (!boost::iequals(IO::GetFileExtension(filePath), "package"))
						filePath += ".package";

					MainWindow::addOnEndUpdateCallback([=]() {
							exportAssets(filePath);
						}
					);
				}
	#endif
			}
			else
			{
				MainWindow::addOnEndUpdateCallback([=]() {
					importAssets();
				});
			}
		}

		if (closed && !ok)
		{
			visible = false;
		}
	}

	void DialogAssetExporter::listFiles(std::vector<std::string>& outList, TreeNode* htStart)
	{
		for (auto it = htStart->children.begin(); it != htStart->children.end(); ++it)
		{
			std::string path = (*it)->getPath();

			if (mode == Mode::Import ? importEntries.count(path) != 0
				: IO::FileExists(Engine::getSingleton()->getAssetsPath() + CP_SYS(path)))
			{
				if ((*it)->getChecked())
					outList.push_back(path);
			}
			else
				listFiles(outList, *it);
		}
	}

	void DialogAssetExporter::copyTreeView(TreeNode* root, TreeNode* dstRoot)
	{
		for (auto it = root->children.begin(); it != root->children.end(); ++it)
		{
			TreeNode* copy = *it;

			TreeNode* node = new TreeNode(dstRoot->treeView);
			node->alias = copy->alias;
			node->name = GUIDGenerator::genGuid();
			node->icon = copy->icon;
			node->enableDrag = false;
			node->setChecked(true);
			node->setCheckboxEnabled(true);
			node->setEditable(false);
			node->setCheckChildren(true);
			dstRoot->addChild(node);

			copyTreeView(copy, node);
		}
	}

	void DialogAssetExporter::exportAssets(std::string filePath)
	{
		visible = false;
		progressDialog->show();

		std::string ext = IO::GetFileExtension(filePath);

		if (!boost::iequals(ext, "package"))
		{
			progressDialog->hide();
			MainWindow::getConsoleWindow()->log("Error exporting package. Can not write to file of unsupported package format (." + ext + ")", LogMessageType::LMT_ERROR);
			return;
		}

		std::vector<std::string> files;
		listFiles(files, treeView->getRootNode());

		if (files.size() == 0)
		{
			progressDialog->hide();
			MainWindow::getConsoleWindow()->log("Error exporting package. No files specified", LogMessageType::LMT_ERROR);
			return;
		}

		progressDialog->setTitle("Exporting package");
		progressDialog->setStatusText("Packing assets...");

		int err = 0;
		zip_t* _zip = zip_open_z(CP_UNI(filePath).c_str(), ZIP_CREATE | ZIP_TRUNCATE, &err);
		
		if (_zip == nullptr) {
			zip_error_t zerr;
			zip_error_init_with_code(&zerr, err);
			MainWindow::getConsoleWindow()->log("Error exporting package: " + std::string(filePath) + " (" + zip_error_strerror(&zerr) + ")", LogMessageType::LMT_ERROR);
			zip_error_fini(&zerr);
			progressDialog->hide();
			return;
		}

		for (auto it = files.begin(); it != files.end(); ++it)
		{
			if (!validPackagePath(*it))
			{
				MainWindow::getConsoleWindow()->log("Cannot export invalid package path: " + *it, LogMessageType::LMT_ERROR);
				zip_discard(_zip);
				progressDialog->hide();
				return;
			}

			zip_source_t* source = zip_source_file(_zip, (CP_UNI(Engine::getSingleton()->getAssetsPath()) + *it).c_str(), 0, 0);
			if (source == nullptr || zip_file_add(_zip, (*it).c_str(), source, ZIP_FL_ENC_UTF_8) < 0)
			{
				if (source != nullptr) zip_source_free(source);
				MainWindow::getConsoleWindow()->log("Error exporting package: " + std::string(zip_strerror(_zip)), LogMessageType::LMT_ERROR);
				zip_discard(_zip);
				progressDialog->hide();
				return;
			}
		}

		zip_register_progress_callback_with_state(_zip, 0.001, [](zip_t*, double progress, void*) { progressCallback(progress); }, NULL, NULL);
		err = zip_close_z(_zip);

		if (err == 0)
		{
			MainWindow::getConsoleWindow()->log("Assets successfully exported!", LogMessageType::LMT_INFO);
		}
		else
		{
			MainWindow::getConsoleWindow()->log("Error exporting package: " + std::string(zip_strerror(_zip)), LogMessageType::LMT_ERROR);
			zip_discard(_zip);
		}

		progressDialog->hide();
		visible = false;
	}

	void DialogAssetExporter::importAssets()
	{
		visible = false;
		progressDialog->setTitle("Importing package");
		progressDialog->setStatusText("Extracting assets...");
		progressDialog->show();
		std::vector<std::string> files;
		listFiles(files, treeView->getRootNode());
		int err = 0;
		zip_t* archive = zip_open_z(CP_UNI(openedPackage).c_str(), ZIP_RDONLY, &err);
		if (!archive)
		{
			zip_error_t error;
			zip_error_init_with_code(&error, err);
			MainWindow::getConsoleWindow()->log("Failed to open package: " + std::string(zip_error_strerror(&error)), LogMessageType::LMT_ERROR);
			zip_error_fini(&error);
			progressDialog->hide();
			return;
		}

		std::string failure;
		size_t imported = 0, skipped = 0;
		const auto root = std::filesystem::u8path(CP_UNI(Engine::getSingleton()->getAssetsPath()));
		for (const auto& name : files)
		{
			const auto entry = importEntries.find(name);
			if (!validPackagePath(name) || entry == importEntries.end())
			{
				failure = "Invalid package path: " + name;
				break;
			}
			const auto destination = root / std::filesystem::u8path(name);
			std::error_code ec;
			// Reject redirected parents instead of extracting outside Assets.
			auto parent = root;
			for (const auto& part : std::filesystem::u8path(name).parent_path())
			{
				parent /= part;
#ifdef _WIN32
				const DWORD attributes = GetFileAttributesW(parent.c_str());
				if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
#else
				if (std::filesystem::is_symlink(parent, ec))
#endif
				{
					failure = "Redirected asset directory: " + name;
					break;
				}
			}
			if (!failure.empty()) break;
			if (std::filesystem::exists(destination, ec))
			{
				++skipped;
				continue;
			}
			std::filesystem::create_directories(destination.parent_path(), ec);
			if (ec) { failure = "Cannot create directory for " + name + ": " + ec.message(); break; }

			zip_stat_t info;
			if (zip_stat(archive, entry->second.c_str(), ZIP_FL_ENC_UTF_8, &info) != 0)
			{
				failure = "Cannot stat " + name + ": " + zip_strerror(archive);
				break;
			}
			zip_file_t* input = zip_fopen(archive, entry->second.c_str(), ZIP_FL_ENC_UTF_8);
			if (!input) { failure = "Cannot read " + name + ": " + zip_strerror(archive); break; }
#ifdef _WIN32
			int output = _wopen(destination.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_BINARY, 0644);
#else
			int output = open(destination.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
#endif
			if (output < 0)
			{
				failure = "Cannot create " + name + ": " + std::strerror(errno);
				zip_fclose(input);
				break;
			}
			char buffer[65536];
			zip_uint64_t total = 0;
			for (;;)
			{
				const zip_int64_t count = zip_fread(input, buffer, sizeof(buffer));
				if (count < 0) { failure = "Cannot decompress " + name + ": " + zip_file_strerror(input); break; }
				if (count == 0) break; // Read to EOF, including libzip's CRC check.
				if (total > info.size || static_cast<zip_uint64_t>(count) > info.size - total)
				{ failure = "Invalid uncompressed size: " + name; break; }
				zip_int64_t written = 0;
				while (written < count)
				{
					const int n = write(output, buffer + written, static_cast<unsigned int>(count - written));
					if (n < 0 && errno == EINTR) continue;
					if (n <= 0) { failure = "Cannot write " + name + ": " + std::strerror(errno); break; }
					written += n;
				}
				if (!failure.empty()) break;
				total += count;
			}
			if (close(output) != 0 && failure.empty()) failure = "Cannot close output: " + name;
			if (zip_fclose(input) != 0 && failure.empty()) failure = "Cannot close package entry: " + name;
			if (total != info.size && failure.empty()) failure = "Incomplete package entry: " + name;
			if (!failure.empty())
			{
				std::filesystem::remove(destination, ec);
				if (ec) failure += "; cannot remove incomplete file: " + ec.message();
				break;
			}
			++imported;
			progressDialog->setProgress(static_cast<float>(imported + skipped) / files.size());
		}
		zip_discard(archive);
		progressDialog->hide();
		if (failure.empty())
		{
			MainWindow::getSingleton()->onRestore();
			MainWindow::getConsoleWindow()->log("Package import complete: " + std::to_string(imported)
				+ " imported, " + std::to_string(skipped) + " existing files skipped.", LogMessageType::LMT_INFO);
		}
		else
		{
			MainWindow::getConsoleWindow()->log("Package import stopped after " + std::to_string(imported)
				+ " files: " + failure, LogMessageType::LMT_ERROR);
		}
	}

	void DialogAssetExporter::checkNodes(TreeNode* root, bool check)
	{
		root->setChecked(check);

		for (auto it = root->children.begin(); it != root->children.end(); ++it)
			checkNodes(*it, check);
	}

	void DialogAssetExporter::progressCallback(double progress)
	{
		progressDialog->setStatusText("Compressing files...");
		progressDialog->setProgress(progress);
	}
}
