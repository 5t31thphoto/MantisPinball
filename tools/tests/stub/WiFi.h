#pragma once
#include <stdint.h>
#define WIFI_STA 1
#define WIFI_OFF 0
struct WiFiClass { void mode(int); void disconnect(bool=false); void macAddress(uint8_t*); };
extern WiFiClass WiFi;
struct EspClass { uint64_t getEfuseMac(); };
extern EspClass ESP;
