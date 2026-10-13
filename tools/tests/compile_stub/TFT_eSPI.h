#pragma once
#include <stdint.h>
#include <string.h>
#define TL_DATUM 0
#define TC_DATUM 1
#define TR_DATUM 2
#define ML_DATUM 3
#define MC_DATUM 4
#define MR_DATUM 5
#define BL_DATUM 6
#define BC_DATUM 7
#define BR_DATUM 8
#define TFT_BLACK 0x0000
#define TFT_WHITE 0xFFFF
// compile-check stand-ins: accept any arguments, like the real overload sets do
struct TFT_eSPI_Base {
    template<class... A> void fillSprite(A...){}       template<class... A> void drawPixel(A...){}
    template<class... A> void fillCircle(A...){}       template<class... A> void drawCircle(A...){}
    template<class... A> void drawLine(A...){}         template<class... A> void fillRect(A...){}
    template<class... A> void drawRect(A...){}         template<class... A> void drawRoundRect(A...){}
    template<class... A> void fillRoundRect(A...){}    template<class... A> void fillTriangle(A...){}
    template<class... A> void drawTriangle(A...){}     template<class... A> void drawFastVLine(A...){}
    template<class... A> void drawFastHLine(A...){}    template<class... A> void setTextDatum(A...){}
    template<class... A> void setTextColor(A...){}     template<class... A> int16_t drawString(A...){return 0;}
    template<class... A> int16_t drawNumber(A...){return 0;} template<class... A> int16_t drawFloat(A...){return 0;}
    template<class... A> void pushSprite(A...){}       template<class... A> void setRotation(A...){} uint8_t getRotation(){return 0;} int16_t width(){return 170;} int16_t height(){return 320;}
    template<class... A> int16_t textWidth(A...){return 0;} template<class... A> int16_t fontHeight(A...){return 0;}
    template<class... A> void* createSprite(A...){return nullptr;} template<class... A> void setColorDepth(A...){}
    template<class... A> void deleteSprite(A...){}     template<class... A> void init(A...){}
    template<class... A> void fillScreen(A...){}       template<class... A> void setSwapBytes(A...){}
    template<class... A> void pushImage(A...){}        template<class... A> void setTextSize(A...){}
    template<class... A> void setCursor(A...){}        template<class... A> void print(A...){}
    template<class... A> void writecommand(A...){}     template<class... A> void writedata(A...){}
    template<class... A> void fillEllipse(A...){}      template<class... A> void drawEllipse(A...){}
    template<class... A> uint16_t color565(A...){return 0;} template<class... A> void setTextWrap(A...){}
    template<class... A> void drawChar(A...){}         template<class... A> void setFreeFont(A...){}
    template<class... A> void setTextFont(A...){}      template<class... A> void* getPointer(A...){return nullptr;}
    template<class... A> void drawBitmap(A...){}       template<class... A> void drawXBitmap(A...){}
    template<class... A> void drawWideLine(A...){}     template<class... A> void fillSmoothRoundRect(A...){}
};
struct TFT_eSPI : TFT_eSPI_Base {};
struct TFT_eSprite : TFT_eSPI_Base { TFT_eSprite(){} TFT_eSprite(TFT_eSPI*){} };
#define TFT_RED 0xF800
#define TFT_GREEN 0x07E0
#define TFT_BLUE 0x001F
#define TFT_YELLOW 0xFFE0
#define TFT_CYAN 0x07FF
#define TFT_MAGENTA 0xF81F
#define TFT_ORANGE 0xFDA0
#define TFT_DARKGREY 0x7BEF
#define TFT_NAVY 0x000F
#define TFT_DARKGREEN 0x03E0
#define TFT_DARKCYAN 0x03EF
#define TFT_MAROON 0x7800
#define TFT_PURPLE 0x780F
#define TFT_OLIVE 0x7BE0
#define TFT_LIGHTGREY 0xD69A
#define TFT_PINK 0xFE19
#define TFT_BROWN 0x9A60
#define TFT_GOLD 0xFEA0
#define TFT_SILVER 0xC618
#define TFT_SKYBLUE 0x867D
#define TFT_VIOLET 0x915C
