#pragma once
#include <stdint.h>
template<class... A> int esp_sleep_pd_config(A...){return 0;}
template<class... A> int esp_sleep_enable_ext1_wakeup(A...){return 0;}
template<class... A> int esp_sleep_enable_ext0_wakeup(A...){return 0;}
template<class... A> int esp_sleep_get_wakeup_cause(A...){return 0;}
inline void esp_deep_sleep_start(){}
#define ESP_PD_DOMAIN_RTC_PERIPH 0
#define ESP_PD_OPTION_ON 1
#define ESP_EXT1_WAKEUP_ALL_LOW 0
#define ESP_EXT1_WAKEUP_ANY_HIGH 1
#define ESP_SLEEP_WAKEUP_EXT1 3
#define ESP_SLEEP_WAKEUP_EXT0 2
#define ESP_PD_OPTION_OFF 0
#define ESP_PD_DOMAIN_RTC_SLOW_MEM 1
#define ESP_PD_DOMAIN_RTC_FAST_MEM 2
#define ESP_EXT1_WAKEUP_ANY_LOW 2
