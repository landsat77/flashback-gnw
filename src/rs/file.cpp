
/*
 * REminiscence - Flashback interpreter
 * Copyright (C) 2005-2019 Gregory Montoir (cyx@users.sourceforge.net)
 */

#ifndef GNW
#include <sys/param.h>
#include <sys/stat.h>
#else
#define MAXPATHLEN 256
#endif
#include "file.h"
#include "fs.h"
#include "util.h"
#ifdef USE_ZLIB
#include "zlib.h"
#endif
#ifdef USE_RWOPS
#include <SDL_filesystem.h>
#include <SDL_rwops.h>
#endif

struct File_impl {
	bool _ioErr;
	File_impl() : _ioErr(false) {}
	virtual ~File_impl() {}
	virtual bool open(const char *path, const char *mode) = 0;
	virtual void close() = 0;
	virtual uint32_t size() = 0;
	virtual void seek(int32_t off) = 0;
	virtual uint32_t tell() = 0;
	virtual uint32_t read(void *ptr, uint32_t len) = 0;
	virtual uint32_t write(const void *ptr, uint32_t len) = 0;
#ifdef GNW
	virtual const uint8_t *mapped() { return 0; }
#endif
};

#ifdef GNW
/* GNW: the data files are read in place from the asset pack (memory mapped
 * flash). */
struct PackFile : File_impl {
	const uint8_t *_data;
	uint32_t _size, _pos;
	PackFile() : _data(0), _size(0), _pos(0) {}
	bool open(const char *name, const char *mode) {
		_ioErr = false;
		_pos = 0;
		return fb_plat_find_file(name, &_data, &_size);
	}
	void close() {
		_data = 0;
		_size = _pos = 0;
	}
	uint32_t size() {
		return _size;
	}
	void seek(int32_t off) {
		_pos = (off < 0) ? 0 : off;
	}
	uint32_t tell() {
		return _pos;
	}
	uint32_t read(void *ptr, uint32_t len) {
		uint32_t n = len;
		if (_pos >= _size) {
			n = 0;
		} else if (n > _size - _pos) {
			n = _size - _pos;
		}
		if (n != len) {
			_ioErr = true;
			memset((uint8_t *)ptr + n, 0, len - n);
		}
		memcpy(ptr, _data + _pos, n);
		_pos += n;
		return n;
	}
	uint32_t write(const void *ptr, uint32_t len) {
		_ioErr = true;
		return 0;
	}
	const uint8_t *mapped() {
		return _data;
	}
};

/* GNW: files of the save directory. The whole file is kept in memory and
 * read / written with a single call (the firmware's file API is slow for many
 * small reads and writes). No compression. */
struct BufFile : File_impl {
	FILE *_fp;
	bool _write;
	uint8_t *_buf;
	uint32_t _size, _cap, _pos;
	BufFile() : _fp(0), _write(false), _buf(0), _size(0), _cap(0), _pos(0) {}
	~BufFile() {
		close();
	}
	bool open(const char *path, const char *mode) {
		close();
		_ioErr = false;
		_write = (mode[0] == 'w');
		_fp = fopen(path, _write ? "wb" : "rb");
		if (!_fp) {
			return false;
		}
		if (!_write) {
			fseek(_fp, 0, SEEK_END);
			long sz = ftell(_fp);
			fseek(_fp, 0, SEEK_SET);
			if (sz < 0 || sz > 1024 * 1024) {
				fclose(_fp);
				_fp = 0;
				return false;
			}
			_buf = (uint8_t *)malloc(sz ? sz : 1);
			if (!_buf || fread(_buf, 1, sz, _fp) != (size_t)sz) {
				_ioErr = true;
			}
			_size = _buf ? sz : 0;
			fclose(_fp);
			_fp = 0;
		}
		_pos = 0;
		return true;
	}
	void close() {
		if (_fp) {
			if (_write && _size && fwrite(_buf, 1, _size, _fp) != _size) {
				_ioErr = true;
			}
			fclose(_fp);
			_fp = 0;
		}
		free(_buf);
		_buf = 0;
		_size = _cap = _pos = 0;
	}
	uint32_t size() {
		return _size;
	}
	void seek(int32_t off) {
		_pos = (off < 0) ? 0 : off;
	}
	uint32_t tell() {
		return _pos;
	}
	uint32_t read(void *ptr, uint32_t len) {
		if (_pos + len > _size) {
			memset(ptr, 0, len);
			_ioErr = true;
			return 0;
		}
		memcpy(ptr, _buf + _pos, len);
		_pos += len;
		return len;
	}
	uint32_t write(const void *ptr, uint32_t len) {
		if (!_write) {
			_ioErr = true;
			return 0;
		}
		if (_pos + len > _cap) {
			uint32_t cap = _cap ? _cap : 16 * 1024;
			while (cap < _pos + len) {
				cap *= 2;
			}
			uint8_t *n = (uint8_t *)realloc(_buf, cap);
			if (!n) {
				_ioErr = true;
				return 0;
			}
			_buf = n;
			_cap = cap;
		}
		memcpy(_buf + _pos, ptr, len);
		_pos += len;
		if (_pos > _size) {
			_size = _pos;
		}
		return len;
	}
};
#endif

