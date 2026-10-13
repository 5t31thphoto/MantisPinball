#pragma once
#include "../../stub/freertos/FreeRTOS.h"
typedef void* SemaphoreHandle_t;
template<class... A> SemaphoreHandle_t xSemaphoreCreateMutex(A...){return nullptr;}
template<class... A> SemaphoreHandle_t xSemaphoreCreateBinary(A...){return nullptr;}
template<class... A> int xSemaphoreTake(A...){return 1;}
template<class... A> int xSemaphoreGive(A...){return 1;}
#define portMAX_DELAY 0xFFFFFFFF
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
template<class... A> void portENTER_CRITICAL(A...){}
template<class... A> void portEXIT_CRITICAL(A...){}
