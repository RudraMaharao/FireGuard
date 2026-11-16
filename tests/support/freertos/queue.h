#pragma once
// Single-threaded queue stand-in for host tests.
#include <deque>
#include <vector>
#include <string.h>
#include <stdint.h>
struct FakeQueue{size_t item,cap;std::deque<std::vector<uint8_t>> q;};
using QueueHandle_t=FakeQueue*;
inline QueueHandle_t xQueueCreate(size_t cap,size_t item){return new FakeQueue{item,cap,{}};}
inline int xQueueSend(QueueHandle_t h,const void*v,unsigned){if(h->q.size()>=h->cap)return 0;auto*b=(const uint8_t*)v;h->q.emplace_back(b,b+h->item);return 1;}
inline int xQueueReceive(QueueHandle_t h,void*v,unsigned){if(h->q.empty())return 0;memcpy(v,h->q.front().data(),h->item);h->q.pop_front();return 1;}
