#pragma once
#include <stdint.h>
#include <stddef.h>
struct String { const char* c_str() const; };
class Preferences { public: bool begin(const char*, bool); void end(); size_t putString(const char*, const char*); String getString(const char*, const char*); size_t getBytes(const char*, void*, size_t); size_t putBytes(const char*, const void*, size_t); size_t getBytesLength(const char*); uint8_t getUChar(const char*, uint8_t=0); size_t putUChar(const char*, uint8_t); uint16_t getUShort(const char*, uint16_t=0); size_t putUShort(const char*, uint16_t); uint32_t getUInt(const char*, uint32_t=0); size_t putUInt(const char*, uint32_t); bool clear(); bool remove(const char*); };
