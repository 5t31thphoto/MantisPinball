#pragma once
#include "FS.h"
struct LittleFSClass : fs::FS { template<class... A> bool begin(A...){return true;} size_t totalBytes(){return 0;} size_t usedBytes(){return 0;} void end(){} };
extern LittleFSClass LittleFS;
