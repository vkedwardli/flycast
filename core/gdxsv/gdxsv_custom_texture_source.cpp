#ifdef _WIN32
#define _AMD64_	 // Fixing GitHub runner's winnt.h error
#endif

#include "gdxsv_custom_texture_source.h"

#include <stb_image.h>

#include "gdxsv_translation.h"
#include "oslib/storage.h"
#include <zip.h>
#include <zipint.h>

#ifdef __APPLE__
#include <mach-o/dyld.h>

#include <regex>

#include "oslib/directory.h"
#elif _WIN32
#include <winbase.h>
#include <winnls.h>
#include <winuser.h>
#endif

GdxsvTexturePackSource gdxsv_texture_pack_source;
GdxsvEmbedTextureSource gdxsv_embed_texture_source;

static std::string trim_colon_prefix(const std::string& str) {
	const auto pos = str.find(':');
	if (pos != std::string::npos) return str.substr(pos + 1);
	return str;
}

static std::string get_game_id() {
	std::string game_id(settings.content.gameId);
	const size_t str_end = game_id.find_last_not_of(' ');
	if (str_end == std::string::npos) return "";
	game_id = game_id.substr(0, str_end + 1);
	std::replace(game_id.begin(), game_id.end(), ' ', '_');
	return game_id;
}

GdxsvEmbedTextureSource ::~GdxsvEmbedTextureSource() { texture_map.clear(); }

bool GdxsvEmbedTextureSource ::Init() {
	if (GdxsvLanguage::Language() == GdxsvLanguage::Lang::Disabled) return false;

	if (!initialized) {
		initialized = true;
		std::string game_id = get_game_id();
		if (game_id == "T13306M") {
#ifdef __APPLE__
			uint32_t bufSize = PATH_MAX + 1;
			char result[bufSize];
			if (_NSGetExecutablePath(result, &bufSize) == 0) {
				textures_path = std::string(result);
				textures_path.replace(textures_path.find("MacOS/Flycast"), sizeof("MacOS/Flycast") - 1, "Resources/Textures/");
			}

			DIR* dir = flycast::opendir(textures_path.c_str());
			if (dir != nullptr) {
				INFO_LOG(RENDERER, "Found custom textures directory: %s", textures_path.c_str());
				custom_textures_available = true;
				closedir(dir);
			}
#elif _WIN32
			custom_textures_available = true;
#endif
			// TODO: Linux
		}
	}

	return custom_textures_available;
}

void GdxsvEmbedTextureSource ::Terminate() {
	initialized = false;
	custom_textures_available = false;
	textures_path.clear();
	texture_map.clear();
}

#ifdef _WIN32
static BOOL CALLBACK StaticEnumRCNamesFunc(HMODULE hModule, LPCTSTR lpType, LPTSTR lpName, LONG_PTR lParam) {
	// Only add target language's hash & resource index into texture_map
	auto mapping = reinterpret_cast<std::map<u32, std::string>*>(lParam);
	const auto name = std::string(lpName);
	auto lang_dir = GdxsvLanguage::TextureDirectoryName();
	std::transform(lang_dir.begin(), lang_dir.end(), lang_dir.begin(), ::toupper);

	if (name.find(lang_dir) == 0 || name.find("COMMON") == 0) {
		auto tex_hash = name.substr(name.find_last_of('_') + 1);
		u32 hash = strtoul(tex_hash.c_str(), NULL, 16);
		mapping->emplace(hash, name);
	}

	return true;
}
#endif

bool GdxsvEmbedTextureSource ::LoadMap() {
	std::map<u32, std::string> mapping;

	if (GdxsvLanguage::Language() != GdxsvLanguage::Lang::Disabled) {
		// Normal image files by language
		if (!textures_path.empty()) {
			hostfs::DirectoryTree tree(textures_path + GdxsvLanguage::TextureDirectoryName());
			for (const hostfs::FileInfo& item : tree) {
				std::string extension = get_file_extension(item.name);
				if (extension != "jpg" && extension != "jpeg" && extension != "png") continue;
				std::string::size_type dotpos = item.name.find_last_of('.');
				std::string basename = item.name.substr(0, dotpos);
				char* endptr;
				u32 hash = (u32)strtoll(basename.c_str(), &endptr, 16);
				if (endptr - basename.c_str() < (ptrdiff_t)basename.length()) {
					INFO_LOG(RENDERER, "Invalid hash %s", basename.c_str());
					continue;
				}
				mapping.emplace(hash, item.path);
			}
		}

#if _WIN32
		// Embed textures (mainly localization)
		const auto ret = EnumResourceNames(GetModuleHandle(NULL), "GDXSV_TEXTURE", (ENUMRESNAMEPROC)&StaticEnumRCNamesFunc,
										   reinterpret_cast<LONG_PTR>(&mapping));
		if (!ret) {
			ERROR_LOG(COMMON, "EnumResourceNames error:%d", GetLastError());
		}
#endif
	}

	texture_map = mapping;
	return custom_textures_available = !texture_map.empty();
}

