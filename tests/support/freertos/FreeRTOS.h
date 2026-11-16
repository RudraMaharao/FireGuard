#pragma once
#include <assert.h>
#define pdMS_TO_TICKS(x) (x)
#define configASSERT(x) assert(x)
#define portMAX_DELAY 0xffffffffu
#include <stdint.h>
using TickType_t=uint32_t;
#define pdTICKS_TO_MS(x) (x)
#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