struct StdioFile : File_impl {
	FILE *_fp;
	StdioFile() : _fp(0) {}
	bool open(const char *path, const char *mode) {
		_ioErr = false;
		_fp = fopen(path, mode);
		return (_fp != 0);
	}
	void close() {
		if (_fp) {
			fclose(_fp);
			_fp = 0;
		}
	}
	uint32_t size() {
		uint32_t sz = 0;
		if (_fp) {
#ifndef GNW
			const int f = fileno(_fp);
			struct stat st;
			if (fstat(f, &st) == 0) {
				return st.st_size;
			}
			warning("Failed to stat() fileno %d", f);
#endif
			const int pos = ftell(_fp);
			fseek(_fp, 0, SEEK_END);
			sz = ftell(_fp);
			fseek(_fp, pos, SEEK_SET);
		}
		return sz;
	}
	void seek(int32_t off) {
		if (_fp) {
			fseek(_fp, off, SEEK_SET);
		}
	}
	uint32_t tell() {
		if (_fp) {
			return ftell(_fp);
		}
		return 0;
	}
	uint32_t read(void *ptr, uint32_t len) {
		if (_fp) {
			uint32_t r = fread(ptr, 1, len, _fp);
			if (r != len) {
				_ioErr = true;
			}
			return r;
		}
		return 0;
	}
	uint32_t write(const void *ptr, uint32_t len) {
		if (_fp) {
			uint32_t r = fwrite(ptr, 1, len, _fp);
			if (r != len) {
				_ioErr = true;
			}
			return r;
		}
		return 0;
	}
};

#ifdef USE_ZLIB
struct GzipFile : File_impl {
	gzFile _fp;
	GzipFile() : _fp(0) {}
	bool open(const char *path, const char *mode) {
		_ioErr = false;
		_fp = gzopen(path, mode);
		return (_fp != 0);
	}
	void close() {
		if (_fp) {
			gzclose(_fp);
			_fp = 0;
		}
	}
	uint32_t size() {
		uint32_t sz = 0;
		if (_fp) {
			int pos = gztell(_fp);
			gzseek(_fp, 0, SEEK_END);
			sz = gztell(_fp);
			gzseek(_fp, pos, SEEK_SET);
		}
		return sz;
	}
	void seek(int32_t off) {
		if (_fp) {
			gzseek(_fp, off, SEEK_SET);
		}
	}
	uint32_t tell() {
		if (_fp) {
			return gztell(_fp);
		}
		return 0;
	}
	uint32_t read(void *ptr, uint32_t len) {
		if (_fp) {
			uint32_t r = gzread(_fp, ptr, len);
			if (r != len) {
				_ioErr = true;
			}
			return r;
		}
		return 0;
	}
	uint32_t write(const void *ptr, uint32_t len) {
		if (_fp) {
			uint32_t r = gzwrite(_fp, ptr, len);
			if (r != len) {
				_ioErr = true;
			}
			return r;
		}
		return 0;
	}
};
#endif

