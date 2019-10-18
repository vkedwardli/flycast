/*
    Created on: Oct 18, 2019

	Copyright 2019 flyinghead

	This file is part of Flycast.

    Flycast is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    Flycast is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Flycast.  If not, see <https://www.gnu.org/licenses/>.
*/
#include "build.h"
#if defined(USE_SDL) && HOST_OS != OS_DARWIN
#include "gl_context.h"
#include "rend/gui.h"
#include "sdl/sdl.h"

SDLGLGraphicsContext theGLContext;

bool SDLGLGraphicsContext::Init()
{
#ifdef GLES
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
#else
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
#endif

	SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
	SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
	SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
	SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
	SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
	SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
	SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

	sdl_recreate_window(SDL_WINDOW_OPENGL);

	glcontext = SDL_GL_CreateContext(window);
	if (!glcontext)
	{
		ERROR_LOG(RENDERER, "Error creating SDL GL context");
		SDL_DestroyWindow(window);
		window = nullptr;
		return false;
	}
	SDL_GL_MakeCurrent(window, NULL);

	SDL_GL_GetDrawableSize(window, &screen_width, &screen_height);

	float ddpi, hdpi, vdpi;
	if (!SDL_GetDisplayDPI(SDL_GetWindowDisplayIndex(window), &ddpi, &hdpi, &vdpi))
		screen_dpi = (int)roundf(max(hdpi, vdpi));

	INFO_LOG(RENDERER, "Created SDL Window and GL Context successfully");

	SDL_GL_MakeCurrent(window, glcontext);

#ifdef GLES
	return true;
#else
	return gl3wInit() != -1 && gl3wIsSupported(3, 1);
#endif
}

void SDLGLGraphicsContext::Swap()
{
	SDL_GL_SwapWindow(window);

	/* Check if drawable has been resized */
	SDL_GL_GetDrawableSize(window, &screen_width, &screen_height);
}

void SDLGLGraphicsContext::Term()
{
	if (glcontext != nullptr)
	{
		SDL_GL_DeleteContext(glcontext);
		glcontext = nullptr;
	}
}

#endif
