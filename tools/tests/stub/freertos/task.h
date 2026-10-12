#pragma once
#include "FreeRTOS.h"
typedef void* TaskHandle_t;
BaseType_t xTaskCreatePinnedToCore(void(*)(void*),const char*,uint32_t,void*,int,TaskHandle_t*,int);
void vTaskDelete(TaskHandle_t);
void vTaskDelay(uint32_t);
#define portTICK_PERIOD_MS 1
#define pdMS_TO_TICKS(x) (x)
