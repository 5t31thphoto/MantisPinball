#pragma once
#include "FreeRTOS.h"
typedef void* TaskHandle_t;
BaseType_t xTaskCreatePinnedToCore(void(*)(void*),const char*,uint32_t,void*,int,TaskHandle_t*,int);
void vTaskDelete(TaskHandle_t);
