#pragma once
#include <stdint.h>
#define WIFI_SECOND_CHAN_NONE 0
typedef int esp_err_t;
#ifndef ESP_OK
#define ESP_OK 0
#endif
#define ESP_ERR_INVALID_STATE 0x103
typedef struct { int nvs_enable, static_rx_buf_num, dynamic_rx_buf_num, ampdu_rx_enable, ampdu_tx_enable; } wifi_init_config_t;
#define WIFI_INIT_CONFIG_DEFAULT() wifi_init_config_t{1,10,32,1,1}
typedef enum { WIFI_STORAGE_FLASH, WIFI_STORAGE_RAM } wifi_storage_t;
typedef enum { WIFI_MODE_NULL, WIFI_MODE_STA } wifi_mode_t;
typedef enum { WIFI_IF_STA = 0 } wifi_interface_t;
int esp_wifi_set_promiscuous(bool); int esp_wifi_set_channel(int,int);
esp_err_t esp_wifi_init(const wifi_init_config_t*); esp_err_t esp_wifi_deinit(); esp_err_t esp_wifi_set_storage(wifi_storage_t);
esp_err_t esp_wifi_set_mode(wifi_mode_t); esp_err_t esp_wifi_start(); esp_err_t esp_wifi_stop();
