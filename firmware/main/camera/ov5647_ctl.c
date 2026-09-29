#include "esp_log.h"
#include "esp_check.h"
#include "driver/i2c_master.h"
#include "bsp/esp-bsp.h"
#include "ov5647_ctl.h"

static const char *TAG = "ov5647_ctl";
static i2c_master_dev_handle_t s_dev;

esp_err_t ov5647_ctl_init(void)
{
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = 0x36,
        .scl_speed_hz = 400000,
    };
    return i2c_master_bus_add_device(bsp_i2c_get_handle(), &cfg, &s_dev);
}

esp_err_t ov5647_ctl_read(uint16_t reg, uint8_t *val)
{
    uint8_t addr[2] = { reg >> 8, reg & 0xff };
    return i2c_master_transmit_receive(s_dev, addr, 2, val, 1, 50);
}

esp_err_t ov5647_ctl_write(uint16_t reg, uint8_t val)
{
    uint8_t buf[3] = { reg >> 8, reg & 0xff, val };
    return i2c_master_transmit(s_dev, buf, 3, 50);
}

static uint32_t rd16(uint16_t hi)
{
    uint8_t h = 0, l = 0;
    ov5647_ctl_read(hi, &h);
    ov5647_ctl_read(hi + 1, &l);
    return ((uint32_t)h << 8) | l;
}

esp_err_t ov5647_ctl_set_manual(bool manual_aec, bool manual_agc)
{
    uint8_t v = 0;
    ESP_RETURN_ON_ERROR(ov5647_ctl_read(0x3503, &v), TAG, "read 3503");
    v = (v & ~0x03) | (manual_aec ? 0x01 : 0) | (manual_agc ? 0x02 : 0);
    return ov5647_ctl_write(0x3503, v);
}

esp_err_t ov5647_ctl_set_exposure_lines(uint32_t lines)
{
    uint32_t v = lines << 4;            /* register holds lines in 1/16 units */
    esp_err_t r = ov5647_ctl_write(0x3500, (v >> 16) & 0x0f);
    r |= ov5647_ctl_write(0x3501, (v >> 8) & 0xff);
    r |= ov5647_ctl_write(0x3502, v & 0xff);
    return r;
}

/* OV5647 gain register: 10-bit linear value in 1/16 steps (0x10 = 1x), as in the Linux driver. */
esp_err_t ov5647_ctl_set_gain_x16(uint32_t gain_x16)
{
    if (gain_x16 < 16) gain_x16 = 16;
    if (gain_x16 > 1023) gain_x16 = 1023;
    esp_err_t r = ov5647_ctl_write(0x350a, (gain_x16 >> 8) & 0x03);
    r |= ov5647_ctl_write(0x350b, gain_x16 & 0xff);
    return r;
}

esp_err_t ov5647_ctl_set_timing(uint16_t hts, uint16_t vts)
{
    esp_err_t ret = ESP_OK;
    if (hts) {
        ret |= ov5647_ctl_write(0x380c, hts >> 8);
        ret |= ov5647_ctl_write(0x380d, hts & 0xff);
    }
    if (vts) {
        ret |= ov5647_ctl_write(0x380e, vts >> 8);
        ret |= ov5647_ctl_write(0x380f, vts & 0xff);
    }
    ESP_LOGI(TAG, "timing: HTS %u VTS %u -> %s", hts, vts, ret == ESP_OK ? "ok" : "FAILED");
    return ret;
}

void ov5647_ctl_dump(void)
{
    uint8_t e0 = 0, e1 = 0, e2 = 0, c3 = 0, gh = 0, gl = 0;
    ov5647_ctl_read(0x3500, &e0); ov5647_ctl_read(0x3501, &e1); ov5647_ctl_read(0x3502, &e2);
    ov5647_ctl_read(0x350a, &gh); ov5647_ctl_read(0x350b, &gl); ov5647_ctl_read(0x3503, &c3);
    uint32_t lines = (((uint32_t)e0 << 16) | ((uint32_t)e1 << 8) | e2) >> 4;
    uint8_t p34 = 0, p35 = 0, p36 = 0, p37 = 0, p106 = 0;
    ov5647_ctl_read(0x3034, &p34); ov5647_ctl_read(0x3035, &p35); ov5647_ctl_read(0x3036, &p36);
    ov5647_ctl_read(0x3037, &p37); ov5647_ctl_read(0x3106, &p106);
    ESP_LOGI(TAG, "exposure %lu lines, gain reg 0x%02x%02x, ctrl03 0x%02x, VTS %lu, HTS %lu | PLL 3034=%02x 3035=%02x 3036=%02x 3037=%02x 3106=%02x",
             (unsigned long)lines, gh, gl, c3, (unsigned long)rd16(0x380e), (unsigned long)rd16(0x380c),
             p34, p35, p36, p37, p106);
}
