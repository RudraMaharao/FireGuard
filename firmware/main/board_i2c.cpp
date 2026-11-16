#include "board_i2c.h"
#include "config.h"
namespace board_i2c {
namespace {
i2c_master_bus_handle_t handle = nullptr;
}
esp_err_t init() {
    i2c_master_bus_config_t c = {};
    c.i2c_port = I2C_NUM_0;
    c.sda_io_num = gpio_num_t(PIN_I2C_SDA);
    c.scl_io_num = gpio_num_t(PIN_I2C_SCL);
    c.clk_source = I2C_CLK_SRC_DEFAULT;
    c.glitch_ignore_cnt = 7;
    c.flags.enable_internal_pullup = true;
    return i2c_new_master_bus(&c, &handle);
}
i2c_master_bus_handle_t bus() { return handle; }
} // namespace board_i2c
