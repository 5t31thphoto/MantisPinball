#pragma once
#include <stddef.h>
#include <stdint.h>
namespace fs { class File { public: operator bool() const; bool available(); int read(); size_t read(uint8_t*,size_t); bool seek(uint32_t); size_t size(); void close(); int readBytesUntil(char,char*,int); size_t position(); }; }
class LittleFSClass { public: bool begin(bool=false, const char* =nullptr, uint8_t=10, const char* =nullptr); fs::File open(const char*, const char* = "r"); bool exists(const char*); };
extern LittleFSClass LittleFS;
