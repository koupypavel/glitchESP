#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_cache.h"
#include "esp_random.h"
#include "ov5647_types.h"
#include "cam_ctrl.h"
#include "app_video.h"
#include "ov5647_ctl.h"
#include "auto_exposure.h"
#include "frame_pipeline.h"
#include "capture.h"
#include "settings.h"
#include "fx.h"

static const char *TAG = "cam";

#define REG_END     0xffff          /* table terminator used by the OV5647 driver */
#define R16(reg, v) { (reg), (uint8_t)((v) >> 8) }, { (reg) + 1, (uint8_t)((v) & 0xff) }

/*
 * All modes reuse the driver's own 800x1280 register table (clocks, analog settings, MIPI)
 * and override only geometry and timing: the entries below are appended to a copy of it,
 * and since registers are written in order the later value wins.
 *
 * Sensor array: 2624 x 1956, optical centre at (1312, 978). The sensor takes its output
 * from the middle of the readout window, so every window here is centred and only a few
 * pixels larger than the output.
 * Pixel clock in this configuration: about 66.7 MHz (measured from the frame rate).
 */

/* WIDE: 2x2 binning. Window 1296 x 1932 sensor pixels -> 648 x 966 binned -> 640 x 960
 * output. Line 1896 clocks (28 us), 1760 lines per frame -> 20 fps, longest exposure 50 ms. */
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

/* TELE: the driver's 800x1280 mode with faster frame timing and a centred 816 x 1288 window
 * (the stock window is 2110 wide and sits left of centre, so its picture is off-centre). */
#define TELE_W      800
#define TELE_H      1280
#define TELE_COLS   816
#define TELE_ROWS   1288
static uint16_t s_tele_x0 = 904, s_tele_y0 = 332;       /* window origin, see build_tele() */

/* STILL: no binning, 1088 x 1920: the same view as WIDE with every sensor pixel, for one
 * photo at a time. 1952 lines of 35.9 us -> 14 fps, longest exposure 70 ms. */
#define STILL_W     1088
#define STILL_H     1920
#define STILL_HTS   OV5647_HTS_FAST
#define STILL_VTS   1952
#define STILL_X0    760
#define STILL_Y0    12
static const ov5647_reginfo_t k_still[] = {
    R16(0x3800, STILL_X0),
    R16(0x3802, STILL_Y0),
    R16(0x3804, STILL_X0 + STILL_W + 16 - 1),
    R16(0x3806, STILL_Y0 + STILL_H + 8 - 1),
    R16(0x3808, STILL_W),
    R16(0x380a, STILL_H),
    R16(0x380c, STILL_HTS),
    R16(0x380e, STILL_VTS),
};
#define STILL_BYTES         ((size_t)STILL_W * STILL_H * 2)

/* Low light (see cam_ctrl.h) */
#define NIGHT_GAIN_PREVIEW  (24 * 16)   /* gain ceiling at the top night level; the averaging hides the rest */
#define NIGHT_TICKS         3           /* seconds a condition must hold before the level changes */
#define STILL_BUFS          2
#define STILL_SET_FRAME     2       /* exposure/gain are written once this frame has arrived */
#define STILL_KEEP_FRAME    7       /* ...and this one is the photo */
#define STILL_KEEP_NEXT     3       /* later photos of a burst: nothing has to settle any more */

enum { MODE_STILL = CAM_MODE_COUNT, MODE_TOTAL };

typedef struct {
    const char *name;
    esp_cam_sensor_format_t fmt;
    ov5647_reginfo_t *regs;
    uint32_t view_base_w;           /* camera pixels across the screen at zoom 1.0 */
    uint32_t hts, vts;
    uint32_t sensitivity;           /* relative light per output pixel: binning collects about twice as much (measured) */
} mode_desc_t;

static mode_desc_t s_mode[MODE_TOTAL] = {
    [CAM_MODE_WIDE] = { .name = "binned 2x2, 640x960", .view_base_w = 540, .hts = WIDE_HTS, .vts = WIDE_VTS, .sensitivity = 2 },
    [CAM_MODE_TELE] = { .name = "1:1, 800x1280", .view_base_w = 1080, .hts = OV5647_HTS_FAST, .vts = OV5647_VTS_FAST, .sensitivity = 1 },
    [MODE_STILL]    = { .name = "1:1, 1088x1920 still", .hts = STILL_HTS, .vts = STILL_VTS, .sensitivity = 1 },
};

