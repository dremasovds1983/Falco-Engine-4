#pragma once

// bgfx moved the public platform API into bgfx/bgfx.h.
// Keep this shim only on the FalcoModernBgfx include path so legacy builds
// continue to consume their original bgfx/platform.h.
#include <bgfx/bgfx.h>
