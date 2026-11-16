#include "buzzer.h"
#include "config.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace buzzer {

void init()
{
    if (BUZZER_PASSIVE) {
        ledc_timer_config_t t = {};
        t.speed_mode = LEDC_LOW_SPEED_MODE;
        t.duty_resolution = LEDC_TIMER_10_BIT;
        t.timer_num = LEDC_TIMER_0;
        t.freq_hz = BUZZER_TONE_HZ;
        t.clk_cfg = LEDC_AUTO_CLK;
        ledc_timer_config(&t);
        ledc_channel_config_t c = {};
        c.gpio_num = PIN_BUZZER;
        c.speed_mode = LEDC_LOW_SPEED_MODE;
        c.channel = LEDC_CHANNEL_0;
        c.timer_sel = LEDC_TIMER_0;
        c.duty = 0;
        ledc_channel_config(&c);
    } else {
        gpio_config_t io = {};
        io.pin_bit_mask = 1ULL << PIN_BUZZER;
        io.mode = GPIO_MODE_OUTPUT;
        gpio_config(&io);
        gpio_set_level((gpio_num_t)PIN_BUZZER, 0);
    }
}

void set(bool on)
{
    if (BUZZER_PASSIVE) {
        ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, on ? 512 : 0);  // 50% duty = tone
        ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    } else {
        gpio_set_level((gpio_num_t)PIN_BUZZER, on ? 1 : 0);
    }
}

void beep(int ms)
{
    set(true);
    vTaskDelay(pdMS_TO_TICKS(ms));
    set(false);
}

}  // namespace buzzer