static struct {
    int fd;
    uint8_t *block;                 /* one PSRAM region, carved into preview or still buffers */
    size_t preview_buf_len;
    int preview_bufs;
    esp_cam_sensor_format_t base;
    volatile cam_mode_t mode;
    volatile float zoom;            /* what the user asked for */
    TaskHandle_t task;
    volatile bool busy;             /* the task is switching modes or taking a still */
    volatile bool paused;           /* stream stopped, the buffer block is lent out (gallery) */

    /* high-resolution still in progress */
    volatile bool still_request, still_dump;
    volatile int still_frames;
    volatile int still_kept;        /* buffer index of the photo, -1 until it arrives */
    volatile int still_keep_at;     /* which frame of the run to keep */
    volatile int still_shots;       /* photos requested by this press (burst) */
    uint32_t still_expo, still_gain;
    uint8_t still_wb[6];            /* white-balance gains of the preview (R, G, B; 0x400 = 1x) */
    bool still_wb_valid;
    SemaphoreHandle_t still_done;

    /* low light */
    int night;                      /* current level */
    volatile int night_force;       /* -1 automatic */
    int night_up, night_down;       /* seconds the level change has been due */
} s_c = { .zoom = CAM_ZOOM_MIN, .night_force = -1 };

static int base_len(const ov5647_reginfo_t *regs, int max)
{
    int n = 0;
    while (n < max && regs[n].reg != REG_END) n++;
    return n;
}

static esp_err_t build_mode(int m, const ov5647_reginfo_t *extra, int n_extra, uint16_t w, uint16_t h)
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

/* Change a mode's frame length (lines) in its register table. */
static void set_mode_vts(int m, uint32_t vts)
{
    if (vts > 0xffff) vts = 0xffff;
    for (ov5647_reginfo_t *r = s_mode[m].regs; r && r->reg != REG_END; r++) {
        if (r->reg == 0x380e) r->val = (uint8_t)(vts >> 8);
        if (r->reg == 0x380f) r->val = (uint8_t)(vts & 0xff);
    }
    s_mode[m].vts = vts;
}

static uint32_t night_gain_cap(int level)
{
    return level >= CAM_NIGHT_LEVELS - 1 ? NIGHT_GAIN_PREVIEW : 16 * 16;
}

/* Program a sensor mode and hand the driver `n` buffers of `len` bytes from the block. */
static esp_err_t program(int m, int n, size_t len)
{
    const void *bufs[8];
    for (int i = 0; i < n; i++) bufs[i] = s_c.block + (size_t)i * len;
    ESP_RETURN_ON_ERROR(app_video_set_sensor_format(&s_mode[m].fmt), TAG, "sensor format");
    ESP_RETURN_ON_ERROR(app_video_set_bufs(s_c.fd, (uint32_t)n, bufs), TAG, "buffers");
    settings_apply();                                   /* mirror/flip live in registers the table rewrote */
    return ESP_OK;
}

