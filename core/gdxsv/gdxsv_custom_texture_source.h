#pragma once

#include "zip.h"

// clang-format off
#include "../rend/TexCache.h"
#include "../rend/CustomTexture.h"
// clang-format on

class GdxsvEmbedTextureSource : public BaseCustomTextureSource {
   public:
	~GdxsvEmbedTextureSource() override;
	bool loadMap() override;
	void terminate() override;
	u8* loadCustomTexture(u32 hash, int& width, int& height) override;
	bool isTextureReplaced(u32 hash) override;
	u8* LoadExtraTexture(const char* name, bool v_flip, int& width, int& height) const;

   private:
	bool init();
	bool initialized = false;
	bool custom_textures_available = false;
	std::string textures_path;
	std::map<u32, std::string> texture_map;
};

class GdxsvTexturePackSource : public BaseCustomTextureSource {
   public:
	~GdxsvTexturePackSource() override;
	bool loadMap() override;
	void terminate() override;
	u8* loadCustomTexture(u32 hash, int& width, int& height) override;
	bool isTextureReplaced(u32 hash) override;
	u8* LoadExtraTexture(const char* name, bool v_flip, int& width, int& height) const;

   private:
	bool init();
	bool initialized = false;
	bool custom_textures_available = false;
	std::map<u32, int> texture_map;
	FILE* texp_file = nullptr;
	zip_t* texp_zip = nullptr;
	zip_source_t* texp_zip_source = nullptr;
};

extern GdxsvTexturePackSource gdxsv_texture_pack_source;
extern GdxsvEmbedTextureSource gdxsv_embed_texture_source;