#ifdef USE_RWOPS
struct AssetFile: File_impl {
	SDL_RWops *_rw;
	AssetFile() : _rw(0) {}
	bool prefixedOpen(const char *prefix, const char *name) {
		char path[MAXPATHLEN];
		snprintf(path, sizeof(path), "%s%s", prefix, name);
		_rw = SDL_RWFromFile(path, "rb");
		if (!_rw) {
			// try uppercase
			char fixedPath[MAXPATHLEN];
			{
				int i = 0;
				for (; path[i] && i < MAXPATHLEN - 1; ++i) {
					fixedPath[i] = path[i];
					if (i < strlen(prefix)) {
						continue;
					}
					if (fixedPath[i] >= 'a' && fixedPath[i] <= 'z') {
						fixedPath[i] += 'A' - 'a';
					}
				}
				fixedPath[i] = 0;
			}
			_rw = SDL_RWFromFile(fixedPath, "rb");
		}
		return _rw != 0;
	}
	bool open(const char *path, const char *mode) {
		_ioErr = false;
		return prefixedOpen("", path) || prefixedOpen("/sdcard/flashback/", path);
	}
	void close() {
		if (_rw) {
			SDL_RWclose(_rw);
			_rw = 0;
		}
	}
	uint32_t size() {
		if (_rw) {
			return SDL_RWsize(_rw);
		}
		return 0;
	}
	void seek(int32_t off) {
		if (_rw) {
			SDL_RWseek(_rw, off, RW_SEEK_SET);
		}
	}
	uint32_t read(void *ptr, uint32_t len) {
		if (_rw) {
			const int count = SDL_RWread(_rw, ptr, 1, len);
			if (count != len) {
				_ioErr = true;
			}
		}
		return 0;
	}
	uint32_t write(const void *ptr, uint32_t len) {
		_ioErr = true;
		return 0;
	}
};
#endif

struct MemoryBufferFile: File_impl {
	uint8_t *_ptr;
	uint32_t _capacity, _offset, _len;
	MemoryBufferFile(int initialCapacity) {
		_capacity = initialCapacity;
		_ptr = (uint8_t *)malloc(_capacity);
		_offset = _len = 0;
	}
	~MemoryBufferFile() {
		free(_ptr);
	}
	bool open(const char *path, const char *mode) {
		return false;
	}
	void close() {
	}
	uint32_t size() {
		return _len;
	}
	uint32_t tell() {
		return _offset;
	}
	void seek(int32_t offs) { // GNW: was int (int32_t is long on ARM)
		_offset = offs;
	}
	uint32_t read(void *ptr, uint32_t len) {
		int count = len;
		if (_offset + count > _len) {
			count = _len - _offset;
			_ioErr = true;
		}
		if (count != 0) {
			memcpy(ptr, _ptr + _offset, count);
			_offset += count;
		}
		return count;
	}
        uint32_t write(const void *ptr, uint32_t len) {
		int count = len;
		while (_offset + count > _capacity) {
			_capacity *= 2;
			_ptr = (uint8_t *)realloc(_ptr, _capacity);
		}
		if (count != 0) {
			memcpy(_ptr + _offset, ptr, count);
			_offset += count;
		}
		_len = _offset;
		return count;
	}
};

File::File()
	: _impl(0) {
}

File::~File() {
	if (_impl) {
		_impl->close();
		delete _impl;
	}
}

bool File::open(const char *filename, const char *mode, FileSystem *fs) {
	if (_impl) {
		_impl->close();
		delete _impl;
		_impl = 0;
	}
	assert(mode[0] != 'z');
#ifdef GNW
	if (mode[0] != 'r') {
		return false;
	}
	_impl = new PackFile;
	char *path = fs->findPath(filename);
	if (path) {
		bool ret = _impl->open(path, mode);
		free(path);
		return ret;
	}
	return false;
#else
	_impl = new StdioFile;
	char *path = fs->findPath(filename);
	if (path) {
		debug(DBG_FILE, "Open file name '%s' mode '%s' path '%s'", filename, mode, path);
		bool ret = _impl->open(path, mode);
		free(path);
		return ret;
	}
#ifdef USE_RWOPS
	if (mode[0] == 'r') {
		_impl = new AssetFile;
		return _impl->open(filename, mode);
	} else if (mode[0] == 'w') {
		bool ret = false;
		char *prefPath = SDL_GetPrefPath("org.cyxdown", "fb");
		if (prefPath) {
			char path[MAXPATHLEN];
			snprintf(path, sizeof(path), "%s/%s", prefPath, filename);
			_impl = new StdioFile;
			ret = _impl->open(path, mode);
			SDL_free(prefPath);
		}
		return ret;
	}
#endif
	return false;
#endif // GNW
}

