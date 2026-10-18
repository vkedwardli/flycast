#include "gdxsv/gdxsv_update.h"

#include <zip.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "gtest/gtest.h"

namespace fs = std::filesystem;

class GdxsvUpdateTest : public ::testing::Test {
   protected:
	void SetUp() override {
		dir_ = fs::temp_directory_path() / ("gdxsv_update_test_" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()));
		fs::remove_all(dir_);
		fs::create_directories(dir_ / "out");
	}
	void TearDown() override {
		std::error_code ec;
		fs::remove_all(dir_, ec);
	}

	static bool Extract(const fs::path& zip_path, const fs::path& dst) {
		return GdxsvUpdate::ExtractZipFile(zip_path.generic_string(), dst.generic_string());
	}

	fs::path WriteZip(const std::string& file_name, const std::string& content) {
		const auto path = dir_ / "release.zip";
		int err = 0;
		zip_t* zip = zip_open(path.string().c_str(), ZIP_CREATE | ZIP_TRUNCATE, &err);
		EXPECT_NE(nullptr, zip);
		EXPECT_LE(0, zip_dir_add(zip, "sub", ZIP_FL_ENC_UTF_8));
		zip_source_t* src = zip_source_buffer(zip, content.data(), content.size(), 0);
		EXPECT_LE(0, zip_file_add(zip, ("sub/" + file_name).c_str(), src, ZIP_FL_ENC_UTF_8));
		EXPECT_EQ(0, zip_close(zip));
		return path;
	}

	static std::string ReadFile(const fs::path& path) {
		std::ifstream in(path, std::ios::binary);
		std::stringstream ss;
		ss << in.rdbuf();
		return ss.str();
	}

	fs::path dir_;
};

TEST_F(GdxsvUpdateTest, ExtractsDirectoriesAndFilesAndReleasesTheArchive) {
	const std::string content = "flycast binary";
	const auto zip_path = WriteZip("flycast.exe", content);

	ASSERT_TRUE(Extract(zip_path, dir_ / "out"));
	EXPECT_EQ(content, ReadFile(dir_ / "out" / "sub" / "flycast.exe"));
	// The update deletes the downloaded zip afterwards.
	EXPECT_TRUE(fs::remove(zip_path));
}

TEST_F(GdxsvUpdateTest, RejectsAFileThatIsNotAZip) {
	const auto path = dir_ / "broken.zip";
	{
		std::ofstream out(path, std::ios::binary);
		out << "not a zip archive";
	}

	EXPECT_FALSE(Extract(path, dir_ / "out"));
	EXPECT_TRUE(fs::remove(path));
}
