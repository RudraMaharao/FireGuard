#pragma once
#include <stdint.h>
struct FakeStop{};
inline uint32_t fake_ticks=1000;
inline int fake_delay_budget=-1; // >=0: throw FakeStop after this many delays (ends an endless task loop)
inline void vTaskDelay(unsigned t){fake_ticks+=t;if(fake_delay_budget>=0&&fake_delay_budget--==0)throw FakeStop{};}
inline uint32_t xTaskGetTickCount(){return fake_ticks;}
inline void(*fake_task_fn)(void*)=nullptr;
inline int xTaskCreate(void(*fn)(void*),const char*,unsigned,void*,unsigned,void*){fake_task_fn=fn;return 1;}
