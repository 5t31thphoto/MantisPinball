#pragma once
#include <stdint.h>
#include "Arduino.h"
#define WIFI_STA 1
#define WIFI_OFF 0
#define WL_CONNECTED 3
struct WiFiAPI;
struct WiFiClass { template<class... A> void mode(A...){} template<class... A> void disconnect(A...){} template<class... A> void macAddress(A...){}
  template<class... A> int begin(A...){return 0;} int status(){return 0;} template<class... A> void setSleep(A...){}
  template<class... A> bool softAP(A...){return true;} template<class... A> bool softAPConfig(A...){return true;} template<class... A> bool softAPdisconnect(A...){return true;}
  template<class... A> int scanNetworks(A...){return 0;} template<class... A> void setHostname(A...){} template<class... A> void setAutoReconnect(A...){} template<class... A> void persistent(A...){}
  template<class... A> int hostByName(A...){return 0;} template<class... A> int RSSI(A...){return 0;} template<class... A> int channel(A...){return 0;} int getMode(){return 0;}
  struct IPAddr { String toString() const {return String();} operator uint32_t() const {return 0;} uint8_t operator[](int) const {return 0;} };
  IPAddr softAPIP(){return IPAddr();} IPAddr localIP(){return IPAddr();} IPAddr gatewayIP(){return IPAddr();} };
extern WiFiClass WiFi;
struct EspClass { uint64_t getEfuseMac(); uint32_t getFreeHeap(); uint32_t getMaxAllocHeap(); };
extern EspClass ESP;
#define WIFI_AP 2
#define WIFI_AP_STA 3
struct IPAddress { IPAddress(){} template<class... A> IPAddress(A...){} String toString() const { return String(); } uint8_t operator[](int) const { return 0; } operator uint32_t() const { return 0; } };
struct WiFiAPI { template<class... A> bool softAP(A...){return true;} IPAddress softAPIP(){return IPAddress();} template<class... A> bool softAPConfig(A...){return true;} template<class... A> bool softAPdisconnect(A...){return true;} IPAddress localIP(){return IPAddress();} template<class... A> int scanNetworks(A...){return 0;} template<class... A> String SSID(A...){return String();} template<class... A> int RSSI(A...){return 0;} template<class... A> void setHostname(A...){} template<class... A> void setAutoReconnect(A...){} template<class... A> void persistent(A...){} template<class... A> int hostByName(A...){return 0;} };
