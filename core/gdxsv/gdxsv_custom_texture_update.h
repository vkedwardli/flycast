#pragma once
#include <future>
#include <string>
#include <vector>

class GdxsvCustomTexutreUpdate {
   public:
	void Reset();
	bool IsUpdateAvailable();
	void FetchLatestVersionInfo();
	std::string GetLatestVersion() const;
	std::shared_future<bool> StartUpdate();
	float UpdateProgress() const;

   private:
	struct LatestVersionInfo {
		bool is_new_version = false;
		std::string name;
		std::string version;
		std::string xxhash;
		int size;
		std::vector<std::tuple<std::string, int>> chunks;
	};

	static void HandleReleaseJSON(const std::string& json_string, LatestVersionInfo& out);
	static std::string GetCurrentXXHash();
	static std::string GetTempDir();

	std::shared_future<LatestVersionInfo> fetch_latest_version_future_;
	std::vector<uint8_t> download_buf_;
};

extern GdxsvCustomTexutreUpdate gdxsv_custom_texture_update;
