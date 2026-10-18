#pragma once

// bx removed the legacy uint32_t.h convenience header. Falco Player only
// includes it for bx's base integer declarations and does not use the removed
// uint32_* helpers, so the modern build can use bx.h directly.
#include <bx/bx.h>
