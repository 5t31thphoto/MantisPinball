#pragma once
#include "../stub/Arduino.h"
// Arduino's String (WString.h comes in through Arduino.h on the real core)
struct String {
  String(){} String(const char*){} String(char){} template<class T> String(T, int=10){}
  const char* c_str() const { return ""; } unsigned length() const { return 0; }
  template<class T> String& operator+=(const T&){ return *this; } template<class T> String operator+(const T&) const { return *this; }
  template<class T> bool operator==(const T&) const { return false; } template<class T> bool operator!=(const T&) const { return true; }
  char operator[](unsigned) const { return 0; } char& operator[](unsigned){ static char c; return c; }
  template<class... A> int indexOf(A...) const { return -1; } template<class... A> int lastIndexOf(A...) const { return -1; }
  template<class... A> String substring(A...) const { return *this; } long toInt() const { return 0; } float toFloat() const { return 0; }
  void trim(){} void toUpperCase(){} void toLowerCase(){} template<class... A> bool startsWith(A...) const { return false; } template<class... A> bool endsWith(A...) const { return false; }
  template<class... A> void replace(A...){} template<class... A> void remove(A...){} void reserve(unsigned){} bool isEmpty() const { return true; }
  template<class... A> bool equals(A...) const { return false; } template<class... A> bool equalsIgnoreCase(A...) const { return false; } char charAt(unsigned) const { return 0; }
  template<class... A> void toCharArray(A...) const {} template<class... A> bool concat(A...){ return true; } operator bool() const { return true; } };
inline String operator+(const char*, const String& s){ return s; }
void delayMicroseconds(uint32_t);
// compile-check extras: real Arduino-ESP32 APIs main.cpp uses
long random(long); long random(long, long); void randomSeed(unsigned long);
void pinMode(uint8_t, uint8_t); void digitalWrite(uint8_t, uint8_t); int digitalRead(uint8_t);
#define INPUT_PULLUP 0x05
void ledcDetachPin(uint8_t); double ledcSetup(uint8_t, double, uint8_t); void ledcAttachPin(uint8_t, uint8_t); void ledcWrite(uint8_t, uint32_t);
typedef enum { GPIO_NUM_0=0,GPIO_NUM_1,GPIO_NUM_2,GPIO_NUM_3,GPIO_NUM_4,GPIO_NUM_5,GPIO_NUM_6,GPIO_NUM_7,GPIO_NUM_8,GPIO_NUM_9,GPIO_NUM_10,GPIO_NUM_11,GPIO_NUM_12,GPIO_NUM_13,GPIO_NUM_14,GPIO_NUM_15,GPIO_NUM_16,GPIO_NUM_17,GPIO_NUM_18,GPIO_NUM_19,GPIO_NUM_20,GPIO_NUM_21,GPIO_NUM_38=38,GPIO_NUM_39,GPIO_NUM_40,GPIO_NUM_41,GPIO_NUM_42,GPIO_NUM_43,GPIO_NUM_44,GPIO_NUM_45,GPIO_NUM_46,GPIO_NUM_47,GPIO_NUM_48 } gpio_num_t;
unsigned long micros();
struct HWSerial { template<class... A> void begin(A...){} template<class... A> size_t printf(A...){return 0;} template<class... A> size_t println(A...){return 0;} template<class A> size_t print(A){return 0;} };
extern HWSerial Serial;
int analogRead(uint8_t); void analogReadResolution(uint8_t); template<class... A> void analogSetPinAttenuation(A...){}
#define ADC_11db 3
#define TFT_BL 38
#define PROGMEM
#define IRAM_ATTR
#define DRAM_ATTR
#define RAM_ATTR
#include <algorithm>
using std::min; using std::max;
template<class... A> void configTime(A...){}
template<class... A> bool getLocalTime(A...){ return false; }
uint8_t pgm_read_byte(const void*); uint16_t pgm_read_word(const void*);
void yield();
#define F(s) (s)
#define PSTR(s) (s)
#define FPSTR(s) (s)
