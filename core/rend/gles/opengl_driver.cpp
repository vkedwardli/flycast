/*
	Copyright 2021 flyinghead

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
#include "opengl_driver.h"
#include "imgui_impl_opengl3.h"
#include "wsi/gl_context.h"
#include "glcache.h"
#include "gles.h"
#include "hw/pvr/Renderer_if.h"

// TEMPDBG begin: four-screen OpenGL darkness after the session ends (new Spout)
#include "imgui_internal.h"
#include <cstdlib>
#include <functional>
#include <thread>
#ifdef USE_SDL
#include <SDL.h>
#endif
int gdxsv_tempdbg_gui_frames = 3;
static unsigned tempdbg_gui_count;

void gdxsv_tempdbg_gl(const char* where)
{
	GLint drawFb = -1, readFb = -1, prog = -1, active = -1, tex = -1, sampler = -1, vp[4] = {}, src_rgb = -1, dst_rgb = -1, src_a = -1,
		  dst_a = -1, eq = -1, drawBuf = -1, readBuf = -1, samples = -1, unpackAlign = -1, unpackRow = -1;
	GLfloat bc[4] = {}, cc[4] = {};
	GLboolean cm[4] = {};
	while (glGetError() != GL_NO_ERROR) {}
	glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFb);
	glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFb);
	glGetIntegerv(GL_VIEWPORT, vp);
	glGetIntegerv(GL_BLEND_SRC_RGB, &src_rgb);
	glGetIntegerv(GL_BLEND_DST_RGB, &dst_rgb);
	glGetIntegerv(GL_BLEND_SRC_ALPHA, &src_a);
	glGetIntegerv(GL_BLEND_DST_ALPHA, &dst_a);
	glGetIntegerv(GL_BLEND_EQUATION_RGB, &eq);
	glGetFloatv(GL_BLEND_COLOR, bc);
	glGetFloatv(GL_COLOR_CLEAR_VALUE, cc);
	glGetBooleanv(GL_COLOR_WRITEMASK, cm);
	glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
	glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
	glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex);
	glGetIntegerv(0x8919 /* GL_SAMPLER_BINDING */, &sampler);
	glGetIntegerv(0x0C01 /* GL_DRAW_BUFFER */, &drawBuf);
	glGetIntegerv(0x0C02 /* GL_READ_BUFFER */, &readBuf);
	glGetIntegerv(0x80A9 /* GL_SAMPLES */, &samples);
	glGetIntegerv(GL_UNPACK_ALIGNMENT, &unpackAlign);
	glGetIntegerv(0x0CF2 /* GL_UNPACK_ROW_LENGTH */, &unpackRow);
	const int scissor = glIsEnabled(GL_SCISSOR_TEST), blend = glIsEnabled(GL_BLEND), srgb = glIsEnabled(0x8DB9 /* FRAMEBUFFER_SRGB */),
			  cov = glIsEnabled(0x80A0), a2c = glIsEnabled(0x809E), ms = glIsEnabled(0x809D), smask = glIsEnabled(0x8E51),
			  depth = glIsEnabled(GL_DEPTH_TEST), stencil = glIsEnabled(GL_STENCIL_TEST), cull = glIsEnabled(GL_CULL_FACE),
			  discard = glIsEnabled(0x8C89 /* RASTERIZER_DISCARD */);
	const GLenum err = glGetError();
	void* ctx = nullptr;
#ifdef USE_SDL
	ctx = SDL_GL_GetCurrentContext();
#endif
	NOTICE_LOG(RENDERER,
			   "TEMPDBG gl %s: thread %zx ctx %p drawFb %d readFb %d drawBuf %x readBuf %x vp %d,%d %dx%d scissor %d blend %d func %x %x %x %x eq %x "
			   "bcolor %.2f %.2f %.2f %.2f clear %.2f %.2f %.2f %.2f mask %d%d%d%d srgb %d prog %d active %x tex %d sampler %d cov %d a2c %d ms %d "
			   "smask %d samples %d depth %d stencil %d cull %d discard %d unpack %d/%d | cache tex %u prog %u blend %d scissor %d | err %x",
			   where, std::hash<std::thread::id>{}(std::this_thread::get_id()) & 0xffff, ctx, drawFb, readFb, drawBuf, readBuf, vp[0], vp[1], vp[2],
			   vp[3], scissor, blend, src_rgb, dst_rgb, src_a, dst_a, eq, bc[0], bc[1], bc[2], bc[3], cc[0], cc[1], cc[2], cc[3], cm[0], cm[1],
			   cm[2], cm[3], srgb, prog, active, tex, sampler, cov, a2c, ms, smask, samples, depth, stencil, cull, discard, unpackAlign, unpackRow,
			   glcache.TempDbgTexture(), glcache.TempDbgProgram(), glcache.TempDbgBlend(), glcache.TempDbgScissor(), err);
}

static void tempdbgPixels(const char* where)
{
	GLint readFb = 0, readBuf = 0;
	glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFb);
	glGetIntegerv(0x0C02, &readBuf);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
	glReadBuffer(GL_BACK);
	const int w = settings.display.width, h = settings.display.height;
	const int pts[5][2] = {{w / 2, h / 2}, {w / 4, h / 4}, {3 * w / 4, h / 4}, {w / 4, 3 * h / 4}, {3 * w / 4, 3 * h / 4}};
	char buf[256];
	int n = 0;
	for (const auto& p : pts) {
		u8 px[4] = {};
		glReadPixels(p[0], p[1], 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
		n += snprintf(buf + n, sizeof(buf) - n, " (%d,%d)=%d,%d,%d,%d", p[0], p[1], px[0], px[1], px[2], px[3]);
	}
	glBindFramebuffer(GL_READ_FRAMEBUFFER, readFb);
	glReadBuffer(readBuf);
	NOTICE_LOG(RENDERER, "TEMPDBG pixels %s:%s err %x", where, buf, glGetError());
}

