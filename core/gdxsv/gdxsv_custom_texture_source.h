#pragma once

#include "zip.h"

// clang-format off
#include "../rend/TexCache.h"
#include "../rend/CustomTexture.h"
// clang-format on

class GdxsvEmbedTextureSource : public BaseCustomTextureSource {
   public:
	GdxsvEmbedTextureSource();
	bool shouldReplace() const override { return !langmodDisabled() && custom_textures_available; }
	bool shouldPreload() const override { return shouldReplace(); }
	bool loadMap() override;
	size_t getTextureCount() const override { return texture_map.size(); }
	u8* loadCustomTexture(u32 hash, int& width, int& height) override;
	bool isTextureReplaced(u32 hash) override;
	void preloadTextures(TextureCallback callback, std::atomic<bool>* stop_flag) override;
	static u8* LoadExtraTexture(const char* name, bool v_flip, int& width, int& height);

   private:
	static bool langmodDisabled();
	bool custom_textures_available = false;
	std::string textures_path;
	std::map<u32, std::string> texture_map;
};

class GdxsvTexturePackSource : public BaseCustomTextureSource {
   public:
	GdxsvTexturePackSource();
	~GdxsvTexturePackSource() override;
	bool shouldReplace() const override { return config::GdxUseTexturePack && custom_textures_available; }
	bool shouldPreload() const override { return shouldReplace(); }
	bool loadMap() override;
	size_t getTextureCount() const override { return texture_map.size(); }
	void terminate() override;
	u8* loadCustomTexture(u32 hash, int& width, int& height) override;
	bool isTextureReplaced(u32 hash) override;

   private:
	bool custom_textures_available = false;
	std::map<u32, int> texture_map;
	FILE* texp_file = nullptr;
	zip_t* texp_zip = nullptr;
	zip_source_t* texp_zip_source = nullptr;
};
