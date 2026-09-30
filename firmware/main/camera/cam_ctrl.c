#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "ov5647_types.h"
#include "cam_ctrl.h"
#include "app_video.h"
#include "ov5647_ctl.h"
#include "auto_exposure.h"
#include "frame_pipeline.h"
#include "capture.h"
#include "settings.h"

static const char *TAG = "cam";

#define REG_END     0xffff          /* table terminator used by the OV5647 driver */
#define R16(reg, v) { (reg), (uint8_t)((v) >> 8) }, { (reg) + 1, (uint8_t)((v) & 0xff) }

/*
 * Both modes reuse the driver's own 800x1280 register table (clocks, analog settings, MIPI)
 * and override only geometry and timing: the entries below are appended to a copy of it,
 * and since registers are written in order the later value wins.
 *
 * Sensor array: 2624 x 1956, optical centre at (1312, 978).
 * Pixel clock in this configuration: about 66.7 MHz (measured from the frame rate).
 */

/* WIDE: 2x2 binning. Window 1296 x 1932 sensor pixels centred on the sensor -> 648 x 966
 * binned -> 640 x 960 output. Line 1896 clocks (28 us), 1760 lines per frame -> 20 fps,
 * longest exposure 50 ms. */
#define WIDE_W      640
#define WIDE_H      960
#define WIDE_HTS    1896
#define WIDE_VTS    1760
#define WIDE_X0     664
#define WIDE_Y0     12
static const ov5647_reginfo_t k_wide[] = {
    { 0x3820, 0x41 },               /* vertical binning on */
    { 0x3821, 0x03 },               /* horizontal binning on (+ mirror, as in the base table) */
    { 0x3814, 0x31 },               /* read every other pixel pair horizontally... */
    { 0x3815, 0x31 },               /* ...and vertically */
    { 0x370c, 0x03 },
    R16(0x3800, WIDE_X0),
    R16(0x3802, WIDE_Y0),
    R16(0x3804, WIDE_X0 + 2 * 648 - 1),
    R16(0x3806, WIDE_Y0 + 2 * 966 - 1),
    R16(0x3808, WIDE_W),
    R16(0x380a, WIDE_H),
    R16(0x3810, 4),
    R16(0x3812, 2),
    R16(0x380c, WIDE_HTS),
    R16(0x380e, WIDE_VTS),
};

/* TELE: the driver's 800x1280 mode with faster frame timing, and with the readout window
 * shrunk to just the output size plus a small margin, centred on the sensor. (The sensor
 * takes the output from the middle of the window; the stock window is 2110 wide and sits
 * left of centre, so the two modes would not look at the same spot.) */
#define TELE_W      800
#define TELE_H      1280
#define TELE_COLS   816
#define TELE_ROWS   1288
static uint16_t s_tele_x0 = 904, s_tele_y0 = 332;       /* window origin, see build_tele() */

typedef struct {
    const char *name;
    esp_cam_sensor_format_t fmt;
    ov5647_reginfo_t *regs;
    uint32_t view_base_w;           /* camera pixels across the screen at zoom 1.0 */
    uint32_t hts, vts;
} mode_desc_t;

static mode_desc_t s_mode[CAM_MODE_COUNT] = {
    [CAM_MODE_WIDE] = { .name = "binned 2x2, 640x960", .view_base_w = 540, .hts = WIDE_HTS, .vts = WIDE_VTS },
    [CAM_MODE_TELE] = { .name = "1:1, 800x1280", .view_base_w = 1080, .hts = OV5647_HTS_FAST, .vts = OV5647_VTS_FAST },
};

static struct {
    int fd;
    void *bufs[8];
    int nbufs;
    esp_cam_sensor_format_t base;
    volatile cam_mode_t mode;
    volatile float zoom;            /* what the user asked for */
    TaskHandle_t task;
    bool streaming;
} s_c = { .zoom = CAM_ZOOM_MIN };

static int base_len(const ov5647_reginfo_t *regs, int max)
{
    int n = 0;
    while (n < max && regs[n].reg != REG_END) n++;
    return n;
}

static esp_err_t build_mode(cam_mode_t m, const ov5647_reginfo_t *extra, int n_extra, uint16_t w, uint16_t h)
{
    const ov5647_reginfo_t *base = s_c.base.regs;
    int n_base = base_len(base, s_c.base.regs_size);
    free(s_mode[m].regs);
    ov5647_reginfo_t *t = malloc((size_t)(n_base + n_extra + 1) * sizeof(*t));
    ESP_RETURN_ON_FALSE(t, ESP_ERR_NO_MEM, TAG, "mode table");
    memcpy(t, base, (size_t)n_base * sizeof(*t));
    memcpy(t + n_base, extra, (size_t)n_extra * sizeof(*t));
    t[n_base + n_extra] = (ov5647_reginfo_t){ REG_END, 0 };
    s_mode[m].regs = t;
    s_mode[m].fmt = s_c.base;
    s_mode[m].fmt.name = s_mode[m].name;
    s_mode[m].fmt.width = w;
    s_mode[m].fmt.height = h;
    s_mode[m].fmt.regs = t;
    s_mode[m].fmt.regs_size = n_base + n_extra + 1;
    return ESP_OK;
}

static esp_err_t build_tele(void)
{
    const ov5647_reginfo_t tele[] = {
        R16(0x3800, s_tele_x0),
        R16(0x3802, s_tele_y0),
        R16(0x3804, s_tele_x0 + TELE_COLS - 1),
        R16(0x3806, s_tele_y0 + TELE_ROWS - 1),
        R16(0x380c, OV5647_HTS_FAST),
        R16(0x380e, OV5647_VTS_FAST),
    };
    return build_mode(CAM_MODE_TELE, tele, sizeof(tele) / sizeof(tele[0]), TELE_W, TELE_H);
}

