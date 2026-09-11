// gdxsv: a no-op ImGui driver for headless test runs (gdxsv:headless=yes).
// Part of Flycast; distributed under the GNU GPL v2 or later, see the
// repository LICENSE.
#pragma once
#include "imgui_driver.h"
#include <string>
#include <unordered_map>

// ImGui driver for headless runs (gdxsv:headless=yes). There is no graphics
// API behind it: the font atlas is built on the CPU so ImGui frames can be
// produced, textures get stable fake ids so callers that cache them behave
// as they do with a real driver, and nothing is ever drawn.
class NullImGuiDriver final : public ImGuiDriver
{
public:
	void newFrame() override
	{
		ImGuiIO& io = ImGui::GetIO();
		if (!io.Fonts->IsBuilt())
			io.Fonts->Build();
	}

	void renderDrawData(ImDrawData *drawData, bool gui_open) override {
	}

	void present() override {
	}

	ImTextureID getTexture(const std::string& name) override
	{
		auto it = textures.find(name);
		return it == textures.end() ? ImTextureID() : it->second;
	}

	ImTextureID updateTexture(const std::string& name, const u8 *data, int width, int height, bool nearestSampling) override
	{
		ImTextureID& id = textures[name];
		if (id == ImTextureID())
			id = nextId++;
		return id;
	}

	void deleteTexture(const std::string& name) override {
		textures.erase(name);
	}

private:
	std::unordered_map<std::string, ImTextureID> textures;
	ImTextureID nextId = 1;
};