u8* GdxsvEmbedTextureSource ::LoadCustomTexture(u32 hash, int& width, int& height) {
	const auto it = texture_map.find(hash);
	if (it == texture_map.end()) return nullptr;

#if _WIN32
	// Load texture from EnumResource
	HRSRC source = FindResourceA(GetModuleHandle(NULL), it->second.c_str(), "GDXSV_TEXTURE");
	if (source != NULL) {
		unsigned int size = SizeofResource(NULL, source);
		HGLOBAL memory = LoadResource(NULL, source);

		if (memory != NULL) {
			void* data = LockResource(memory);

			int n;
			stbi_set_flip_vertically_on_load(1);
			u8* imgData = stbi_load_from_memory(static_cast<unsigned char*>(data), size, &width, &height, &n, STBI_rgb_alpha);

			FreeResource(memory);
			return imgData;
		}
	}
	return nullptr;

#else
	// Load texture from file
	FILE* file = nowide::fopen(it->second.c_str(), "rb");
	if (file == nullptr) return nullptr;
	int n;
	stbi_set_flip_vertically_on_load(1);
	u8* imgData = stbi_load_from_file(file, &width, &height, &n, STBI_rgb_alpha);
	std::fclose(file);
	return imgData;
#endif
}

u8* GdxsvEmbedTextureSource ::LoadExtraTexture(const char* name, bool v_flip, int& width, int& height) const {
#ifdef _WIN32
	u8* imgData = nullptr;
	std::string upper_name = name;
	std::transform(upper_name.begin(), upper_name.end(), upper_name.begin(), ::toupper);
	if (upper_name.find_last_of("PNG") != std::string::npos) {
		upper_name = upper_name.substr(0, upper_name.find_last_of('.'));
	}
	HRSRC source = FindResourceA(GetModuleHandle(NULL), ("EXTRA_" + upper_name).c_str(), "GDXSV_TEXTURE");
	if (source != NULL) {
		unsigned int size = SizeofResource(NULL, source);
		HGLOBAL memory = LoadResource(NULL, source);

		if (memory != NULL) {
			void* data = LockResource(memory);

			int n;
			stbi_set_flip_vertically_on_load((int)v_flip);
			imgData = stbi_load_from_memory(static_cast<unsigned char*>(data), size, &width, &height, &n, STBI_rgb_alpha);

			FreeResource(memory);
		}
	}
	return imgData;

#else

	FILE* file = nowide::fopen((textures_path + "/Extra/" + name).c_str(), "rb");
	if (file == nullptr) return nullptr;
	int n;
	stbi_set_flip_vertically_on_load((int)v_flip);
	u8* imgData = stbi_load_from_file(file, &width, &height, &n, STBI_rgb_alpha);
	std::fclose(file);
	return imgData;

#endif
}

GdxsvTexturePackSource::~GdxsvTexturePackSource() {
	if (texp_zip != nullptr) zip_close(texp_zip);
	if (texp_file != nullptr) std::fclose(texp_file);
	texp_file = nullptr;
	texp_zip = nullptr;
	texp_zip_source = nullptr;
}

bool GdxsvTexturePackSource::Init() {
	if (!config::GdxUseTexturePack) return false;

	if (!initialized) {
		initialized = true;
		std::string game_id = get_game_id();
		if (game_id == "T13306M") {
			custom_textures_available = true;
		}
	}

	return custom_textures_available;
}

void GdxsvTexturePackSource::Terminate() {
	if (texp_zip != nullptr) zip_close(texp_zip);
	if (texp_file != nullptr) std::fclose(texp_file);
	initialized = false;
	custom_textures_available = false;
	texture_map.clear();
	texp_file = nullptr;
	texp_zip = nullptr;
	texp_zip_source = nullptr;
}

