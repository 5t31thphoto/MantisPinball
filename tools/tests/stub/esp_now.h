#pragma once
#include <stdint.h>
#include <stddef.h>
#include "esp_wifi.h"
typedef void (*esp_now_recv_cb_t)(const uint8_t*, const uint8_t*, int);
struct esp_now_peer_info_t { uint8_t peer_addr[6]; uint8_t channel; wifi_interface_t ifidx; bool encrypt; };
esp_err_t esp_now_init(); esp_err_t esp_now_deinit(); esp_err_t esp_now_register_recv_cb(esp_now_recv_cb_t);
esp_err_t esp_now_unregister_recv_cb(); esp_err_t esp_now_add_peer(const esp_now_peer_info_t*);
bool esp_now_is_peer_exist(const uint8_t*); esp_err_t esp_now_send(const uint8_t*, const uint8_t*, size_t);
