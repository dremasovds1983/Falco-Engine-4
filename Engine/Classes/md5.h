#pragma once
#ifdef FALCO_MODERN_BGFX
#include <string>
// Stable lowercase MD5 identifier for existing caches and file formats.
std::string md5(const std::string text);
#else
#include "md5_legacy.h"
#endif
