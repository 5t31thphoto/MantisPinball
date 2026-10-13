#pragma once
#include <stddef.h>
#include <stdint.h>
#define MALLOC_CAP_8BIT 4
#define MALLOC_CAP_INTERNAL 2048
#define MALLOC_CAP_SPIRAM 1024
size_t heap_caps_get_free_size(uint32_t); size_t heap_caps_get_largest_free_block(uint32_t); void* heap_caps_malloc(size_t,uint32_t);
size_t heap_caps_get_total_size(uint32_t); size_t heap_caps_get_minimum_free_size(uint32_t);
