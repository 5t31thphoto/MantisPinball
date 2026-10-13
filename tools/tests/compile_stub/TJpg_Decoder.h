#pragma once
#include <stdint.h>
#include <stddef.h>
#define JDR_OK 0
typedef int JRESULT;
struct TJpg_Decoder { template<class... A> void setJpgScale(A...){} template<class... A> void setSwapBytes(A...){} template<class... A> void setCallback(A...){} template<class... A> JRESULT drawJpg(A...){return 0;} template<class... A> JRESULT getJpgSize(A...){return 0;} };
extern TJpg_Decoder TJpgDec;
