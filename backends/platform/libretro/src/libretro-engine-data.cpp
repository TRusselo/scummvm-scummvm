/* ScummVM - Graphic Adventure Engine
 *
 * ScummVM is the legal property of its developers, whose names
 * are too numerous to list here. Please refer to the COPYRIGHT
 * file distributed with this source distribution.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#define FORBIDDEN_SYMBOL_ALLOW_ALL

#include "backends/platform/libretro/include/libretro-engine-data.h"

#ifdef EMSCRIPTEN

#include <emscripten.h>
#include <stdlib.h>

#include "backends/platform/libretro/include/libretro-core.h"
#include "backends/platform/libretro/include/libretro-engine-data-manifest.h"
#include "common/memstream.h"

LibretroRemoteEngineData::LibretroRemoteEngineData(const Common::String &baseUrl) :
	_baseUrl(baseUrl) {
	if (!_baseUrl.empty() && !_baseUrl.hasSuffix("/"))
		_baseUrl += "/";

	_cache = (void **)calloc(s_engineDataManifestCount, sizeof(void *));
	_cacheSize = (unsigned int *)calloc(s_engineDataManifestCount, sizeof(unsigned int));
}

LibretroRemoteEngineData::~LibretroRemoteEngineData() {
	if (_cache) {
		for (unsigned int i = 0; i < s_engineDataManifestCount; i++)
			free(_cache[i]);
		free(_cache);
	}
	free(_cacheSize);
}

int LibretroRemoteEngineData::indexOf(const Common::Path &path) const {
	Common::String name = path.baseName();
	if (name.empty())
		return -1;

	for (unsigned int i = 0; i < s_engineDataManifestCount; i++) {
		if (name.equalsIgnoreCase(s_engineDataManifest[i].name))
			return (int)i;
	}
	return -1;
}

bool LibretroRemoteEngineData::hasFile(const Common::Path &path) const {
	return indexOf(path) >= 0;
}

int LibretroRemoteEngineData::listMembers(Common::ArchiveMemberList &list) const {
	for (unsigned int i = 0; i < s_engineDataManifestCount; i++)
		list.push_back(Common::ArchiveMemberPtr(new Common::GenericArchiveMember(Common::Path(s_engineDataManifest[i].name), *this)));

	return (int)s_engineDataManifestCount;
}

const Common::ArchiveMemberPtr LibretroRemoteEngineData::getMember(const Common::Path &path) const {
	int index = indexOf(path);
	if (index < 0)
		return Common::ArchiveMemberPtr();

	return Common::ArchiveMemberPtr(new Common::GenericArchiveMember(Common::Path(s_engineDataManifest[index].name), *this));
}

void LibretroRemoteEngineData::resolveBaseUrl() const {
	if (!_baseUrl.empty())
		return;

	char buffer[1024] = {0};
	MAIN_THREAD_EM_ASM({
		var base = (typeof EJS_pathtodata === "string" && EJS_pathtodata) ? EJS_pathtodata : "";
		if (base && !base.endsWith("/")) base += "/";
		var url = new URL(base + "cores/scummvm-engine-data/", document.baseURI).href;
		stringToUTF8(url, $0, $1);
	}, buffer, (int)sizeof(buffer));

	_baseUrl = buffer;
	if (!_baseUrl.empty() && !_baseUrl.hasSuffix("/"))
		_baseUrl += "/";

	retro_log_cb(RETRO_LOG_INFO, "[engine-data] base URL resolved to %s\n", _baseUrl.c_str());
}

bool LibretroRemoteEngineData::fetch(int index) const {
	if (_cache[index])
		return true;

	if (!_cache || !_cacheSize)
		return false;

	resolveBaseUrl();

	Common::String url = _baseUrl + s_engineDataManifest[index].name;

	void *buffer = NULL;
	int numBytes = 0;
	int error = 0;

	retro_log_cb(RETRO_LOG_INFO, "[engine-data] fetching %s (%u bytes expected)\n",
	             s_engineDataManifest[index].name, s_engineDataManifest[index].size);

	emscripten_wget_data(url.c_str(), &buffer, &numBytes, &error);

	if (error || !buffer || numBytes <= 0) {
		retro_log_cb(RETRO_LOG_ERROR, "[engine-data] fetch failed for %s (error %d)\n",
		             s_engineDataManifest[index].name, error);
		free(buffer);
		return false;
	}

	if ((unsigned int)numBytes != s_engineDataManifest[index].size) {
		retro_log_cb(RETRO_LOG_WARN, "[engine-data] %s is %d bytes, manifest says %u\n",
		             s_engineDataManifest[index].name, numBytes, s_engineDataManifest[index].size);
	}

	_cache[index] = buffer;
	_cacheSize[index] = (unsigned int)numBytes;

	retro_log_cb(RETRO_LOG_INFO, "[engine-data] fetched %s (%d bytes)\n",
	             s_engineDataManifest[index].name, numBytes);
	return true;
}

Common::SeekableReadStream *LibretroRemoteEngineData::createReadStreamForMember(const Common::Path &path) const {
	int index = indexOf(path);
	if (index < 0)
		return NULL;

	if (!fetch(index))
		return NULL;

	return new Common::MemoryReadStream((const byte *)_cache[index], _cacheSize[index], DisposeAfterUse::NO);
}

#endif // EMSCRIPTEN
