/*
	 Copyright 2018 flyinghead
 
	 This file is part of reicast.
 
	 reicast is free software: you can redistribute it and/or modify
	 it under the terms of the GNU General Public License as published by
	 the Free Software Foundation, either version 2 of the License, or
	 (at your option) any later version.
 
	 reicast is distributed in the hope that it will be useful,
	 but WITHOUT ANY WARRANTY; without even the implied warranty of
	 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	 GNU General Public License for more details.
 
	 You should have received a copy of the GNU General Public License
	 along with reicast.  If not, see <https://www.gnu.org/licenses/>.
 */
#pragma once

#include "texconv.h"
#include "stdclass.h"

#include <string>
#include <vector>
#include <map>
#include <mutex>

class ICustomTextureSource {
public:
	virtual ~ICustomTextureSource() = default;
	virtual bool Init() = 0;
	virtual bool LoadMap() = 0;
	virtual u8* LoadCustomTexture(u32 hash, int& width, int& height) = 0;
	virtual void Terminate() = 0;
};

class CustomTextureFolderSource : public ICustomTextureSource {
public:
	bool Init() override;
	bool LoadMap() override;
	u8* LoadCustomTexture(u32 hash, int& width, int& height) override;
	void Terminate() override;

private:
	bool initialized = false;
	std::string textures_path;
	std::map<u32, std::string> texture_map;
};

extern CustomTextureFolderSource custom_texture_folder_source;
class BaseTextureCacheData;

class CustomTexture {
public:
	CustomTexture() : loader_thread(loader_thread_func, this, "CustomTexLoader") {
		sources.push_back(&custom_texture_folder_source);
	}
	~CustomTexture() { Terminate(); }
	u8* LoadCustomTexture(u32 hash, int& width, int& height);
	void LoadCustomTextureAsync(BaseTextureCacheData *texture_data);
	void DumpTexture(u32 hash, int w, int h, TextureType textype, void *src_buffer);
	void Terminate();
	void AddTextureSource(ICustomTextureSource* source) { sources.push_back(source); }
	std::string GetGameId();

private:
	bool Init();
	void LoaderThread();
	void LoadMap();
	
	static void *loader_thread_func(void *param) { ((CustomTexture *)param)->LoaderThread(); return NULL; }
	
	bool initialized = false;
	bool custom_textures_available = false;
	cThread loader_thread;
	cResetEvent wakeup_thread;
	std::vector<BaseTextureCacheData *> work_queue;
	std::mutex work_queue_mutex;
	std::vector<ICustomTextureSource*> sources;
};

extern CustomTexture custom_texture;
