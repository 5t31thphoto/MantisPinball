#pragma once
#include <stdint.h>
#define MC_DATUM 4
#define TC_DATUM 1
#define TL_DATUM 0
#define TR_DATUM 2
#define ML_DATUM 3
#define MR_DATUM 5
#define BC_DATUM 7
#define BL_DATUM 6
#define BR_DATUM 8
class TFT_eSprite { public:
 void fillSprite(uint16_t); void drawPixel(int32_t,int32_t,uint16_t); void fillCircle(int32_t,int32_t,int32_t,uint16_t);
 void drawCircle(int32_t,int32_t,int32_t,uint16_t); void drawLine(int32_t,int32_t,int32_t,int32_t,uint16_t);
 void fillRect(int32_t,int32_t,int32_t,int32_t,uint16_t); void drawRect(int32_t,int32_t,int32_t,int32_t,uint16_t);
 void drawRoundRect(int32_t,int32_t,int32_t,int32_t,int32_t,uint16_t); void fillRoundRect(int32_t,int32_t,int32_t,int32_t,int32_t,uint16_t); void fillTriangle(int32_t,int32_t,int32_t,int32_t,int32_t,int32_t,uint16_t);
 void drawFastVLine(int32_t,int32_t,int32_t,uint16_t); void drawFastHLine(int32_t,int32_t,int32_t,uint16_t);
 void setTextDatum(uint8_t); void setTextColor(uint16_t); int16_t drawString(const char*,int32_t,int32_t,uint8_t);
 void pushSprite(int32_t,int32_t); int16_t textWidth(const char*,uint8_t=1); void setRotation(uint8_t); };
