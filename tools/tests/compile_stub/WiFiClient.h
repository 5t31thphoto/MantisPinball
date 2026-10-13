#pragma once
#include <stdint.h>
#include <stddef.h>
#include "Preferences.h"
struct WiFiClient { template<class... A> int connect(A...){return 0;} template<class... A> size_t print(A...){return 0;} template<class... A> size_t println(A...){return 0;}
  template<class... A> size_t printf(A...){return 0;} template<class... A> size_t write(A...){return 0;} int available(){return 0;} int read(){return -1;} template<class... A> int read(A...){return 0;}
  bool connected(){return false;} void stop(){} template<class... A> void setTimeout(A...){} template<class... A> String readStringUntil(A...){return String();} int peek(){return -1;}
  template<class... A> size_t readBytes(A...){return 0;} template<class... A> size_t readBytesUntil(A...){return 0;} template<class... A> void setNoDelay(A...){} void flush(){} operator bool(){return false;} };
