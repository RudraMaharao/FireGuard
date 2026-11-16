#pragma once
#include <stdint.h>
using gpio_num_t=int;
constexpr int GPIO_MODE_OUTPUT=1;
struct gpio_config_t { uint64_t pin_bit_mask;int mode; };
inline int gpio_set_level(int,int){return 0;}
inline int gpio_config(gpio_config_t*){return 0;}
