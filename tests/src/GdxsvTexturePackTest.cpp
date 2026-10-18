#include "gdxsv/gdxsv_custom_texture_source.h"

#include <zip.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "gtest/gtest.h"
#include "stdclass.h"
#include "types.h"

namespace fs = std::filesystem;

class GdxsvTexturePackTest : public ::testing::Test {
   protected:
	void SetUp() override {
		dir_ = fs::temp_directory_path() / "gdxsv_texture_pack_test";
		fs::remove_all(dir_);
		fs::create_directories(dir_);
		set_user_data_dir(dir_.generic_string() + "/");
		settings.content.gameId = "GDXTEST";
		config::GdxUseTexturePack.override(true);
	}
	void TearDown() override {
		config::GdxUseTexturePack.reset();
		settings.content.gameId.clear();
		std::error_code ec;
		fs::remove_all(dir_, ec);
	}

	fs::path PackPath() const { return dir_ / "GDXTEST.texp"; }

	void WritePack(const std::string& content) {
		int err = 0;
		zip_t* zip = zip_open(PackPath().string().c_str(), ZIP_CREATE | ZIP_TRUNCATE, &err);
		ASSERT_NE(nullptr, zip);
		zip_source_t* src = zip_source_buffer(zip, content.data(), content.size(), 0);
		ASSERT_LE(0, zip_file_add(zip, "textures/0000abcd.png", src, ZIP_FL_ENC_UTF_8));
		ASSERT_EQ(0, zip_close(zip));
	}

	fs::path dir_;
};

TEST_F(GdxsvTexturePackTest, ReloadAndTerminateReleaseThePackFile) {
	WritePack("not a png");
	GdxsvTexturePackSource source;

	ASSERT_TRUE(source.loadMap());
	EXPECT_EQ(1u, source.getTextureCount());
	EXPECT_TRUE(source.isTextureReplaced(0x0000abcd));
	int w = 0, h = 0;
	EXPECT_EQ(nullptr, source.loadCustomTexture(0x0000abcd, w, h));

	ASSERT_TRUE(source.loadMap());
	source.terminate();
	EXPECT_EQ(0u, source.getTextureCount());
	// Windows refuses to delete a file that is still open.
	EXPECT_TRUE(fs::remove(PackPath()));
}

TEST_F(GdxsvTexturePackTest, AFileThatIsNotAZipLoadsNoTextures) {
	{
		std::ofstream out(PackPath(), std::ios::binary);
		out << "not a zip archive";
	}
	GdxsvTexturePackSource source;

	EXPECT_FALSE(source.loadMap());
	EXPECT_TRUE(fs::remove(PackPath()));
}