bool GdxsvTexturePackSource::LoadMap() {
	if (texp_zip != nullptr) zip_close(texp_zip);
	if (texp_file != nullptr) std::fclose(texp_file);
	texp_file = nullptr;
	texp_zip = nullptr;
	texp_zip_source = nullptr;

	std::map<u32, int> mapping;

	// Zipped texture pack
	const auto texture_pack_path = get_writable_data_path(get_game_id() + ".texp");
	if (config::GdxUseTexturePack && file_exists(texture_pack_path)) do {
			auto zip_file = nowide::fopen(texture_pack_path.c_str(), "rb");
			if (zip_file == nullptr) {
				ERROR_LOG(COMMON, "LoadMap: fopen zip_path failure");
				break;
			}

			zip_error_t error;
			zip_source_t* source = zip_source_filep_create(zip_file, 0, -1, &error);
			if (source == nullptr) {
				ERROR_LOG(COMMON, "LoadMap: zip_source_filep_create failure: %s", error.str);
				std::fclose(zip_file);
				break;
			}

			const auto zip = zip_open_from_source(source, ZIP_RDONLY, &error);
			if (zip == nullptr) {
				ERROR_LOG(COMMON, "LoadMap: zip_open_from_source failure: %s", error.str);
				std::fclose(zip_file);
				zip_source_free(source);
				break;
			}

#ifdef GDXSV_TEXTURE_PACK_PASS
			zip_set_default_password(zip, GDXSV_TEXTURE_PACK_PASS);
#endif
#
			const auto num_entries = zip_get_num_entries(zip, 0);
			if (num_entries < 0) {
				ERROR_LOG(COMMON, "LoadMap: zip_get_num_entries failure");
				std::fclose(zip_file);
				zip_source_free(source);
				break;
			}

			this->texp_file = zip_file;
			this->texp_zip = zip;
			this->texp_zip_source = source;

			for (int i = 0; i < num_entries; i++) {
				std::string name(zip_get_name(zip, i, 0));
				if (name.empty()) {
					ERROR_LOG(COMMON, "LoadMap: zip_get_name");
					break;
				}

				std::string extension = get_file_extension(name);
				if (extension != "jpg" && extension != "jpeg" && extension != "png") continue;
				std::string::size_type dotpos = name.find_last_of('.');
				std::string::size_type slashpos = name.find_last_of('/');
				if (slashpos == std::string::npos) slashpos = 0;
				std::string basename = name.substr(slashpos + 1, dotpos - (slashpos + 1));
				char* endptr;
				u32 hash = (u32)strtoll(basename.c_str(), &endptr, 16);
				if (endptr - basename.c_str() < (ptrdiff_t)basename.length()) {
					INFO_LOG(COMMON, "Invalid hash %s", basename.c_str());
					continue;
				}
				mapping.emplace(hash, i);
			}
		} while (false);

	texture_map = mapping;
	custom_textures_available = !texture_map.empty();
	return custom_textures_available;
}

u8* GdxsvTexturePackSource::LoadCustomTexture(u32 hash, int& width, int& height) {
	const auto it = texture_map.find(hash);
	if (it == texture_map.end()) return nullptr;

	// Load texture from texture pack file
	auto zfp = zip_fopen_index(texp_zip, it->second, 0);
	if (zfp == nullptr) {
		ERROR_LOG(COMMON, "LoadCustomTexture: zip_fopen_index failure");
	} else {
		stbi_io_callbacks cbk{};
		cbk.read = [](void* user, char* data, int size) -> int {
			return static_cast<int>(zip_fread(static_cast<zip_file_t*>(user), data, size));
		};
		cbk.skip = [](void* user, int n) {
			while (0 < n) {
				char buf[4096];
				const int size = std::min<int>(sizeof(buf), n);
				n -= static_cast<int>(zip_fread(static_cast<zip_file_t*>(user), buf, size));
			}
			verify(n == 0);
		};
		cbk.eof = [](void* user) -> int { return static_cast<zip_file_t*>(user)->eof; };

		int n;
		stbi_set_flip_vertically_on_load(1);
		u8* imgData = stbi_load_from_callbacks(&cbk, zfp, &width, &height, &n, STBI_rgb_alpha);

		zip_fclose(zfp);
		return imgData;
	}

	return nullptr;
}
