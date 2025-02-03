#pragma once

#include "zip.h"

// clang-format off
#include "../rend/TexCache.h"
#include "../rend/CustomTexture.h"
// clang-format on

class GdxsvEmbedTextureSource : public ICustomTextureSource {
   public:
	~GdxsvEmbedTextureSource() override;
	bool Init() override;
	bool LoadMap() override;
	void Terminate() override;
	u8* LoadCustomTexture(u32 hash, int& width, int& height) override;
	u8* LoadExtraTexture(const char* name, bool v_flip, int& width, int& height) const;

   private:
	bool initialized = false;
	bool custom_textures_available = false;
	std::string textures_path;
	std::map<u32, std::string> texture_map;
};

class GdxsvTexturePackSource : public ICustomTextureSource {
   public:
	~GdxsvTexturePackSource() override;
	bool Init() override;
	bool LoadMap() override;
	void Terminate() override;
	u8* LoadCustomTexture(u32 hash, int& width, int& height) override;
	u8* LoadExtraTexture(const char* name, bool v_flip, int& width, int& height) const;

   private:
	bool initialized = false;
	bool custom_textures_available = false;
	std::map<u32, int> texture_map;
	FILE* texp_file = nullptr;
	zip_t* texp_zip = nullptr;
	zip_source_t* texp_zip_source = nullptr;
};

extern GdxsvTexturePackSource gdxsv_texture_pack_source;
extern GdxsvEmbedTextureSource gdxsv_embed_texture_source;
