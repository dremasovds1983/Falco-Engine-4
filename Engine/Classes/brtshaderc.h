#pragma once
#include <bgfx/bgfx.h>

// Falco compiler boundary; stage values match shaderc command flags.
namespace shaderc
{
    enum ShaderType { ST_VERTEX = 'v', ST_FRAGMENT = 'f', ST_COMPUTE = 'c' };

    // Compile source text; filePath identifies diagnostics/include context.
    // Pass the returned memory to bgfx::createShader. Null indicates failure.
    const bgfx::Memory* compileShaderFromSources(
        ShaderType type, const char* filePath, const char* src,
        const char* defines = nullptr, const char* varyingSrc = nullptr,
        const char* profile = nullptr);

    // Compile with shaderc-compatible command arguments.
    const bgfx::Memory* compileShader(int argc, const char* argv[]);
}
