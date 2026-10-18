#include "../Engine/Classes/brtshaderc.h"
#include "../Engine/Renderer/SystemShaders/ShadowCaster.h"
#include "../Engine/Renderer/SystemShaders/OutlineShader.h"
#include "../Engine/Renderer/SystemShaders/SimpleShader.h"
#include "../Engine/Renderer/SystemShaders/SimpleTextureShader.h"
#include "../Engine/Renderer/SystemShaders/TerrainTreeBillboardShader.h"
#include "../Engine/Renderer/SystemShaders/TerrainTreeBillboardLightShader.h"
#include "../Engine/Renderer/SystemShaders/TransparentShader.h"
#include "../Engine/Renderer/SystemShaders/DeferredCombine.h"
#include "../Engine/Renderer/SystemShaders/OutlineFinalShader.h"
#include "../Engine/Renderer/SystemShaders/CameraBackBuffer.h"
#include "../Engine/Renderer/SystemShaders/FXAA.h"
#include "../Engine/Renderer/SystemShaders/Presentation.h"
#include "../Engine/Renderer/SystemShaders/Bloom.h"
#include "../Engine/Renderer/SystemShaders/SSR.h"
#include "../Engine/Renderer/SystemShaders/Irradiance.h"
#include "../Engine/Renderer/SystemShaders/Skybox.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <regex>
#include <sstream>
#include <utility>
#include <vector>

namespace bgfx
{
    const Memory* copy(const void* data, uint32_t size)
    {
        static std::vector<uint8_t> storage;
        static Memory memory{};
        const uint8_t* bytes = static_cast<const uint8_t*>(data);
        storage.assign(bytes, bytes + size);
        memory.data = storage.data();
        memory.size = static_cast<uint32_t>(storage.size());
        return &memory;
    }
}

namespace
{
    namespace fs = std::filesystem;

    bool hasHeader(const bgfx::Memory* memory, char a, char b, char c)
    {
        return memory != nullptr
            && memory->size >= 3
            && memory->data[0] == static_cast<uint8_t>(a)
            && memory->data[1] == static_cast<uint8_t>(b)
            && memory->data[2] == static_cast<uint8_t>(c);
    }

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

    bool writeBinary(const fs::path& path, const bgfx::Memory* memory)
    {
        if (memory == nullptr || memory->data == nullptr || memory->size == 0)
        {
            return false;
        }

        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.write(
            reinterpret_cast<const char*>(memory->data),
            static_cast<std::streamsize>(memory->size));
        return output.good();
    }

    std::string readText(const fs::path& path)
    {
        std::ifstream input(path, std::ios::binary);
        std::ostringstream text;
        text << input.rdbuf();
        return input.good() || input.eof() ? text.str() : std::string();
    }

    std::string extractShaderSection(const std::string& source, const char* sectionName)
    {
        const size_t namePos = source.find(sectionName);
        const size_t openPos = namePos == std::string::npos ? namePos : source.find('{', namePos);
        if (openPos == std::string::npos)
        {
            return {};
        }

        int depth = 1;
        for (size_t cursor = openPos + 1; cursor < source.size(); ++cursor)
        {
            if (source[cursor] == '{')
            {
                ++depth;
            }
            else if (source[cursor] == '}' && --depth == 0)
            {
                return source.substr(openPos + 1, cursor - openPos - 1);
            }
        }
        return {};
    }

    std::vector<std::string> extractShaderBlocks(const std::string& source, const char* blockName)
    {
        std::vector<std::string> blocks;
        size_t lineStart = 0;
        while (lineStart < source.size())
        {
            const size_t lineEnd = source.find('\n', lineStart);
            size_t wordStart = lineStart;
            while (wordStart < source.size() && (source[wordStart] == ' ' || source[wordStart] == '\t' || source[wordStart] == '\r'))
            {
                ++wordStart;
            }

            const size_t nameLength = std::char_traits<char>::length(blockName);
            const size_t afterName = wordStart + nameLength;
            if (source.compare(wordStart, nameLength, blockName) == 0
                && (afterName >= source.size() || !(std::isalnum(static_cast<unsigned char>(source[afterName])) || source[afterName] == '_')))
            {
                const size_t openPos = source.find('{', afterName);
                if (openPos == std::string::npos)
                {
                    break;
                }

                int depth = 1;
                for (size_t cursor = openPos + 1; cursor < source.size(); ++cursor)
                {
                    if (source[cursor] == '{')
                    {
                        ++depth;
                    }
                    else if (source[cursor] == '}' && --depth == 0)
                    {
                        blocks.push_back(source.substr(openPos + 1, cursor - openPos - 1));
                        lineStart = cursor + 1;
                        break;
                    }
                }
                continue;
            }

            lineStart = lineEnd == std::string::npos ? source.size() : lineEnd + 1;
        }
        return blocks;
    }

