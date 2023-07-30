#include "gdxsv_key_display.h"

#include "gdxsv.h"
#include "types.h"
#include "libs.h"
#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"
#include "rend/imgui_driver.h"
#include "rend/transform_matrix.h"
#include "gdxsv_CustomTexture.h"

const char* ButtonsTextureName = "BUTTONS.png";
const char* ArrowsTextureName = "ARROWS.png";
ImTextureID buttonsTexture = ImTextureID{};
ImTextureID arrowsTexture = ImTextureID{};


namespace {
	void getArrowsUV(int keyCode, ImVec2& uv0, ImVec2& uv1);
	float getScale();
}

void GdxsvKeyDisplay::DisplayOSD() {
	static bool once = false;
	if (!once) {
		once = true;
		AppendInput(0, KeyInput{RIGHT, 100});
		AppendInput(0, KeyInput{LEFT, 200});
		AppendInput(0, KeyInput{DOWN, 201});
		AppendInput(0, KeyInput{UP, 300});
		AppendInput(0, KeyInput{UP | RIGHT, 300});
		AppendInput(0, KeyInput{UP | RIGHT | JUMP, 300});
		AppendInput(0, KeyInput{UP | RIGHT | JUMP | COMBAT, 300});
	}

	if (gdxsv.Disk() == 2) {
		if (gdxsv_ReadMem16(0x0c3d1cd4 + 0x0210 + 4)) {
			AppendInput(0, KeyInput{ gdxsv_ReadMem16(0x0c3d1cd4 + 0x0210), 0 });
		}
	}

	if (buttonsTexture == ImTextureID{} || buttonsTexture != imguiDriver->getTexture(ButtonsTextureName))
	{
		int w, h;
		u8* imgData = gdx_custom_texture.LoadExtraTexture(ButtonsTextureName, false, w, h);

		if (imgData) {
            buttonsTexture = imguiDriver->updateTexture(ButtonsTextureName, imgData, w, h);
		}
	}

	if (arrowsTexture == ImTextureID{} || arrowsTexture != imguiDriver->getTexture(ArrowsTextureName))
	{
		int w, h;
		u8* imgData = gdx_custom_texture.LoadExtraTexture(ArrowsTextureName, false, w, h);

		if (imgData) {
            arrowsTexture = imguiDriver->updateTexture(ArrowsTextureName, imgData, w, h);
		}
	}

	const float w = ImGui::GetIO().DisplaySize.x;
	const float h = ImGui::GetIO().DisplaySize.y;
	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
	ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x / 2.f, ImGui::GetIO().DisplaySize.y / 2.f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
	ImGui::SetNextWindowSize(ImVec2(w, h));
	ImGui::SetNextWindowBgAlpha(0.0f);
	ImGui::Begin("##gdxsv_keydisplay", NULL,
		ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoScrollbar);

	float scale = getScale();
	auto size = ImVec2(24 * scale, 24 * scale);

	for (const auto& e : history[0]) {
		ImVec2 uv0, uv1;

		getArrowsUV(e.code, uv0, uv1);
		ImGui::Image(arrowsTexture, size, uv0, uv1);

        uv0 = ImVec2{0, 0};
        uv1 = ImVec2{0, 1};
		if (e.code & SHOOT) {
			uv0.x = 0.f / 6;
			uv1.x = 1.f / 6;
			ImGui::SameLine();
            ImGui::Image(buttonsTexture, size, uv0, uv1);
		}
		if (e.code & COMBAT) {
			uv0.x = 1.f / 6;
			uv1.x = 2.f / 6;
			ImGui::SameLine();
            ImGui::Image(buttonsTexture, size, uv0, uv1);
		}
		if (e.code & JUMP) {
			uv0.x = 2.f / 6;
			uv1.x = 3.f / 6;
			ImGui::SameLine();
            ImGui::Image(buttonsTexture, size, uv0, uv1);
		}
		if (e.code & SEARCH) {
			uv0.x = 3.f / 6;
			uv1.x = 4.f / 6;
			ImGui::SameLine();
            ImGui::Image(buttonsTexture, size, uv0, uv1);
		}
		if (e.code & COOP) {
			uv0.x = 4.f / 6;
			uv1.x = 5.f / 6;
			ImGui::SameLine();
            ImGui::Image(buttonsTexture, size, uv0, uv1);
		}
	}

	ImGui::End();
	ImGui::PopStyleVar();
	ImGui::PopStyleVar();
		
}

void GdxsvKeyDisplay::AppendInput(int player, KeyInput input) {
	verify(0 <= player && player < 4);
	history[player].push_front(input);
	if (20 <= history[player].size()) history[player].pop_back();
}

void GdxsvKeyDisplay::SetCurrentFrame(int frame) {
	current_frame = frame;
}

namespace {
float getScale() {
	const float w = ImGui::GetIO().DisplaySize.x;
	const float h = ImGui::GetIO().DisplaySize.y;
	const float renderAR = getOutputFramebufferAspectRatio();
	const float screenAR = w / h;
	float dx = 0;
	float dy = 0;
	if (renderAR > screenAR)
		dy = h * (1 - screenAR / renderAR) / 2;
	else
		dx = w * (1 - renderAR / screenAR) / 2;

	return std::min((w - dx * 2) / 640.f, (h - dy * 2) / 480.f);
}

void getArrowsUV(int keyCode, ImVec2& uv0, ImVec2& uv1) {
    uv0 = ImVec2{ 1.f / 3, 1.f / 3 };
    uv1 = ImVec2{ 2.f / 3, 2.f / 3 };

	if (keyCode & GdxsvKeyDisplay::UP) {
		if (keyCode & GdxsvKeyDisplay::LEFT) {
			uv0 = ImVec2{0.f/3, 0.f/3};
			uv1 = ImVec2{1.f/3, 1.f/3};
		} else if (keyCode & GdxsvKeyDisplay::RIGHT) {
			uv0 = ImVec2{2.f/3, 0.f/3};
			uv1 = ImVec2{3.f/3, 1.f/3};
		} else {
			uv0 = ImVec2{1.f/3, 0.f/3};
			uv1 = ImVec2{2.f/3, 1.f/3};
		}
	} else if (keyCode & GdxsvKeyDisplay::DOWN) {
		if (keyCode & GdxsvKeyDisplay::LEFT) {
			uv0 = ImVec2{0.f/3, 2.f/3};
			uv1 = ImVec2{1.f/3, 3.f/3};
		} else if (keyCode & GdxsvKeyDisplay::RIGHT) {
			uv0 = ImVec2{2.f/3, 2.f/3};
			uv1 = ImVec2{3.f/3, 3.f/3};
		} else {
			uv0 = ImVec2{1.f/3, 2.f/3};
			uv1 = ImVec2{2.f/3, 3.f/3};
		}
	} else if (keyCode & GdxsvKeyDisplay::LEFT) {
        uv0 = ImVec2{ 0.f / 3, 1.f / 3 };
        uv1 = ImVec2{ 1.f / 3, 2.f / 3 };
	} else if (keyCode & GdxsvKeyDisplay::RIGHT) {
        uv0 = ImVec2{ 2.f / 3, 1.f / 3 };
        uv1 = ImVec2{ 3.f / 3, 2.f / 3 };
	}
}

}


