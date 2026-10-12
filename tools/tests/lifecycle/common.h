#include <cstdio>
#include <cstring>
#include <cstdlib>
#include "Arduino.h"
#include "TFT_eSPI.h"
#include "WiFi.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "Preferences.h"
static uint32_t g_ms=1000; uint32_t millis(){return g_ms;} uint32_t esp_random(){static uint32_t s=7; s=s*1664525u+1013904223u; return s;} void delay(uint32_t){}
WiFiClass WiFi; void WiFiClass::mode(int){} void WiFiClass::disconnect(bool){} void WiFiClass::macAddress(uint8_t* m){ for(int i=0;i<6;i++) m[i]=i+1; }
EspClass ESP; uint64_t EspClass::getEfuseMac(){return 0x112233445566ull;}
esp_err_t esp_now_init(){return 0;} esp_err_t esp_now_deinit(){return 0;} esp_err_t esp_now_register_recv_cb(esp_now_recv_cb_t){return 0;}
esp_err_t esp_now_unregister_recv_cb(){return 0;} esp_err_t esp_now_add_peer(const esp_now_peer_info_t*){return 0;} bool esp_now_is_peer_exist(const uint8_t*){return false;} esp_err_t esp_now_send(const uint8_t*,const uint8_t*,size_t){return 0;}
int esp_wifi_set_promiscuous(bool){return 0;} int esp_wifi_set_channel(int,int){return 0;}
const char* String::c_str() const { return ""; }
bool Preferences::begin(const char*,bool){return true;} void Preferences::end(){} size_t Preferences::putString(const char*,const char*){return 0;} String Preferences::getString(const char*,const char*){return String();}
size_t Preferences::getBytes(const char*,void*,size_t){return 0;} size_t Preferences::putBytes(const char*,const void*,size_t n){return n;} size_t Preferences::getBytesLength(const char*){return 0;}
uint8_t Preferences::getUChar(const char*,uint8_t d){return d;} size_t Preferences::putUChar(const char*,uint8_t){return 1;} uint16_t Preferences::getUShort(const char*,uint16_t d){return d;} size_t Preferences::putUShort(const char*,uint16_t){return 2;}
uint32_t Preferences::getUInt(const char*,uint32_t d){return d;} size_t Preferences::putUInt(const char*,uint32_t){return 4;} bool Preferences::clear(){return true;} bool Preferences::remove(const char*){return true;}
TFT_eSprite fb; void TFT_eSprite::fillSprite(uint16_t){} void TFT_eSprite::drawPixel(int32_t,int32_t,uint16_t){} void TFT_eSprite::fillCircle(int32_t,int32_t,int32_t,uint16_t){} void TFT_eSprite::drawCircle(int32_t,int32_t,int32_t,uint16_t){} void TFT_eSprite::drawLine(int32_t,int32_t,int32_t,int32_t,uint16_t){} void TFT_eSprite::fillRect(int32_t,int32_t,int32_t,int32_t,uint16_t){} void TFT_eSprite::drawRect(int32_t,int32_t,int32_t,int32_t,uint16_t){} void TFT_eSprite::drawRoundRect(int32_t,int32_t,int32_t,int32_t,int32_t,uint16_t){} void TFT_eSprite::fillRoundRect(int32_t,int32_t,int32_t,int32_t,int32_t,uint16_t){} void TFT_eSprite::fillTriangle(int32_t,int32_t,int32_t,int32_t,int32_t,int32_t,uint16_t){} void TFT_eSprite::drawFastVLine(int32_t,int32_t,int32_t,uint16_t){} void TFT_eSprite::drawFastHLine(int32_t,int32_t,int32_t,uint16_t){} void TFT_eSprite::setTextDatum(uint8_t){} void TFT_eSprite::setTextColor(uint16_t){} int16_t TFT_eSprite::drawString(const char*,int32_t,int32_t,uint8_t){return 0;} void TFT_eSprite::pushSprite(int32_t,int32_t){} void TFT_eSprite::setRotation(uint8_t){} int16_t TFT_eSprite::textWidth(const char* s,uint8_t){return (int16_t)strlen(s)*6;}
bool btnL=false,btnR=false,btnL_prev=false,btnR_prev=false; bool fbReady=true;
void chordGuardTick(){} void chordGuardReset(){} bool chordLEdge(){return btnL&&!btnL_prev;} bool chordREdge(){return btnR&&!btnR_prev;} bool chordLHeld(){return btnL;} bool chordRHeld(){return btnR;}
static void frame(bool l,bool r); 
void pgRegisterFlush(void (*)()){} void qpAddXp(unsigned int){} void qpGrantPetXp(unsigned int){}
