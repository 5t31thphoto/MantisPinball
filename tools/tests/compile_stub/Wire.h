#pragma once
#include <stdint.h>
#include <stddef.h>
struct TwoWire { template<class... A> bool begin(A...){return true;} template<class... A> void beginTransmission(A...){} template<class... A> uint8_t endTransmission(A...){return 0;}
  template<class... A> size_t write(A...){return 0;} template<class... A> uint8_t requestFrom(A...){return 0;} int read(){return 0;} int available(){return 0;} template<class... A> void setClock(A...){} };
extern TwoWire Wire;
