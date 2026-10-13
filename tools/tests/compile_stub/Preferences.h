#pragma once
#include <stdint.h>
#include <stddef.h>
#include "Arduino.h"
class Preferences { public:
  template<class... A> bool begin(A...){return true;} void end(){}
  template<class... A> size_t putString(A...){return 0;} template<class... A> String getString(A...){return String();}
  template<class... A> size_t getBytes(A...){return 0;} template<class... A> size_t putBytes(A...){return 0;} template<class... A> size_t getBytesLength(A...){return 0;}
  template<class... A> uint8_t getUChar(A...){return 0;} template<class... A> size_t putUChar(A...){return 0;}
  template<class... A> uint16_t getUShort(A...){return 0;} template<class... A> size_t putUShort(A...){return 0;}
  template<class... A> uint32_t getUInt(A...){return 0;} template<class... A> size_t putUInt(A...){return 0;}
  template<class... A> int32_t getInt(A...){return 0;} template<class... A> size_t putInt(A...){return 0;}
  template<class... A> uint32_t getULong(A...){return 0;} template<class... A> size_t putULong(A...){return 0;}
  template<class... A> float getFloat(A...){return 0;} template<class... A> size_t putFloat(A...){return 0;}
  template<class... A> double getDouble(A...){return 0;} template<class... A> size_t putDouble(A...){return 0;} template<class... A> bool getBool(A...){return false;} template<class... A> size_t putBool(A...){return 0;}
  template<class... A> bool isKey(A...){return false;} bool clear(){return true;} template<class... A> bool remove(A...){return true;} };