bool File::open(const char *filename, const char *mode, const char *directory) {
	if (_impl) {
		_impl->close();
		delete _impl;
		_impl = 0;
	}
#ifdef USE_ZLIB
	if (mode[0] == 'z') {
		_impl = new GzipFile;
		++mode;
	}
#endif
#ifdef GNW
	if (mode[0] == 'z') {
		++mode;
	}
	_impl = new BufFile;
#endif
	if (!_impl) {
		_impl = new StdioFile;
	}
	char path[MAXPATHLEN];
#ifdef GNW
	if (!directory || !directory[0]) { // full path
		snprintf(path, sizeof(path), "%s", filename);
	} else
#endif
	snprintf(path, sizeof(path), "%s/%s", directory, filename);
	debug(DBG_FILE, "Open file name '%s' mode '%s' path '%s'", filename, mode, path);
	return _impl->open(path, mode);
}

void File::openMemoryBuffer(int initialCapacity) {
	if (_impl) {
		_impl->close();
		delete _impl;
		_impl = 0;
	}
	_impl = new MemoryBufferFile(initialCapacity);
}

void File::close() {
	if (_impl) {
		_impl->close();
	}
}

bool File::ioErr() const {
	return _impl->_ioErr;
}

uint32_t File::size() {
	return _impl->size();
}

void File::seek(int32_t off) {
	_impl->seek(off);
}

uint32_t File::tell() {
	return _impl->tell();
}

uint32_t File::read(void *ptr, uint32_t len) {
	return _impl->read(ptr, len);
}

#ifdef GNW
const uint8_t *File::mapped() {
	return _impl ? _impl->mapped() : 0;
}
#endif

uint8_t File::readByte() {
	uint8_t b;
	read(&b, 1);
	return b;
}

uint16_t File::readUint16LE() {
	uint8_t lo = readByte();
	uint8_t hi = readByte();
	return (hi << 8) | lo;
}

uint32_t File::readUint32LE() {
	uint16_t lo = readUint16LE();
	uint16_t hi = readUint16LE();
	return (hi << 16) | lo;
}

uint16_t File::readUint16BE() {
	uint8_t hi = readByte();
	uint8_t lo = readByte();
	return (hi << 8) | lo;
}

uint32_t File::readUint32BE() {
	uint16_t hi = readUint16BE();
	uint16_t lo = readUint16BE();
	return (hi << 16) | lo;
}

uint32_t File::write(const void *ptr, uint32_t len) {
	return _impl->write(ptr, len);
}

void File::writeByte(uint8_t b) {
	write(&b, 1);
}

void File::writeUint16LE(uint16_t n) {
	writeByte(n & 0xFF);
	writeByte(n >> 8);
}

void File::writeUint32LE(uint32_t n) {
	writeUint16LE(n & 0xFFFF);
	writeUint16LE(n >> 16);
}

void File::writeUint16BE(uint16_t n) {
	writeByte(n >> 8);
	writeByte(n & 0xFF);
}

void File::writeUint32BE(uint32_t n) {
	writeUint16BE(n >> 16);
	writeUint16BE(n & 0xFFFF);
}

void dumpFile(const char *filename, const uint8_t *p, int size) {
	char path[MAXPATHLEN];
	snprintf(path, sizeof(path), "DUMP/%s", filename);
	FILE *fp = fopen(filename, "wb");
	if (!fp) {
		warning("Failed to open '%s' for writing", path);
	} else {
		const int count = fwrite(p, 1, size, fp);
		if (count != size) {
			warning("Failed to write %d bytes (expected %d)", count, size);
		}
		fclose(fp);
	}
}