/* Stream must be stopped. */
static esp_err_t apply_mode(cam_mode_t m, cam_mode_t from)
{
    /* the night level lengthens the wide mode's frames; the others keep theirs */
    int level = m == CAM_MODE_WIDE ? s_c.night : 0;
    if (m == CAM_MODE_WIDE) set_mode_vts(CAM_MODE_WIDE, WIDE_VTS * (uint32_t)(1 + level));
    auto_exposure_set_max_gain(night_gain_cap(level));
    frame_pipeline_set_denoise(level > 0);
    ESP_RETURN_ON_ERROR(program(m, s_c.preview_bufs, s_c.preview_buf_len), TAG, "mode");
    /* same exposure time in the new mode: lines scale with the line length */
    auto_exposure_restart(OV5647_EXPO_MAX(s_mode[m].vts), s_mode[from].hts, s_mode[m].hts,
                          s_mode[from].sensitivity, s_mode[m].sensitivity);
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
    uint8_t wb[6];
    auto_exposure_read_wb(wb);                          /* remembered for the new mode */
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

/* ---- high-resolution still ---- */

/* Frame callback while the STILL mode streams (camera task). */
static void still_frame_cb(uint8_t *buf, uint8_t idx, uint32_t w, uint32_t h, size_t len, void *user)
{
    (void)buf; (void)w; (void)h; (void)len; (void)user;
    int n = ++s_c.still_frames;
    if (n == STILL_SET_FRAME) {
        /* only takes effect when written while the sensor is streaming */
        ov5647_ctl_set_exposure_lines(s_c.still_expo);
        ov5647_ctl_set_gain_x16(s_c.still_gain);
        /* The sensor's auto white balance starts over from neutral gains after every mode
         * change and needs about a second; a frame taken earlier comes out green. So the
         * gains it had settled on in the preview are applied as manual gains. */
        if (s_c.still_wb_valid) {
            for (int i = 0; i < 6; i++) ov5647_ctl_write(OV5647_REG_WB_MANUAL + i, s_c.still_wb[i]);
            ov5647_ctl_write(OV5647_REG_WB_CTRL, OV5647_WB_MANUAL_EN);
        }
    }
    if (n == s_c.still_keep_at && s_c.still_kept < 0) {
        s_c.still_kept = idx;                           /* not released: the driver cannot overwrite it */
        xSemaphoreGive(s_c.still_done);
        return;
    }
    app_video_release_frame(idx);
}

static void take_still(bool dump)
{
    int64_t t0 = esp_timer_get_time();
    fp_recipe_t recipe;
    frame_pipeline_get_recipe(&recipe);
    ae_state_t ae;
    auto_exposure_get(&ae);
    s_c.still_wb_valid = auto_exposure_read_wb(s_c.still_wb);

    if (app_video_stream_stop_wait(2000) != ESP_OK) {
        ESP_LOGE(TAG, "stream did not stop");
        capture_finish_external(NULL, 0, 0, &recipe, dump, false);
        return;
    }

    /* Same exposure time as the preview (lines scale with the line length); the light that
     * binning no longer collects is made up with gain, and past the gain limit with time. */
    uint32_t max_expo = OV5647_EXPO_MAX(STILL_VTS);
    uint32_t expo = (uint32_t)((uint64_t)ae.exposure_lines * s_mode[s_c.mode].hts / STILL_HTS);
    uint32_t gain = ae.gain_x16 * s_mode[s_c.mode].sensitivity / s_mode[MODE_STILL].sensitivity;
    if (gain > ae.max_gain_x16) {
        expo = (uint32_t)((uint64_t)expo * gain / ae.max_gain_x16);
        gain = ae.max_gain_x16;
    }
    if (expo > max_expo) expo = max_expo;
    if (expo < 8) expo = 8;
    s_c.still_expo = expo;
    s_c.still_gain = gain;
    recipe.zoom = 1.0f;

    esp_err_t ret = program(MODE_STILL, STILL_BUFS, STILL_BYTES);
    if (ret != ESP_OK) ESP_LOGE(TAG, "still mode failed: %s", esp_err_to_name(ret));
    app_video_register_frame_operation_cb(still_frame_cb);

    /* One photo per pass. A burst stays in this sensor mode: the stream is only stopped
     * while a frame is processed, because the effect chain needs both buffers. */
    int shots = s_c.still_shots < 1 ? 1 : s_c.still_shots;
    for (int shot = 0; shot < shots; shot++) {
        const uint8_t *result = NULL;
        if (ret == ESP_OK) {
            if (shot > 0) {
                recipe.seed = esp_random();                 /* every burst photo glitches differently */
                capture_notify_shot();
                const void *bufs[STILL_BUFS] = { s_c.block, s_c.block + STILL_BYTES };
                app_video_set_bufs(s_c.fd, STILL_BUFS, bufs);
            } else {
                ov5647_ctl_set_manual(true, true);
            }
            s_c.still_frames = 0;
            s_c.still_kept = -1;
            s_c.still_keep_at = shot == 0 ? STILL_KEEP_FRAME : STILL_KEEP_NEXT;   /* exposure is settled by then */
            xSemaphoreTake(s_c.still_done, 0);
            app_video_stream_task_start(s_c.fd, 1, NULL);
            bool got = xSemaphoreTake(s_c.still_done, pdMS_TO_TICKS(3000)) == pdTRUE;
            app_video_stream_stop_wait(2000);
            if (got) {
                int64_t t1 = esp_timer_get_time();
                fx_frame_t a = { (uint16_t *)(s_c.block + (size_t)s_c.still_kept * STILL_BYTES), STILL_W, STILL_H, STILL_W };
                fx_frame_t b = { (uint16_t *)(s_c.block + (size_t)(1 - s_c.still_kept) * STILL_BYTES), STILL_W, STILL_H, STILL_W };
                fx_ctx_t ctx = { .seed = recipe.seed, .frame_no = recipe.frame_no, .prev = NULL };
                fx_frame_t *out = fx_chain_apply_pingpong(&recipe.chain, &a, &b, &ctx);
                /* the JPEG encoder reads memory, not this core's cache */
                esp_cache_msync(out->px, STILL_BYTES, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
                result = (const uint8_t *)out->px;
                ESP_LOGI(TAG, "still %d/%d: %d frames, exposure %lu lines, gain %lu/16, effects %lld ms", shot + 1, shots,
                         s_c.still_frames, (unsigned long)expo, (unsigned long)gain,
                         (long long)((esp_timer_get_time() - t1) / 1000));
            } else {
                ESP_LOGE(TAG, "still: no frame (%d arrived)", s_c.still_frames);
            }
        }
        if (!capture_finish_external(result, STILL_W, STILL_H, &recipe, dump, shot + 1 < shots)) break;
    }
    app_video_register_frame_operation_cb(frame_pipeline_on_camera_frame);

    /* back to the preview */
    apply_mode(s_c.mode, s_c.mode);
    frame_pipeline_set_zoom(s_c.zoom);
    app_video_stream_task_start(s_c.fd, 1, NULL);
    ESP_LOGI(TAG, "still done in %lld ms", (long long)((esp_timer_get_time() - t0) / 1000));
}

/* Once a second: should the wide mode's frames get longer, or shorter again? */
static void night_tick(void)
{
    if (s_c.paused || s_c.still_request || s_c.mode != CAM_MODE_WIDE || capture_video_active() || capture_busy()) {
        s_c.night_up = s_c.night_down = 0;
        return;
    }
    int want = s_c.night_force;
    if (want < 0) {
        ae_state_t ae;
        auto_exposure_get(&ae);
        if (ae.frame < 30) return;                      /* still settling after a restart */
        uint32_t t = ae.target_luma, l = ae.measured_luma;
        bool starved = ae.exposure_lines + 16 >= ae.max_exposure_lines && ae.gain_x16 >= ae.max_gain_x16 &&
                       l * 100 < t * 80;
        /* could the level below give this brightness with half of its exposure x gain to spare? */
        bool plenty = false;
        if (s_c.night > 0 && l * 100 > t * 75) {
            uint64_t now = (uint64_t)ae.exposure_lines * ae.gain_x16;
            uint64_t below = (uint64_t)OV5647_EXPO_MAX(WIDE_VTS * (uint32_t)s_c.night) * night_gain_cap(s_c.night - 1);
            plenty = now * 2 < below;
        }
        s_c.night_up = starved ? s_c.night_up + 1 : 0;
        s_c.night_down = plenty ? s_c.night_down + 1 : 0;
        want = s_c.night;
        if (s_c.night_up >= NIGHT_TICKS && s_c.night < CAM_NIGHT_LEVELS - 1) want++;
        if (s_c.night_down >= NIGHT_TICKS && s_c.night > 0) want--;
    }
    if (want == s_c.night) return;
    s_c.night_up = s_c.night_down = 0;
    ESP_LOGI(TAG, "night level %d -> %d", s_c.night, want);
    s_c.night = want;
    switch_mode(CAM_MODE_WIDE);
}

static void cam_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (!ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000))) {
            s_c.busy = true;
            night_tick();
            s_c.busy = false;
            continue;
        }
        if (s_c.paused) continue;                       /* resume() picks the mode for the zoom */
        s_c.busy = true;
        if (s_c.still_request) {
            take_still(s_c.still_dump);
            s_c.still_request = false;
        }
        cam_mode_t want = mode_for(s_c.zoom);
        if (want != s_c.mode && !s_c.paused) switch_mode(want);
        frame_pipeline_set_zoom(s_c.zoom);
        s_c.busy = false;
    }
}

