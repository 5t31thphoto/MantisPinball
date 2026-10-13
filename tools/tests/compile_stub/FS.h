#pragma once
#include <stddef.h>
#include <stdint.h>
#include "Arduino.h"
namespace fs { struct File { explicit operator bool() const { return false; }
  template<class... A> int read(A...){return -1;} template<class... A> size_t write(A...){return 0;} template<class... A> bool seek(A...){return false;}
  size_t size(){return 0;} size_t position(){return 0;} void close(){} bool available(){return false;} template<class... A> int readBytesUntil(A...){return 0;}
  template<class... A> int readBytes(A...){return 0;} const char* name(){return "";} bool isDirectory(){return false;} File openNextFile(){return File();} template<class... A> size_t print(A...){return 0;} template<class... A> size_t println(A...){return 0;} int peek(){return -1;} template<class... A> String readStringUntil(A...){return String();} String readString(){return String();} };
  struct FS { template<class... A> File open(A...){return File();} template<class... A> bool exists(A...){return false;} template<class... A> bool remove(A...){return false;} template<class... A> bool mkdir(A...){return false;} template<class... A> bool rename(A...){return false;} }; }
using fs::File;