static void tempdbgImgui(ImDrawData* drawData)
{
	ImGuiContext& g = *ImGui::GetCurrentContext();
	const ImVec4 bg = ImGui::GetStyle().Colors[ImGuiCol_WindowBg];
	NOTICE_LOG(RENDERER, "TEMPDBG imgui: style alpha %.2f windowBg %.2f %.2f %.2f %.2f, %d draw lists, %d vertices", ImGui::GetStyle().Alpha, bg.x,
			   bg.y, bg.z, bg.w, drawData->CmdListsCount, drawData->TotalVtxCount);
	for (ImGuiWindow* w : g.Windows) {
		if (!w->Active && !w->WasActive) continue;
		NOTICE_LOG(RENDERER, "TEMPDBG imgui window '%s' active %d hidden %d pos %.0f,%.0f size %.0fx%.0f flags %x cmds %d", w->Name, w->Active,
				   w->Hidden, w->Pos.x, w->Pos.y, w->Size.x, w->Size.y, w->Flags, w->DrawList ? w->DrawList->CmdBuffer.Size : -1);
	}
	for (int i = 0; i < drawData->CmdListsCount; ++i) {
		const ImDrawList* dl = drawData->CmdLists[i];
		NOTICE_LOG(RENDERER, "TEMPDBG imgui list %d '%s': %d cmds %d vtx, first tex %p", i, dl->_OwnerName ? dl->_OwnerName : "", dl->CmdBuffer.Size,
				   dl->VtxBuffer.Size, dl->CmdBuffer.Size > 0 ? (void*)dl->CmdBuffer[0].GetTexID() : nullptr);
	}
}
// TEMPDBG end

#ifndef GL_CLAMP_TO_BORDER
#define GL_CLAMP_TO_BORDER 0x812D
#endif
#ifndef GL_TEXTURE_BORDER_COLOR
#define GL_TEXTURE_BORDER_COLOR 0x1004
#endif

OpenGLDriver::OpenGLDriver()
{
	ImGui_ImplOpenGL3_Init();
}

OpenGLDriver::~OpenGLDriver()
{
	std::vector<GLuint> texIds;
	texIds.reserve(textures.size());
	for (const auto& it : textures)
		texIds.push_back((GLuint)(uintptr_t)it.second);
	if (!texIds.empty())
		glcache.DeleteTextures(texIds.size(), &texIds[0]);
	ImGui_ImplOpenGL3_Shutdown();
}

void OpenGLDriver::newFrame()
{
	ImGui_ImplOpenGL3_NewFrame();
}

void OpenGLDriver::renderDrawData(ImDrawData* drawData, bool gui_open)
{
	// TEMPDBG: every 240th GUI frame, and the first frames after a four-screen session ends
	const bool tempdbg = gui_open && (gdxsv_tempdbg_gui_frames > 0 || tempdbg_gui_count++ % 240 == 0);
	if (tempdbg && gdxsv_tempdbg_gui_frames > 0) --gdxsv_tempdbg_gui_frames;
	if (tempdbg) gdxsv_tempdbg_gl("gui before");
	if (gui_open)
	{
#ifndef TARGET_IPHONE
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
#endif
		glcache.Disable(GL_SCISSOR_TEST);
		glcache.ClearColor(0, 0, 0, 0);
		glClear(GL_COLOR_BUFFER_BIT);
		if (renderer != nullptr)
			renderer->RenderLastFrame();
	}
	if (tempdbg) tempdbgPixels("after last frame");
	ImGui_ImplOpenGL3_RenderDrawData(drawData);
	if (tempdbg) {
		gdxsv_tempdbg_gl("gui after");
		tempdbgPixels("after imgui");
		tempdbgImgui(drawData);
	}
	if (gui_open)
		frameRendered = true;
}

// TEMPDBG: glcache.Reset() after the four-screen receivers are released, when GDXSV_TEMPDBG_GLCACHE_RESET=1
void gdxsv_tempdbg_glcache_reset()
{
	const char* env = std::getenv("GDXSV_TEMPDBG_GLCACHE_RESET");
	const bool on = env && env[0] == '1';
	NOTICE_LOG(RENDERER, "TEMPDBG glcache reset %s", on ? "applied" : "off");
	if (on) glcache.Reset();
}

void OpenGLDriver::present()
{
	if (frameRendered)
		GLGraphicsContext::Instance()->swap();
	frameRendered = false;
}

ImTextureID OpenGLDriver::updateTexture(const std::string& name, const u8 *data, int width, int height, bool nearestSampling)
{
	ImTextureID oldId = getTexture(name);
	if (oldId != ImTextureID())
		glcache.DeleteTextures(1, (GLuint *)&oldId);
	GLuint texId = glcache.GenTexture();
	glcache.BindTexture(GL_TEXTURE_2D, texId);
	if (nearestSampling) {
		glcache.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glcache.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	}
	else {
		glcache.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glcache.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	}
    if (gl.border_clamp_supported)
	{
		float color[] = { 0.0f, 0.0f, 0.0f, 0.0f };
		glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, color);
		glcache.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
		glcache.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
	}
	else
	{
		glcache.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glcache.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	}
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);

    return textures[name] = (ImTextureID)(u64)texId;
}

void OpenGLDriver::deleteTexture(const std::string& name)
{
	auto it = textures.find(name);
	if (it != textures.end()) {
		glcache.DeleteTextures(1, (GLuint *)&it->second);
		textures.erase(it);
	}
}
