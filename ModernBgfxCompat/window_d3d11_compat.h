#pragma once

#include <SDL2/SDL.h>

// Window.cpp is shared with the legacy OpenGL configuration. For the modern
// D3D11 build, remove the GL-only window flag/context calls without changing
// the legacy translation unit stored in its original Windows encoding.
#define SDL_WINDOW_OPENGL 0
#define SDL_GL_CreateContext(window) nullptr
#define SDL_GL_SetSwapInterval(interval) 0
#define SDL_GL_GetDrawableSize(window, width, height) SDL_GetWindowSize(window, width, height)
