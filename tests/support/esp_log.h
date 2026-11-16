#pragma once
#include <stdio.h>
#define ESP_LOGI(tag,...) (printf("[%s] ",tag),printf(__VA_ARGS__),printf("\n"))
#define ESP_LOGW(tag,...) ESP_LOGI(tag,__VA_ARGS__)
#define ESP_LOGE(tag,...) ESP_LOGI(tag,__VA_ARGS__)
