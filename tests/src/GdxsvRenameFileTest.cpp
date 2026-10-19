#include <cstdio>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "nowide/cstdio.hpp"
#include "gdxsv/gdxsv_update.h"

namespace {

void WriteFile(const std::string& path, const std::vector<char>& data) {
	FILE* f = nowide::fopen(path.c_str(), "wb");
	ASSERT_NE(f, nullptr);
	ASSERT_EQ(std::fwrite(data.data(), 1, data.size(), f), data.size());
	std::fclose(f);
}

std::vector<char> ReadFile(const std::string& path) {
	std::vector<char> data;
	FILE* f = nowide::fopen(path.c_str(), "rb");
	if (f == nullptr) return data;
	char buf[4096];
	size_t n;
	while ((n = std::fread(buf, 1, sizeof(buf), f)) != 0) data.insert(data.end(), buf, buf + n);
	std::fclose(f);
	return data;
}

bool Exists(const std::string& path) {
	FILE* f = nowide::fopen(path.c_str(), "rb");
	if (f == nullptr) return false;
	std::fclose(f);
	return true;
}

// Larger than the copy buffer, so the copy takes more than one read.
std::vector<char> Payload() {
	std::vector<char> data(150 * 1024 + 7);
	for (size_t i = 0; i < data.size(); i++) data[i] = static_cast<char>(i * 7 + i / 251);
	return data;
}

}  // namespace

// The cross-filesystem fallback: the destination gets every byte, the source is gone.
TEST(GdxsvRenameFileTest, CopyThenRemoveMovesTheFile) {
	const std::string from = "gdxsv_rename_test_from.bin";
	const std::string to = "gdxsv_rename_test_to.bin";
	std::remove(to.c_str());
	WriteFile(from, Payload());

	ASSERT_TRUE(gdxsv_copy_then_remove(from, to));
	EXPECT_EQ(ReadFile(to), Payload());
	EXPECT_FALSE(Exists(from));
	std::remove(to.c_str());
}

// A destination that cannot be opened keeps the source.
TEST(GdxsvRenameFileTest, CopyThenRemoveKeepsTheSourceOnFailure) {
	const std::string from = "gdxsv_rename_test_keep.bin";
	WriteFile(from, Payload());

	EXPECT_FALSE(gdxsv_copy_then_remove(from, "gdxsv_no_such_dir/to.bin"));
	EXPECT_EQ(ReadFile(from), Payload());
	std::remove(from.c_str());
}

TEST(GdxsvRenameFileTest, RenameMovesTheFile) {
	const std::string from = "gdxsv_rename_test_a.bin";
	const std::string to = "gdxsv_rename_test_b.bin";
	std::remove(to.c_str());
	WriteFile(from, Payload());

	ASSERT_TRUE(gdxsv_rename_file(from, to));
	EXPECT_EQ(ReadFile(to), Payload());
	EXPECT_FALSE(Exists(from));
	std::remove(to.c_str());
}
