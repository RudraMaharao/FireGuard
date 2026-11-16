#pragma once
#include <mutex>
using SemaphoreHandle_t=std::recursive_mutex*;
inline SemaphoreHandle_t xSemaphoreCreateMutex(){return new std::recursive_mutex;}
inline bool xSemaphoreTake(SemaphoreHandle_t m,unsigned){m->lock();return true;}
inline void xSemaphoreGive(SemaphoreHandle_t m){m->unlock();}