esp_err_t cam_ctrl_init(int video_fd, void *block, size_t preview_buf_len, int preview_bufs)
{
    ESP_RETURN_ON_FALSE(preview_bufs <= 8, ESP_ERR_INVALID_ARG, TAG, "too many buffers");
    ESP_RETURN_ON_FALSE(preview_buf_len * (size_t)preview_bufs >= STILL_BYTES * STILL_BUFS, ESP_ERR_INVALID_SIZE, TAG,
                        "block too small for stills");
    s_c.fd = video_fd;
    s_c.block = block;
    s_c.preview_buf_len = preview_buf_len;
    s_c.preview_bufs = preview_bufs;
    s_c.still_done = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(s_c.still_done, ESP_ERR_NO_MEM, TAG, "semaphore");

    ESP_RETURN_ON_ERROR(app_video_get_sensor_format(&s_c.base), TAG, "get sensor format");
    ESP_LOGI(TAG, "base sensor format '%s' %ux%u, %d registers", s_c.base.name, s_c.base.width, s_c.base.height,
             s_c.base.regs_size);
    ESP_RETURN_ON_ERROR(build_mode(CAM_MODE_WIDE, k_wide, sizeof(k_wide) / sizeof(k_wide[0]), WIDE_W, WIDE_H), TAG, "wide");
    ESP_RETURN_ON_ERROR(build_tele(), TAG, "tele");
    ESP_RETURN_ON_ERROR(build_mode(MODE_STILL, k_still, sizeof(k_still) / sizeof(k_still[0]), STILL_W, STILL_H), TAG, "still");

    ESP_RETURN_ON_ERROR(ov5647_ctl_init(), TAG, "sensor i2c");
    uint32_t max_expo = OV5647_EXPO_MAX(WIDE_VTS);
    ESP_RETURN_ON_ERROR(auto_exposure_init(max_expo / 2, 16, max_expo), TAG, "auto exposure");
    return apply_mode(CAM_MODE_WIDE, CAM_MODE_WIDE);
}