/* Stream must be stopped. */
static esp_err_t apply_mode(cam_mode_t m, cam_mode_t from)
{
    ESP_RETURN_ON_ERROR(app_video_set_sensor_format(&s_mode[m].fmt), TAG, "sensor format");
    ESP_RETURN_ON_ERROR(app_video_set_bufs(s_c.fd, (uint32_t)s_c.nbufs, (const void **)s_c.bufs), TAG, "buffers");
    /* same exposure time in the new mode: lines scale with the line length */
    auto_exposure_restart(OV5647_EXPO_MAX(s_mode[m].vts), s_mode[from].hts, s_mode[m].hts);
    settings_apply();                                   /* mirror/flip live in registers the table rewrote */
    frame_pipeline_set_view_base(s_mode[m].view_base_w);
    s_c.mode = m;
    return ESP_OK;
}

static cam_mode_t mode_for(float zoom)
{
    return zoom < CAM_ZOOM_TELE - 0.01f ? CAM_MODE_WIDE : CAM_MODE_TELE;
}

static void switch_mode(cam_mode_t m)
{
    int64_t t0 = esp_timer_get_time();
    esp_err_t ret = app_video_stream_stop_wait(2000);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "stream did not stop: %s", esp_err_to_name(ret));
        return;
    }
    cam_mode_t from = s_c.mode;
    ret = apply_mode(m, from);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "mode %s failed (%s), going back", s_mode[m].name, esp_err_to_name(ret));
        apply_mode(from, from);
    }
    frame_pipeline_set_zoom(s_c.zoom);
    app_video_stream_task_start(s_c.fd, 1, NULL);
    ESP_LOGI(TAG, "mode -> %s in %lld ms", s_mode[s_c.mode].name, (long long)((esp_timer_get_time() - t0) / 1000));
}

static void cam_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        cam_mode_t want = mode_for(s_c.zoom);
        if (want != s_c.mode) switch_mode(want);
        frame_pipeline_set_zoom(s_c.zoom);
    }
}

esp_err_t cam_ctrl_init(int video_fd, void *const *bufs, int nbufs)
{
    ESP_RETURN_ON_FALSE(nbufs <= 8, ESP_ERR_INVALID_ARG, TAG, "too many buffers");
    s_c.fd = video_fd;
    s_c.nbufs = nbufs;
    memcpy(s_c.bufs, bufs, (size_t)nbufs * sizeof(void *));

    ESP_RETURN_ON_ERROR(app_video_get_sensor_format(&s_c.base), TAG, "get sensor format");
    ESP_LOGI(TAG, "base sensor format '%s' %ux%u, %d registers", s_c.base.name, s_c.base.width, s_c.base.height,
             s_c.base.regs_size);
    ESP_RETURN_ON_ERROR(build_mode(CAM_MODE_WIDE, k_wide, sizeof(k_wide) / sizeof(k_wide[0]), WIDE_W, WIDE_H), TAG, "wide");
    ESP_RETURN_ON_ERROR(build_tele(), TAG, "tele");

    ESP_RETURN_ON_ERROR(ov5647_ctl_init(), TAG, "sensor i2c");
    uint32_t max_expo = OV5647_EXPO_MAX(WIDE_VTS);
    ESP_RETURN_ON_ERROR(auto_exposure_init(max_expo / 2, 16, max_expo), TAG, "auto exposure");
    return apply_mode(CAM_MODE_WIDE, CAM_MODE_WIDE);
}

esp_err_t cam_ctrl_start(void)
{
    frame_pipeline_set_zoom(s_c.zoom);
    ESP_RETURN_ON_ERROR(app_video_stream_task_start(s_c.fd, 1, NULL), TAG, "stream");
    BaseType_t ok = xTaskCreatePinnedToCore(cam_task, "cam_ctrl", 6 * 1024, NULL, 5, &s_c.task, 0);
    return ok == pdPASS ? ESP_OK : ESP_FAIL;
}

float cam_ctrl_set_zoom(float zoom)
{
    if (zoom < CAM_ZOOM_MIN) zoom = CAM_ZOOM_MIN;
    if (zoom > CAM_ZOOM_MAX) zoom = CAM_ZOOM_MAX;
    if (mode_for(zoom) != s_c.mode) {
        if (capture_video_active() || capture_busy()) {
            /* the stream cannot restart now: stay inside the current mode's range */
            zoom = s_c.mode == CAM_MODE_WIDE ? CAM_ZOOM_TELE - 0.1f : CAM_ZOOM_TELE;
            s_c.zoom = zoom;
            frame_pipeline_set_zoom(zoom);
            return zoom;
        }
        s_c.zoom = zoom;
        if (s_c.task) xTaskNotifyGive(s_c.task);
        return zoom;
    }
    s_c.zoom = zoom;
    frame_pipeline_set_zoom(zoom);
    return zoom;
}

float cam_ctrl_get_zoom(void)          { return s_c.zoom; }
cam_mode_t cam_ctrl_mode(void)         { return s_c.mode; }
const char *cam_ctrl_mode_name(void)   { return s_mode[s_c.mode].name; }

void cam_ctrl_set_tele_origin(uint16_t x0, uint16_t y0)
{
    s_tele_x0 = x0 & ~3u;                               /* multiples of 4 keep the Bayer colour order */
    s_tele_y0 = y0 & ~3u;
    if (capture_video_active() || capture_busy()) return;
    app_video_stream_stop_wait(2000);
    build_tele();
    apply_mode(s_c.mode, s_c.mode);
    app_video_stream_task_start(s_c.fd, 1, NULL);
    ESP_LOGI(TAG, "tele origin %u,%u", s_tele_x0, s_tele_y0);
}
