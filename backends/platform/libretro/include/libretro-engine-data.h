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

#ifndef BACKENDS_PLATFORM_LIBRETRO_ENGINE_DATA_H
#define BACKENDS_PLATFORM_LIBRETRO_ENGINE_DATA_H

#ifdef EMSCRIPTEN

#include "common/archive.h"
#include "common/str.h"

class LibretroRemoteEngineData : public Common::Archive {
public:
	explicit LibretroRemoteEngineData(const Common::String &baseUrl);
	~LibretroRemoteEngineData() override;

	bool hasFile(const Common::Path &path) const override;
	int listMembers(Common::ArchiveMemberList &list) const override;
	const Common::ArchiveMemberPtr getMember(const Common::Path &path) const override;
	Common::SeekableReadStream *createReadStreamForMember(const Common::Path &path) const override;

private:
	int indexOf(const Common::Path &path) const;
	void resolveBaseUrl() const;
	bool fetch(int index) const;

	mutable Common::String _baseUrl;
	void **_cache;
	unsigned int *_cacheSize;
};

#endif // EMSCRIPTEN

#endif