esp_err_t cam_ctrl_start(void)
{
    frame_pipeline_set_zoom(s_c.zoom);
    ESP_RETURN_ON_ERROR(app_video_stream_task_start(s_c.fd, 1, NULL), TAG, "stream");
    /* core 1: the still path runs the effect chain here while the camera task is stopped */
    BaseType_t ok = xTaskCreatePinnedToCore(cam_task, "cam_ctrl", 16 * 1024, NULL, 5, &s_c.task, 1);
    return ok == pdPASS ? ESP_OK : ESP_FAIL;
}

float cam_ctrl_set_zoom(float zoom)
{
    if (zoom < CAM_ZOOM_MIN) zoom = CAM_ZOOM_MIN;
    if (zoom > CAM_ZOOM_MAX) zoom = CAM_ZOOM_MAX;
    if (s_c.paused) {
        s_c.zoom = zoom;
        return zoom;
    }
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
int cam_ctrl_night_level(void)         { return s_c.mode == CAM_MODE_WIDE ? s_c.night : 0; }

void cam_ctrl_set_night(int level)
{
    s_c.night_force = level < 0 ? -1 : (level >= CAM_NIGHT_LEVELS ? CAM_NIGHT_LEVELS - 1 : level);
    s_c.night_up = s_c.night_down = 0;
}
cam_mode_t cam_ctrl_mode(void)         { return s_c.mode; }
const char *cam_ctrl_mode_name(void)   { return s_mode[s_c.still_request ? MODE_STILL : s_c.mode].name; }

bool cam_ctrl_still_available(void)
{
    /* not at a night level: there the binned frames collect twice the light per pixel, and
     * the photo averages several of them (frame_pipeline.c) */
    return s_c.task && !s_c.paused && s_c.mode == CAM_MODE_WIDE && s_c.zoom < CAM_ZOOM_MIN + 0.01f &&
           !frame_pipeline_chain_is_temporal() && s_c.night == 0;
}

esp_err_t cam_ctrl_take_still(bool dump, int shots)
{
    if (!cam_ctrl_still_available() || s_c.still_request) return ESP_ERR_INVALID_STATE;
    ESP_RETURN_ON_ERROR(capture_begin_external(), TAG, "capture busy");
    s_c.still_dump = dump;
    s_c.still_shots = shots;
    s_c.still_request = true;
    xTaskNotifyGive(s_c.task);
    return ESP_OK;
}

esp_err_t cam_ctrl_pause(uint8_t **block, size_t *len)
{
    if (!s_c.task || s_c.paused || s_c.busy || s_c.still_request || capture_busy() || capture_video_active()) {
        return ESP_ERR_INVALID_STATE;
    }
    s_c.paused = true;
    uint8_t wb[6];
    auto_exposure_read_wb(wb);                          /* remembered for the restart */
    esp_err_t ret = app_video_stream_stop_wait(2000);
    if (ret != ESP_OK) {
        s_c.paused = false;
        return ret;
    }
    *block = s_c.block;
    *len = s_c.preview_buf_len * (size_t)s_c.preview_bufs;
    return ESP_OK;
}

void cam_ctrl_resume(void)
{
    if (!s_c.paused) return;
    apply_mode(mode_for(s_c.zoom), s_c.mode);
    frame_pipeline_set_zoom(s_c.zoom);
    app_video_stream_task_start(s_c.fd, 1, NULL);
    s_c.paused = false;
}

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
