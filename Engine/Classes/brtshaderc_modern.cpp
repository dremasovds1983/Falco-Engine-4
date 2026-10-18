#include "brtshaderc.h"

#ifdef FALCO_MODERN_BGFX

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <system_error>
#include <unordered_set>
#include <vector>

namespace
{
    namespace fs = std::filesystem;

    class TemporaryDirectory
    {
    public:
        TemporaryDirectory()
        {
            const fs::path base = fs::temp_directory_path();
            for (uint32_t attempt = 0; attempt < 32; ++attempt)
            {
                const std::wstring name = L"falco-shaderc-"
                    + std::to_wstring(GetCurrentProcessId()) + L"-"
                    + std::to_wstring(GetTickCount64()) + L"-"
                    + std::to_wstring(attempt);
                path = base / name;
                std::error_code error;
                if (fs::create_directory(path, error))
                {
                    return;
                }
            }
            path.clear();
        }

        ~TemporaryDirectory()
        {
            if (!path.empty())
            {
                std::error_code error;
                fs::remove_all(path, error);
            }
        }

        fs::path path;
    };

    fs::path getExecutableDirectory()
    {
        std::vector<wchar_t> buffer(32768, L'\0');
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), DWORD(buffer.size()));
        if (length == 0 || length >= buffer.size())
        {
            return {};
        }
        return fs::path(std::wstring(buffer.data(), length)).parent_path();
    }

    fs::path findShaderCompiler()
    {
        const fs::path executableCandidate = getExecutableDirectory() / L"shadercRelease.exe";
        if (fs::is_regular_file(executableCandidate))
        {
            return executableCandidate;
        }
        return {};
    }

    constexpr uint64_t shaderCacheOffset = 14695981039346656037ull;
    constexpr uint64_t shaderCachePrime = 1099511628211ull;

    void hashBytes(uint64_t& hash, const void* data, size_t size)
    {
        const uint8_t* bytes = static_cast<const uint8_t*>(data);
        for (size_t index = 0; index < size; ++index)
        {
            hash ^= bytes[index];
            hash *= shaderCachePrime;
        }
    }

    void hashString(uint64_t& hash, const std::string& value)
    {
        hashBytes(hash, value.data(), value.size());
        const uint8_t separator = 0xff;
        hashBytes(hash, &separator, sizeof(separator));
    }

    void hashFile(uint64_t& hash, const fs::path& path)
    {
        std::ifstream stream(path, std::ios::binary);
        if (!stream)
        {
            return;
        }

        char buffer[16384];
        while (stream)
        {
            stream.read(buffer, sizeof(buffer));
            const std::streamsize count = stream.gcount();
            if (count > 0)
            {
                hashBytes(hash, buffer, static_cast<size_t>(count));
            }
        }
    }

    void hashIncludeDirectory(uint64_t& hash, const fs::path& directory)
    {
        std::error_code error;
        if (directory.empty() || !fs::is_directory(directory, error))
        {
            return;
        }

        std::vector<fs::path> files;
        fs::recursive_directory_iterator iterator(
            directory, fs::directory_options::skip_permission_denied, error);
        const fs::recursive_directory_iterator end;
        while (!error && iterator != end)
        {
            if (iterator->is_regular_file(error))
            {
                const fs::path extension = iterator->path().extension();
                if (extension == L".sh" || extension == L".sc" || extension == L".shader")
                {
                    files.push_back(iterator->path());
                }
            }
            iterator.increment(error);
        }

        std::sort(files.begin(), files.end());
        for (const fs::path& path : files)
        {
            const fs::path relative = fs::relative(path, directory, error);
            hashString(hash, (error ? path : relative).generic_string());
            error.clear();
            hashFile(hash, path);
        }
    }

    fs::path getShaderCacheDirectory()
    {
        // Build layouts place Editor, Player and ShadercSmoke in sibling folders.
        // Share their cache so the compile gate can prewarm runtime shaders.
        return getExecutableDirectory().parent_path()
            / L"ShaderCache" / L"modern-d3d11-v1";
    }

    fs::path getShaderCachePath(
        const fs::path& compiler,
        const char* typeName,
        const char* selectedProfile,
        const char* defines,
        const std::string& sourceText,
        const char* varyingSrc,
        const char* filePath)
    {
        uint64_t hash = shaderCacheOffset;
        hashString(hash, "falco-modern-shader-cache-v1");
        hashString(hash, typeName != nullptr ? typeName : "");
        hashString(hash, selectedProfile != nullptr ? selectedProfile : "");
        hashString(hash, defines != nullptr ? defines : "");
        hashString(hash, sourceText);
        hashString(hash, varyingSrc != nullptr ? varyingSrc : "");

        std::error_code error;
        const uintmax_t compilerSize = fs::file_size(compiler, error);
        if (!error)
        {
            hashBytes(hash, &compilerSize, sizeof(compilerSize));
        }
        error.clear();
        const auto compilerTime = fs::last_write_time(compiler, error);
        if (!error)
        {
            const auto ticks = compilerTime.time_since_epoch().count();
            hashBytes(hash, &ticks, sizeof(ticks));
        }

        const fs::path executableDirectory = getExecutableDirectory();
        hashIncludeDirectory(hash, executableDirectory / L"Shaders");

        if (filePath != nullptr && filePath[0] != '\0')
        {
            const fs::path origin(filePath);
            const fs::path includeDirectory = origin.has_filename() ? origin.parent_path() : origin;
            if (!includeDirectory.empty() && includeDirectory != includeDirectory.root_path())
            {
                hashIncludeDirectory(hash, includeDirectory);
            }
        }

        wchar_t name[32]{};
        swprintf_s(name, L"%016llx.bin", static_cast<unsigned long long>(hash));
        return getShaderCacheDirectory() / name;
    }

    bool readShaderCache(const fs::path& path, std::vector<uint8_t>& data)
    {
        std::ifstream stream(path, std::ios::binary | std::ios::ate);
        if (!stream)
        {
            return false;
        }
        const std::streamsize size = stream.tellg();
        if (size <= 0 || size > UINT32_MAX)
        {
            return false;
        }
        data.resize(static_cast<size_t>(size));
        stream.seekg(0, std::ios::beg);
        return bool(stream.read(reinterpret_cast<char*>(data.data()), size));
    }

    void writeShaderCache(const fs::path& path, const std::vector<uint8_t>& data)
    {
        static std::mutex cacheMutex;
        std::lock_guard<std::mutex> lock(cacheMutex);

        std::error_code error;
        fs::create_directories(path.parent_path(), error);
        if (error || fs::is_regular_file(path, error))
        {
            return;
        }

        const fs::path temporary = path.wstring() + L".tmp-"
            + std::to_wstring(GetCurrentProcessId()) + L"-"
            + std::to_wstring(GetTickCount64());
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream)
        {
            return;
        }
        stream.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
        stream.close();
        if (!stream)
        {
            fs::remove(temporary, error);
            return;
        }
        fs::rename(temporary, path, error);
        if (error)
        {
            fs::remove(temporary, error);
        }
    }

    std::wstring quoteWindowsArgument(const std::wstring& argument)
    {
        std::wstring quoted = L"\"";
        size_t backslashes = 0;
        for (const wchar_t character : argument)
        {
            if (character == L'\\')
            {
                ++backslashes;
                continue;
            }

            if (character == L'\"')
            {
                quoted.append(backslashes * 2 + 1, L'\\');
                quoted.push_back(L'\"');
                backslashes = 0;
                continue;
            }

            quoted.append(backslashes, L'\\');
            backslashes = 0;
            quoted.push_back(character);
        }
        quoted.append(backslashes * 2, L'\\');
        quoted.push_back(L'\"');
        return quoted;
    }

    bool runShaderCompiler(
        const fs::path& executable,
        const std::vector<std::wstring>& arguments,
        const fs::path& diagnosticPath,
        std::string& diagnostics)
    {
        std::wstring commandLine = quoteWindowsArgument(executable.wstring());
        for (const std::wstring& argument : arguments)
        {
            commandLine.push_back(L' ');
            commandLine += quoteWindowsArgument(argument);
        }

        std::vector<wchar_t> mutableCommand(commandLine.begin(), commandLine.end());
        mutableCommand.push_back(L'\0');

        SECURITY_ATTRIBUTES securityAttributes{};
        securityAttributes.nLength = sizeof(securityAttributes);
        securityAttributes.bInheritHandle = TRUE;
        HANDLE diagnosticHandle = CreateFileW(
            diagnosticPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
            &securityAttributes, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (diagnosticHandle == INVALID_HANDLE_VALUE)
        {
            return false;
        }
        HANDLE inputHandle = CreateFileW(
            L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
            &securityAttributes, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (inputHandle == INVALID_HANDLE_VALUE)
        {
            CloseHandle(diagnosticHandle);
            return false;
        }

        STARTUPINFOW startupInfo{};
        startupInfo.cb = sizeof(startupInfo);
        startupInfo.dwFlags = STARTF_USESTDHANDLES;
        startupInfo.hStdInput = inputHandle;
        startupInfo.hStdOutput = diagnosticHandle;
        startupInfo.hStdError = diagnosticHandle;
        PROCESS_INFORMATION processInfo{};
        if (!CreateProcessW(
                executable.c_str(), mutableCommand.data(), nullptr, nullptr, TRUE,
                CREATE_NO_WINDOW, nullptr, nullptr, &startupInfo, &processInfo))
        {
            CloseHandle(diagnosticHandle);
            CloseHandle(inputHandle);
            return false;
        }
        CloseHandle(diagnosticHandle);
        CloseHandle(inputHandle);

        constexpr DWORD shaderCompilerTimeoutMs = 120000;
        const DWORD waitResult = WaitForSingleObject(processInfo.hProcess, shaderCompilerTimeoutMs);
        if (waitResult != WAIT_OBJECT_0)
        {
            if (waitResult == WAIT_TIMEOUT)
            {
                TerminateProcess(processInfo.hProcess, ERROR_TIMEOUT);
                WaitForSingleObject(processInfo.hProcess, 5000);
            }
            CloseHandle(processInfo.hThread);
            CloseHandle(processInfo.hProcess);
            return false;
        }

        DWORD exitCode = 1;
        const BOOL exitCodeRead = GetExitCodeProcess(processInfo.hProcess, &exitCode);
        CloseHandle(processInfo.hThread);
        CloseHandle(processInfo.hProcess);
        std::ifstream diagnosticStream(diagnosticPath, std::ios::binary);
        if (diagnosticStream)
        {
            diagnostics.assign(
                std::istreambuf_iterator<char>(diagnosticStream),
                std::istreambuf_iterator<char>());
        }

        return exitCodeRead != FALSE && exitCode == 0;
    }

    bool writeTextFile(const fs::path& path, const char* text)
    {
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        if (!stream)
        {
            return false;
        }
        stream << (text == nullptr ? "" : text) << '\n';
        return stream.good();
    }

    const char* shaderTypeName(shaderc::ShaderType type)
    {
        switch (type)
        {
        case shaderc::ST_VERTEX: return "vertex";
        case shaderc::ST_FRAGMENT: return "fragment";
        case shaderc::ST_COMPUTE: return "compute";
        default: return nullptr;
        }
    }

    void eraseAll(std::string& text, const char* value)
    {
        const size_t valueLength = std::char_traits<char>::length(value);
        for (size_t position = text.find(value);
             position != std::string::npos;
             position = text.find(value, position))
        {
            text.erase(position, valueLength);
        }
    }

    unsigned samplerRegister(const std::string& name, unsigned declarationIndex)
    {
        if (name == "u_albedoMap") return 0;
        if (name == "u_depthMap") return 1;
	if (name == "u_lightBuffer" || name == "u_forwardColor") return 2;
        if (name == "u_lightMap" || name == "u_forwardDepth") return 3;
        if (name == "u_normalMap") return 4;
        if (name == "u_mraMap") return 5;
        if (name == "u_bloomMap") return 2;
        if (name == "u_brdfMap") return 6;
        if (name == "u_envMap") return 9;
        if (name == "u_irradianceMap") return 10;
        if (name == "u_prefilteredMap") return 11;
        return declarationIndex;
    }

    void rewriteLegacySamplerDeclarations(std::string& text)
    {
        constexpr const char* sampler2D = "uniform sampler2D";
        constexpr const char* samplerCube = "uniform samplerCube";
        size_t searchPosition = 0;
        unsigned declarationIndex = 0;

        for (;;)
        {
            const size_t sampler2DPosition = text.find(sampler2D, searchPosition);
            const size_t samplerCubePosition = text.find(samplerCube, searchPosition);
            const bool isCube = samplerCubePosition < sampler2DPosition;
            const size_t declarationPosition = isCube ? samplerCubePosition : sampler2DPosition;
            if (declarationPosition == std::string::npos)
            {
                break;
            }

            const char* declarationPrefix = isCube ? samplerCube : sampler2D;
            size_t nameStart = declarationPosition + std::char_traits<char>::length(declarationPrefix);
            while (nameStart < text.size() && (text[nameStart] == ' ' || text[nameStart] == '\t'))
            {
                ++nameStart;
            }
            size_t nameEnd = nameStart;
            while (nameEnd < text.size()
                && ((text[nameEnd] >= 'a' && text[nameEnd] <= 'z')
                    || (text[nameEnd] >= 'A' && text[nameEnd] <= 'Z')
                    || (text[nameEnd] >= '0' && text[nameEnd] <= '9')
                    || text[nameEnd] == '_'))
            {
                ++nameEnd;
            }
            const size_t semicolon = text.find(';', nameEnd);
            if (nameEnd == nameStart || semicolon == std::string::npos)
            {
                searchPosition = nameEnd;
                continue;
            }

            const std::string name = text.substr(nameStart, nameEnd - nameStart);
            const std::string replacement = std::string(isCube ? "SAMPLERCUBE(" : "SAMPLER2D(")
                + name + ", " + std::to_string(samplerRegister(name, declarationIndex)) + ");";
            text.replace(declarationPosition, semicolon - declarationPosition + 1, replacement);
            searchPosition = declarationPosition + replacement.size();
            ++declarationIndex;
        }
    }

    void moveShaderDirectivesToPrelude(std::string& text)
    {
        std::string directives;
        std::string body;
        size_t lineStart = 0;
        while (lineStart < text.size())
        {
            const size_t lineEnd = text.find('\n', lineStart);
            const size_t contentEnd = lineEnd == std::string::npos ? text.size() : lineEnd;
            std::string line = text.substr(lineStart, contentEnd - lineStart);
            if (!line.empty() && line.back() == '\r')
            {
                line.pop_back();
            }

            const size_t firstToken = line.find_first_not_of(" \t");
            const bool isDirective = firstToken != std::string::npos
                && (line.compare(firstToken, 6, "$input") == 0
                    || line.compare(firstToken, 7, "$output") == 0);
            if (isDirective)
            {
                directives += line.substr(firstToken);
                directives.push_back('\n');
            }
            else
            {
                body += line;
                body.push_back('\n');
            }

            if (lineEnd == std::string::npos)
            {
                break;
            }
            lineStart = lineEnd + 1;
        }

        text = directives + body;
    }

    bool rewriteSingleArgumentConstructors(
        std::string& text,
        const char* constructor,
        const char* replacement)
    {
        const size_t constructorLength = std::char_traits<char>::length(constructor);
        bool rewritten = false;
        size_t searchPosition = 0;
        while ((searchPosition = text.find(constructor, searchPosition)) != std::string::npos)
        {
            const bool tokenStart = searchPosition == 0
                || !((text[searchPosition - 1] >= 'a' && text[searchPosition - 1] <= 'z')
                    || (text[searchPosition - 1] >= 'A' && text[searchPosition - 1] <= 'Z')
                    || (text[searchPosition - 1] >= '0' && text[searchPosition - 1] <= '9')
                    || text[searchPosition - 1] == '_');
            const size_t openParen = searchPosition + constructorLength;
            if (!tokenStart || openParen >= text.size() || text[openParen] != '(')
            {
                searchPosition = openParen;
                continue;
            }

            size_t depth = 1;
            bool hasTopLevelComma = false;
            size_t closeParen = openParen + 1;
            for (; closeParen < text.size() && depth > 0; ++closeParen)
            {
                if (text[closeParen] == '(')
                {
                    ++depth;
                }
                else if (text[closeParen] == ')')
                {
                    --depth;
                }
                else if (text[closeParen] == ',' && depth == 1)
                {
                    hasTopLevelComma = true;
                }
            }
            if (depth != 0)
            {
                break;
            }

            if (!hasTopLevelComma)
            {
                text.replace(searchPosition, constructorLength, replacement);
                searchPosition += std::char_traits<char>::length(replacement);
                rewritten = true;
            }
            else
            {
                searchPosition = closeParen;
            }
        }
        return rewritten;
    }

    const char* defaultProfile(shaderc::ShaderType type)
    {
        switch (type)
        {
        case shaderc::ST_VERTEX:
        case shaderc::ST_FRAGMENT:
        case shaderc::ST_COMPUTE:
            return "s_5_0";
        default: return nullptr;
        }
    }

    void writeShaderFailure(
        const char* filePath,
        const char* typeName,
        const char* defines,
        const std::string& diagnostics)
    {
        static std::mutex failureMutex;
        static std::unordered_set<size_t> reportedFailures;
        static bool logInitialized = false;

        std::lock_guard<std::mutex> lock(failureMutex);
        const fs::path logPath = getExecutableDirectory() / L"shaderc-errors.log";
        if (!logInitialized)
        {
            std::error_code error;
            fs::remove(logPath, error);
            logInitialized = true;
        }

        const std::string identity = std::string(filePath != nullptr ? filePath : "<memory>")
            + "\n" + (typeName != nullptr ? typeName : "<unknown>")
            + "\n" + (defines != nullptr ? defines : "")
            + "\n" + diagnostics;
        const size_t identityHash = std::hash<std::string>{}(identity);
        if (!reportedFailures.insert(identityHash).second)
        {
            return;
        }

        std::ofstream log(logPath, std::ios::binary | std::ios::app);
        if (!log)
        {
            return;
        }
        log << "=== SHADERC FAILURE ===\n"
            << "source: " << (filePath != nullptr ? filePath : "<memory>") << '\n'
            << "stage: " << (typeName != nullptr ? typeName : "<unknown>") << '\n'
            << "defines: " << (defines != nullptr ? defines : "") << '\n'
            << diagnostics;
        if (diagnostics.empty() || diagnostics.back() != '\n')
        {
            log << '\n';
        }
        log << "=== END SHADERC FAILURE ===\n";
    }
}

namespace shaderc
{
    const bgfx::Memory* compileShaderFromSources(
        ShaderType type,
        const char* filePath,
        const char* src,
        const char* defines,
        const char* varyingSrc,
        const char* profile)
    {
        const fs::path compiler = findShaderCompiler();
        const char* typeName = shaderTypeName(type);
        const char* selectedProfile = profile != nullptr && profile[0] != '\0'
            ? profile
            : defaultProfile(type);
        if (compiler.empty() || typeName == nullptr || selectedProfile == nullptr || src == nullptr)
        {
            return nullptr;
        }

        const std::wstring compilerDiagnostic = L"Falco shader compiler: " + compiler.wstring() + L"\n";
        OutputDebugStringW(compilerDiagnostic.c_str());

        std::string sourceText(src);
        if (sourceText.find("bgfx_shader.sh") == std::string::npos)
        {
            eraseAll(sourceText, "uniform mat4 u_modelViewProj;\n");
            eraseAll(sourceText, "uniform mat4 u_model[32];\n");
            eraseAll(sourceText, "uniform mat4 u_viewProj;\n");
            eraseAll(sourceText, "uniform mat4 u_invProj;\n");
            rewriteLegacySamplerDeclarations(sourceText);
            moveShaderDirectivesToPrelude(sourceText);

            bool needsConstructorHelpers = false;
            needsConstructorHelpers |= rewriteSingleArgumentConstructors(sourceText, "vec2", "falco_vec2");
            needsConstructorHelpers |= rewriteSingleArgumentConstructors(sourceText, "vec3", "falco_vec3");
            needsConstructorHelpers |= rewriteSingleArgumentConstructors(sourceText, "vec4", "falco_vec4");
            needsConstructorHelpers |= rewriteSingleArgumentConstructors(sourceText, "mat3", "falco_mat3");

            size_t preludePosition = 0;
            while (preludePosition < sourceText.size() && sourceText[preludePosition] == '$')
            {
                const size_t lineEnd = sourceText.find('\n', preludePosition);
                preludePosition = lineEnd == std::string::npos ? sourceText.size() : lineEnd + 1;
            }

            std::string compatibilityPrelude = "#include <bgfx_shader.sh>\n";
            if (needsConstructorHelpers)
            {
                compatibilityPrelude +=
                    "vec2 falco_vec2(float value) { return vec2_splat(value); }\n"
                    "vec2 falco_vec2(vec2 value) { return value; }\n"
                    "vec3 falco_vec3(float value) { return vec3_splat(value); }\n"
                    "vec3 falco_vec3(vec3 value) { return value; }\n"
                    "vec4 falco_vec4(float value) { return vec4_splat(value); }\n"
                    "vec4 falco_vec4(vec4 value) { return value; }\n"
                    "mat3 falco_mat3(mat3 value) { return value; }\n"
                    "mat3 falco_mat3(mat4 value) { return mat3(value[0].xyz, value[1].xyz, value[2].xyz); }\n";
            }
            sourceText.insert(preludePosition, compatibilityPrelude);
        }

        const fs::path cachePath = getShaderCachePath(
            compiler, typeName, selectedProfile, defines, sourceText, varyingSrc, filePath);
        std::vector<uint8_t> cachedData;
        if (readShaderCache(cachePath, cachedData))
        {
            return bgfx::copy(cachedData.data(), static_cast<uint32_t>(cachedData.size()));
        }

        TemporaryDirectory temporary;
        if (temporary.path.empty())
        {
            return nullptr;
        }

        const fs::path sourcePath = temporary.path / L"shader.sc";
        const fs::path varyingPath = temporary.path / L"varying.def.sc";
        const fs::path outputPath = temporary.path / L"shader.bin";
        const fs::path diagnosticPath = temporary.path / L"shaderc.log";
        if (!writeTextFile(sourcePath, sourceText.c_str()) || !writeTextFile(varyingPath, varyingSrc))
        {
            return nullptr;
        }

        std::vector<std::wstring> arguments = {
            L"-f", sourcePath.wstring(),
            L"-o", outputPath.wstring(),
            L"--type", fs::path(typeName).wstring(),
            L"--platform", L"windows",
            L"--profile", fs::path(selectedProfile).wstring(),
            L"--varyingdef", varyingPath.wstring(),
            L"-i", temporary.path.wstring()
        };

        if (defines != nullptr && defines[0] != '\0')
        {
            arguments.emplace_back(L"--define");
            arguments.push_back(fs::path(defines).wstring());
        }

        if (filePath != nullptr && filePath[0] != '\0')
        {
            const fs::path origin(filePath);
            const fs::path includeDirectory = origin.has_filename() ? origin.parent_path() : origin;
            if (!includeDirectory.empty() && fs::is_directory(includeDirectory))
            {
                arguments.emplace_back(L"-i");
                arguments.push_back(includeDirectory.wstring());
            }
        }

        const fs::path executableShaders = getExecutableDirectory() / L"Shaders";
        if (fs::is_directory(executableShaders))
        {
            arguments.emplace_back(L"-i");
            arguments.push_back(executableShaders.wstring());
        }

        const fs::path builtinShaders = getExecutableDirectory() / L"BuiltinResources" / L"Shaders";
        if (fs::is_directory(builtinShaders))
        {
            arguments.emplace_back(L"-i");
            arguments.push_back(builtinShaders.wstring());
        }

        std::string diagnostics;
        if (!runShaderCompiler(compiler, arguments, diagnosticPath, diagnostics) || !fs::is_regular_file(outputPath))
        {
            writeShaderFailure(filePath, typeName, defines, diagnostics);
            if (!diagnostics.empty())
            {
                std::fputs(diagnostics.c_str(), stderr);
                if (diagnostics.back() != '\n')
                {
                    std::fputc('\n', stderr);
                }
            }
            return nullptr;
        }

        std::ifstream output(outputPath, std::ios::binary | std::ios::ate);
        if (!output)
        {
            return nullptr;
        }
        const std::streamsize size = output.tellg();
        if (size <= 0 || size > UINT32_MAX)
        {
            return nullptr;
        }

        std::vector<uint8_t> data(static_cast<size_t>(size));
        output.seekg(0, std::ios::beg);
        if (!output.read(reinterpret_cast<char*>(data.data()), size))
        {
            return nullptr;
        }
        writeShaderCache(cachePath, data);
        return bgfx::copy(data.data(), static_cast<uint32_t>(data.size()));
    }

    const bgfx::Memory* compileShader(int, const char*[])
    {
        return nullptr;
    }
}

#endif // FALCO_MODERN_BGFX