    std::vector<std::pair<std::string, bool>> extractBoolDefines(const std::string& source)
    {
        const std::regex declaration(R"(define\s+bool\s+"[^"]*"\s+([A-Za-z_][A-Za-z0-9_]*)\s+(true|false))");
        std::vector<std::pair<std::string, bool>> result;
        for (std::sregex_iterator it(source.begin(), source.end(), declaration), end; it != end; ++it)
        {
            result.emplace_back((*it)[1].str(), (*it)[2].str() == "true");
        }
        return result;
    }

    std::string makeDefines(const std::vector<std::pair<std::string, bool>>& declarations, int overrideValue)
    {
        std::string result;
        for (const auto& declaration : declarations)
        {
            const bool value = overrideValue < 0 ? declaration.second : overrideValue != 0;
            result += declaration.first + "=" + (value ? "1;" : "0;");
        }
        return result;
    }

    bool checkAssetShaderVariant(const fs::path& shaderPath, const char* variant, const char* defines)
    {
        const std::string source = readText(shaderPath);
        const std::vector<std::string> passBlocks = extractShaderBlocks(source, "pass");
        const std::string origin = shaderPath.string();
        if (passBlocks.empty())
        {
            std::cerr << "Could not parse asset shader smoke input: " << origin << ".\n";
            return false;
        }

        std::string varying;
        std::string vertex;
        std::string fragment;
        for (size_t passIndex = 0; passIndex < passBlocks.size(); ++passIndex)
        {
            const std::string passVarying = extractShaderSection(passBlocks[passIndex], "varying");
            const std::string passVertex = extractShaderSection(passBlocks[passIndex], "vertex");
            const std::string passFragment = extractShaderSection(passBlocks[passIndex], "fragment");
            if (!passVarying.empty()) varying = passVarying;
            if (!passVertex.empty()) vertex = passVertex;
            if (!passFragment.empty()) fragment = passFragment;
            if (varying.empty() || vertex.empty() || fragment.empty())
            {
                std::cerr << "Incomplete asset shader pass: " << variant << " pass " << passIndex << ".\n";
                return false;
            }

            const bgfx::Memory* vertexBinary = shaderc::compileShaderFromSources(
                shaderc::ST_VERTEX, origin.c_str(), vertex.c_str(), defines, varying.c_str(), "s_5_0");
            const bool vertexPassed = hasHeader(vertexBinary, 'V', 'S', 'H');
            const bgfx::Memory* fragmentBinary = shaderc::compileShaderFromSources(
                shaderc::ST_FRAGMENT, origin.c_str(), fragment.c_str(), defines, varying.c_str(), "s_5_0");
            if (!vertexPassed || !hasHeader(fragmentBinary, 'F', 'S', 'H'))
            {
                std::cerr << "Asset shader smoke failed: " << variant << " pass " << passIndex << ".\n";
                return false;
            }
        }

        std::cout << "Asset shader smoke passed: " << variant << ".\n";
        return true;
    }

    bool checkSystemShader(
        const char* name,
        const std::string& vertex,
        const std::string& fragment,
        const std::string& varying)
    {
        const bgfx::Memory* vertexBinary = shaderc::compileShaderFromSources(
            shaderc::ST_VERTEX, name, vertex.c_str(), nullptr, varying.c_str(), "s_5_0");
        if (!hasHeader(vertexBinary, 'V', 'S', 'H'))
        {
            std::cerr << "System vertex shader smoke failed: " << name << ".\n";
            return false;
        }

        const bgfx::Memory* fragmentBinary = shaderc::compileShaderFromSources(
            shaderc::ST_FRAGMENT, name, fragment.c_str(), nullptr, varying.c_str(), "s_5_0");
        if (!hasHeader(fragmentBinary, 'F', 'S', 'H'))
        {
            std::cerr << "System fragment shader smoke failed: " << name << ".\n";
            return false;
        }

        std::cout << "System shader smoke passed: " << name << ".\n";
        return true;
    }
}

int main()
{
    const char* varying =
        "vec2 v_texcoord0 : TEXCOORD0 = vec2(0.0, 0.0);\n"
        "vec3 a_position : POSITION;\n"
        "vec2 a_texcoord0 : TEXCOORD0;\n";
    const char* vertex =
        "$input a_position, a_texcoord0\n"
        "$output v_texcoord0\n"
        "#include <bgfx_shader.sh>\n"
        "void main()\n"
        "{\n"
        "    gl_Position = mul(u_modelViewProj, vec4(a_position, 1.0));\n"
        "    v_texcoord0 = a_texcoord0;\n"
        "}\n";
    const char* fragment =
        "$input v_texcoord0\n"
        "#include <bgfx_shader.sh>\n"
        "void main()\n"
        "{\n"
        "    gl_FragColor = vec4(v_texcoord0, 0.0, 1.0);\n"
        "}\n";

    const bgfx::Memory* vertexBinary = shaderc::compileShaderFromSources(
        shaderc::ST_VERTEX, "shaderc_smoke_vs.sc", vertex, nullptr, varying, "s_5_0");
    if (!hasHeader(vertexBinary, 'V', 'S', 'H'))
    {
        std::cerr << "Vertex shader smoke failed.\n";
        return 1;
    }
    const uint32_t vertexSize = vertexBinary->size;
    const fs::path outputRoot = getExecutableDirectory();
    if (outputRoot.empty() || !writeBinary(outputRoot / L"shaderc_smoke_vs.bin", vertexBinary))
    {
        std::cerr << "Could not save vertex shader smoke output.\n";
        return 3;
    }

    const bgfx::Memory* fragmentBinary = shaderc::compileShaderFromSources(
        shaderc::ST_FRAGMENT, "shaderc_smoke_fs.sc", fragment, nullptr, varying, "s_5_0");
    if (!hasHeader(fragmentBinary, 'F', 'S', 'H'))
    {
        std::cerr << "Fragment shader smoke failed.\n";
        return 2;
    }
    if (!writeBinary(outputRoot / L"shaderc_smoke_fs.bin", fragmentBinary))
    {
        std::cerr << "Could not save fragment shader smoke output.\n";
        return 4;
    }

    bool systemShadersPassed = true;
    const std::string legacyPreludeVertex =
        "\n\t$input a_position, a_texcoord0\n"
        "\t$output v_texcoord0\n"
        "void main()\n"
        "{\n"
        "    mat3 normalMatrix = mat3(u_model[0]);\n"
        "    vec3 scalarOffset = vec3(0.0);\n"
        "    gl_Position = mul(u_modelViewProj, vec4(a_position + mul(normalMatrix, scalarOffset), 1.0));\n"
        "    v_texcoord0 = a_texcoord0;\n"
        "}\n";
    const std::string legacyPreludeFragment =
        "\r\n    $input v_texcoord0\r\n"
        "void main()\r\n"
        "{\r\n"
        "    vec3 scalarColor = vec3(0.5);\r\n"
        "    gl_FragColor = vec4(v_texcoord0, scalarColor.x, 1.0);\r\n"
        "}\r\n";
    systemShadersPassed &= checkSystemShader("LegacyDirectivePrelude", legacyPreludeVertex, legacyPreludeFragment, varying);
    systemShadersPassed &= checkSystemShader("ShadowCaster", shaders::shadowCasterVertex, shaders::shadowCasterFragment, shaders::shadowCasterVarying);
    systemShadersPassed &= checkSystemShader("Outline", shaders::outlineVertex, shaders::outlineFragment, shaders::outlineVarying);
    systemShadersPassed &= checkSystemShader("Simple", shaders::simpleVertex, shaders::simpleFragment, shaders::simpleVarying);
    systemShadersPassed &= checkSystemShader("SimpleTexture", shaders::simpleTextureVertex, shaders::simpleTextureFragment, shaders::simpleTextureVarying);
    systemShadersPassed &= checkSystemShader("TerrainTreeCapture", shaders::terrainTreeBillboardLightVertex, shaders::terrainTreeCaptureFragment, shaders::terrainTreeBillboardLightVarying);
    systemShadersPassed &= checkSystemShader("TerrainTreeBillboard", shaders::terrainTreeBillboardLightVertex, shaders::terrainTreeBillboardFragment, shaders::terrainTreeBillboardLightVarying);
    systemShadersPassed &= checkSystemShader("TerrainTreeBillboardLight", shaders::terrainTreeBillboardLightVertex, shaders::terrainTreeBillboardLightFragment, shaders::terrainTreeBillboardLightVarying);
    systemShadersPassed &= checkSystemShader("Transparent", shaders::transparentVertex, shaders::transparentFragment, shaders::transparentVarying);
    systemShadersPassed &= checkSystemShader("DeferredCombine", shaders::deferredCombineVertex, shaders::deferredCombineFragment, shaders::deferredCombineVarying);
    systemShadersPassed &= checkSystemShader("OutlineFinal", shaders::outlineFinalVertex, shaders::outlineFinalFragment, shaders::outlineFinalVarying);
    systemShadersPassed &= checkSystemShader("CameraBackBuffer", shaders::cameraBackBufferVertex, shaders::cameraBackBufferFragment, shaders::cameraBackBufferVarying);
    systemShadersPassed &= checkSystemShader("FXAA", shaders::fxaaVertex, shaders::fxaaFragment, shaders::fxaaVarying);
    systemShadersPassed &= checkSystemShader("SSR", shaders::deferredCombineVertex, shaders::ssrFragment, shaders::deferredCombineVarying);
    systemShadersPassed &= checkSystemShader("Irradiance", shaders::deferredCombineVertex, shaders::irradianceFragment, shaders::deferredCombineVarying);
    systemShadersPassed &= checkSystemShader("Bloom", shaders::deferredCombineVertex, shaders::bloomFragment, shaders::deferredCombineVarying);
    systemShadersPassed &= checkSystemShader("Presentation", shaders::deferredCombineVertex, shaders::presentationFragment, shaders::deferredCombineVarying);
    systemShadersPassed &= checkSystemShader("Skybox", shaders::skyboxVertex, shaders::skyboxFragment, shaders::skyboxVarying);
    const fs::path repositoryRoot = outputRoot.parent_path().parent_path().parent_path().parent_path();
    const fs::path standardDeferred = repositoryRoot / L"Res/BuiltinResources/Shaders/StandardDeferred.shader";
    const fs::path builtinShaderRoot = standardDeferred.parent_path();
    for (const fs::directory_entry& entry : fs::directory_iterator(builtinShaderRoot))
    {
        if (!entry.is_regular_file() || entry.path().extension() != L".shader")
        {
            continue;
        }

        const std::string source = readText(entry.path());
        const std::vector<std::pair<std::string, bool>> declarations = extractBoolDefines(source);
        const std::string label = entry.path().filename().string();
        const std::string defaults = makeDefines(declarations, -1);
        systemShadersPassed &= checkAssetShaderVariant(entry.path(), (label + " defaults").c_str(), defaults.c_str());
        if (!declarations.empty())
        {
            const std::string allOff = makeDefines(declarations, 0);
            const std::string allOn = makeDefines(declarations, 1);
            systemShadersPassed &= checkAssetShaderVariant(entry.path(), (label + " all-off").c_str(), allOff.c_str());
            systemShadersPassed &= checkAssetShaderVariant(entry.path(), (label + " all-on").c_str(), allOn.c_str());
        }
    }
    systemShadersPassed &= checkAssetShaderVariant(
        standardDeferred,
        "StandardDeferred rifle",
        "CUTOUT=0;HAS_ALBEDO_MAP=1;HAS_NORMAL_MAP=0;HAS_METALLIC_MAP=0;HAS_ROUGHNESS_MAP=0;HAS_AO_MAP=1;HAS_EMISSION_MAP=0;DOUBLE_SIDED=1;");
    systemShadersPassed &= checkAssetShaderVariant(
        standardDeferred,
        "StandardDeferred hands normal map",
        "CUTOUT=0;HAS_ALBEDO_MAP=1;HAS_NORMAL_MAP=1;HAS_METALLIC_MAP=0;HAS_ROUGHNESS_MAP=0;HAS_AO_MAP=0;HAS_EMISSION_MAP=0;DOUBLE_SIDED=1;");
    // Mixed Terrain variants exercise cache reflection even without layer samplers.
    systemShadersPassed &= checkAssetShaderVariant(
        builtinShaderRoot / L"StandardTerrain.shader", "Terrain GI without layers",
        "HAS_ALBEDO=0;HAS_NORMAL_MAP=0;ENABLE_GI=1;");
    systemShadersPassed &= checkAssetShaderVariant(
        builtinShaderRoot / L"StandardTerrain.shader", "Terrain GI normal layers only",
        "HAS_ALBEDO=0;HAS_NORMAL_MAP=1;ENABLE_GI=1;");
    if (!systemShadersPassed)
    {
        return 5;
    }

    std::cout << "Shaderc smoke passed: VSH=" << vertexSize
              << " bytes, FSH=" << fragmentBinary->size << " bytes.\n";
    return 0;
}
