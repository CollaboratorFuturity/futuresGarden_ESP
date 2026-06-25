#include "I2C_Driver.h"

static const char *I2C_TAG = "I2C";

i2c_master_bus_handle_t i2c_bus_handle = NULL;

#define I2C_MAX_CACHED_DEVS 8
static struct {
    uint8_t addr;
    i2c_master_dev_handle_t dev;
} s_dev_cache[I2C_MAX_CACHED_DEVS];
static int s_dev_count = 0;

static i2c_master_dev_handle_t get_dev(uint8_t addr)
{
    for (int i = 0; i < s_dev_count; i++) {
        if (s_dev_cache[i].addr == addr) return s_dev_cache[i].dev;
    }
    if (s_dev_count >= I2C_MAX_CACHED_DEVS) {
        ESP_LOGE(I2C_TAG, "device cache full, can't add 0x%02X", addr);
        return NULL;
    }
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = I2C_MASTER_FREQ_HZ,
    };
    i2c_master_dev_handle_t dev = NULL;
    if (i2c_master_bus_add_device(i2c_bus_handle, &cfg, &dev) != ESP_OK) {
        return NULL;
    }
    s_dev_cache[s_dev_count].addr = addr;
    s_dev_cache[s_dev_count].dev = dev;
    s_dev_count++;
    return dev;
}

void I2C_Init(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_MASTER_NUM,
        .sda_io_num = I2C_Touch_SDA_IO,
        .scl_io_num = I2C_Touch_SCL_IO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &i2c_bus_handle));
    ESP_LOGI(I2C_TAG, "I2C initialized successfully");
}

esp_err_t I2C_Write(uint8_t Driver_addr, uint8_t Reg_addr, const uint8_t *Reg_data, uint32_t Length)
{
    i2c_master_dev_handle_t dev = get_dev(Driver_addr);
    if (!dev) return ESP_FAIL;
    uint8_t buf[Length + 1];
    buf[0] = Reg_addr;
    memcpy(&buf[1], Reg_data, Length);
    return i2c_master_transmit(dev, buf, Length + 1, I2C_MASTER_TIMEOUT_MS);
}

esp_err_t I2C_Read(uint8_t Driver_addr, uint8_t Reg_addr, uint8_t *Reg_data, uint32_t Length)
{
    i2c_master_dev_handle_t dev = get_dev(Driver_addr);
    if (!dev) return ESP_FAIL;
    return i2c_master_transmit_receive(dev, &Reg_addr, 1, Reg_data, Length, I2C_MASTER_TIMEOUT_MS);
}
