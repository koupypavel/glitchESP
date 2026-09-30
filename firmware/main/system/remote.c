#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_private/esp_cache_private.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_check.h"
#include "mbedtls/base64.h"
#include "sdkconfig.h"
#include "remote.h"
#include "capture.h"
#include "frame_pipeline.h"
#include "ui_live.h"
#include "ui_lvgl.h"
#include "ov5647_ctl.h"
#include "cam_ctrl.h"
#include "auto_exposure.h"
#include "presets.h"
#include "settings.h"
#include "gallery.h"

static const char *TAG = "remote";

#define REMOTE_UART   ((uart_port_t)CONFIG_ESP_CONSOLE_UART_NUM)

static remote_action_t s_photo, s_video;
static void *s_user;

static void cmd_ls(void)
{
    DIR *d = opendir(CAPTURE_DIR);
    if (!d) { printf("ls: cannot open %s\n", CAPTURE_DIR); return; }
    struct dirent *e;
    int n = 0;
    while ((e = readdir(d)) != NULL) {
        char path[300];
        struct stat st;
        snprintf(path, sizeof(path), CAPTURE_DIR "/%s", e->d_name);
        long size = stat(path, &st) == 0 ? (long)st.st_size : -1;
        printf("ls: %-16s %ld\n", e->d_name, size);
        n++;
    }
    closedir(d);
    printf("ls: %d entries\n", n);
}

/* Print a file from the capture folder as base64 lines with an "F:" prefix. */
static void cmd_get(const char *name)
{
    char path[96];
    snprintf(path, sizeof(path), CAPTURE_DIR "/%s", name);
    FILE *f = fopen(path, "rb");
    if (!f) { printf("get: cannot open %s\n", path); return; }
    struct stat st;
    long size = stat(path, &st) == 0 ? (long)st.st_size : -1;
    printf("FILE_B64_BEGIN %s %ld\n", name, size);
    unsigned char raw[57], line[80];
    size_t n, olen, lines = 0;
    while ((n = fread(raw, 1, sizeof(raw), f)) > 0) {
        mbedtls_base64_encode(line, sizeof(line), &olen, raw, n);
        line[olen] = 0;
        printf("F:%s\n", line);
        if (++lines % 64 == 0) vTaskDelay(1);
    }
    fclose(f);
    printf("FILE_B64_END\n");
}

/* Card write throughput, three ways (see docs: what the recorder should use). */
static void cmd_sdbench(void)
{
    const size_t chunk = 64 * 1024, total = 4 * 1024 * 1024;
    const char *path = CAPTURE_DIR "/BENCH.BIN";
    size_t align = 128;
    esp_cache_get_alignment(MALLOC_CAP_SPIRAM, &align);
    uint8_t *ext = heap_caps_aligned_alloc(align, chunk, MALLOC_CAP_SPIRAM);
    uint8_t *in = heap_caps_aligned_alloc(64, chunk / 2, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!ext || !in) { printf("sdbench: no memory\n"); goto out; }
    memset(ext, 0xA5, chunk);
    memset(in, 0x5A, chunk / 2);

    for (int variant = 0; variant < 3; variant++) {
        int64_t t0 = esp_timer_get_time();
        size_t done = 0;
        if (variant < 2) {
            const uint8_t *buf = variant == 0 ? ext : in;
            size_t n = variant == 0 ? chunk : chunk / 2;
            int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd < 0) { printf("sdbench: open failed\n"); break; }
            while (done < total && write(fd, buf, n) == (ssize_t)n) done += n;
            close(fd);
        } else {
            FILE *f = fopen(path, "wb");
            if (!f) { printf("sdbench: fopen failed\n"); break; }
            setvbuf(f, NULL, _IOFBF, 32 * 1024);
            while (done < total && fwrite(ext + 1, 1, 60000, f) == 60000) done += 60000;
            fclose(f);
        }
        int64_t us = esp_timer_get_time() - t0;
        static const char *const names[] = { "write() 64K, aligned PSRAM buffer", "write() 32K, internal DMA buffer",
                                             "fwrite() 60K pieces, 32K stdio buffer" };
        printf("sdbench: %-40s %u KB in %lld ms = %lld KB/s\n", names[variant], (unsigned)(done / 1024),
               (long long)(us / 1000), us ? (long long)done * 1000000 / 1024 / us : 0);
    }
    unlink(path);
out:
    heap_caps_free(ext);
    heap_caps_free(in);
}

static void cmd_reg(char *args)
{
    char *end;
    unsigned long reg = strtoul(args, &end, 16);
    while (*end == ' ') end++;
    if (*end) {
        unsigned long val = strtoul(end, NULL, 16);
        esp_err_t r = ov5647_ctl_write((uint16_t)reg, (uint8_t)val);
        printf("reg %04lx <- %02lx: %s\n", reg, val, esp_err_to_name(r));
    } else {
        uint8_t val = 0;
        esp_err_t r = ov5647_ctl_read((uint16_t)reg, &val);
        printf("reg %04lx = %02x (%s)\n", reg, val, esp_err_to_name(r));
    }
}

static void handle(char *line)
{
    char *arg = strchr(line, ' ');
    if (arg) { *arg++ = 0; while (*arg == ' ') arg++; } else arg = "";

    if (!strcmp(line, "photo")) {
        if (s_photo) s_photo(s_user);
    } else if (!strcmp(line, "video")) {
        if (s_video) s_video(s_user);
    } else if (!strcmp(line, "dump")) {
        esp_err_t r = capture_trigger_dump();
        if (r != ESP_OK) printf("dump: %s\n", esp_err_to_name(r));
    } else if (!strcmp(line, "stilldump")) {
        esp_err_t r = cam_ctrl_take_still(true, 1);
        if (r != ESP_OK) printf("stilldump: %s\n", esp_err_to_name(r));
    } else if (!strcmp(line, "uidump")) {
        esp_err_t r = capture_trigger_screenshot();
        if (r != ESP_OK) printf("uidump: %s\n", esp_err_to_name(r));
    } else if (!strcmp(line, "preset")) {
        /* preset list | save N | load N | clear N | panel 0/1   (N = 1..PRESET_SLOTS) */
        char what[8] = "";
        int n = 0;
        sscanf(arg, "%7s %d", what, &n);
        if (!strcmp(what, "list")) {
            for (int i = 0; i < PRESET_SLOTS; i++) {
                char text[96];
                presets_describe(i, text, sizeof(text));
                printf("preset %d: %s\n", i + 1, text);
            }
        } else if (!strcmp(what, "panel")) {
            ui_live_show_panel(0, n != 0);
        } else if (n < 1 || n > PRESET_SLOTS) {
            printf("usage: preset list | save N | load N | clear N | panel 0/1\n");
        } else if (!strcmp(what, "save") || !strcmp(what, "load")) {
            printf("preset %s %d: %s\n", what, n, ui_live_preset(n - 1, what[0] == 's') ? "ok" : "failed");
        } else if (!strcmp(what, "clear")) {
            printf("preset clear %d: %s\n", n, esp_err_to_name(presets_clear(n - 1)));
        }
    } else if (!strcmp(line, "bar")) {
        ui_live_set_bar_hidden(atoi(arg) == 0);         /* bar 1 = show, bar 0 = hide */
    } else if (!strcmp(line, "set")) {
        /* set burst <1|3|5|10> | set h264 <0|1> | set hires <0|1>: settings, stored like from the panel */
        char key[12] = "";
        int v = 0;
        settings_t cfg = *settings_get();
        if (sscanf(arg, "%11s %d", key, &v) == 2) {
            if (!strcmp(key, "burst"))      cfg.burst = (uint8_t)(v < 1 ? 1 : (v > 10 ? 10 : v));
            else if (!strcmp(key, "h264"))  cfg.video_h264 = v != 0;
            else if (!strcmp(key, "hires")) cfg.photo_hires = v != 0;
            settings_set(&cfg);
        }
        printf("settings: burst %u, h264 %d, hires %d\n", cfg.burst, cfg.video_h264, cfg.photo_hires);
    } else if (!strcmp(line, "edit")) {
        /* edit [fx id] | edit close: the parameter editor */
        bool open = strcmp(arg, "close") != 0;
        printf("edit %s: %s\n", arg, ui_live_editor(open ? arg : "", open) ? "ok" : "no active effect");
    } else if (!strcmp(line, "param")) {
        /* param <fx id> <param id> <value> */
        char fx_id[16] = "", param_id[16] = "";
        float v = 0;
        if (sscanf(arg, "%15s %15s %f", fx_id, param_id, &v) == 3) {
            printf("param %s.%s = %.3f: %s\n", fx_id, param_id, (double)v,
                   ui_live_set_param(fx_id, param_id, v) ? "ok" : "unknown, or the effect is not active");
        } else {
            printf("usage: param <fx id> <param id> <value>\n");
        }
    } else if (!strcmp(line, "gallery")) {
        if (!strcmp(arg, "open"))        gallery_open();
        else if (!strcmp(arg, "close"))  gallery_close();
        else if (!strcmp(arg, "next"))   gallery_next();
        else if (!strcmp(arg, "prev"))   gallery_prev();
        else if (!strcmp(arg, "play"))   gallery_play();
        else if (!strcmp(arg, "look"))   gallery_use_look();
        else if (!strcmp(arg, "delete")) gallery_delete();
        else printf("usage: gallery open|close|next|prev|play|look|delete\n");
    } else if (!strcmp(line, "settings")) {
        ui_live_show_panel(1, atoi(arg) != 0);
    } else if (!strcmp(line, "knob")) {
        /* what the header controls do: knob <steps> | knob click | knob reroll */
        if (!strcmp(arg, "click"))       ui_live_next_preset();
        else if (!strcmp(arg, "reroll")) ui_live_reroll();
        else                             ui_live_adjust_amount(atoi(arg));
    } else if (!strcmp(line, "idle")) {
        if (!strcmp(arg, "poke")) ui_lvgl_poke();
        printf("idle: %lu ms, backlight %s\n", (unsigned long)ui_lvgl_idle_ms(), ui_lvgl_dimmed() ? "dimmed" : "full");
    } else if (!strcmp(line, "recipe")) {
        fp_recipe_t r;
        frame_pipeline_get_recipe(&r);
        printf("recipe: amount %.2f seed %08lx", (double)r.amount, (unsigned long)r.seed);
        for (int i = 0; i < r.chain.count; i++) {
            const fx_slot_t *s = &r.chain.slots[i];
            if (!s->fx || !s->enabled) continue;
            printf(" | %s", s->fx->id);
            for (int k = 0; k < s->fx->n_params; k++) printf(" %.2f", (double)s->params[k]);
        }
        printf("\n");
    } else if (!strcmp(line, "zoom")) {
        if (*arg) ui_live_set_zoom((float)atof(arg));
        printf("zoom %.2f (%s)\n", (double)cam_ctrl_get_zoom(), cam_ctrl_mode_name());
    } else if (!strcmp(line, "tele")) {
        unsigned x = 0, y = 0;
        if (sscanf(arg, "%u %u", &x, &y) == 2) cam_ctrl_set_tele_origin((uint16_t)x, (uint16_t)y);
        else printf("usage: tele <x0> <y0>\n");
    } else if (!strcmp(line, "fx")) {
        printf("fx %s: %s\n", arg, ui_live_toggle_effect(arg) ? "toggled" : "unknown effect or chain full");
    } else if (!strcmp(line, "amount")) {
        ui_live_set_amount((float)atof(arg));
    } else if (!strcmp(line, "ae")) {
        ae_state_t ae;
        auto_exposure_get(&ae);
        printf("ae: frame %lu luma %u rgb %u %u %u expo %lu gain %lu/16\n", (unsigned long)ae.frame, ae.measured_luma,
               ae.mean_r, ae.mean_g, ae.mean_b, (unsigned long)ae.exposure_lines, (unsigned long)ae.gain_x16);
    } else if (!strcmp(line, "eject")) {
        capture_sd_eject();                     /* unmount; the next photo mounts the card again */
    } else if (!strcmp(line, "ls")) {
        cmd_ls();
    } else if (!strcmp(line, "get")) {
        cmd_get(arg);
    } else if (!strcmp(line, "reg")) {
        cmd_reg(arg);
    } else if (!strcmp(line, "sdbench")) {
        cmd_sdbench();
    } else if (!strcmp(line, "help")) {
        printf("commands: photo | video | dump | stilldump | uidump | zoom [1..6] | fx <id> | amount <0..1> | bar 0/1 | edit [fx]|close | param <fx> <id> <value> | knob <steps>|click|reroll | idle [poke] | recipe | "
               "preset list|save N|load N|clear N|panel 0/1 | settings 0/1 | gallery open|close|next|prev|play|look|delete | ae | eject | ls | get <file> | reg <hex> [hex] | tele <x0> <y0> | sdbench\n");
    } else {
        printf("unknown command '%s' (try help)\n", line);
    }
}

static void remote_task(void *arg)
{
    (void)arg;
    char line[64];
    size_t n = 0;
    for (;;) {
        uint8_t c;
        if (uart_read_bytes(REMOTE_UART, &c, 1, portMAX_DELAY) != 1) continue;
        if (c == '\r' || c == '\n') {
            line[n] = 0;
            if (n) handle(line);
            n = 0;
        } else if (c >= 0x20 && c < 0x7f && n < sizeof(line) - 1) {
            line[n++] = (char)c;
        }
    }
}

esp_err_t remote_init(remote_action_t photo, remote_action_t video, void *user)
{
    s_photo = photo;
    s_video = video;
    s_user = user;
    /* RX only: log output keeps using the console's own direct writes. */
    if (!uart_is_driver_installed(REMOTE_UART)) {
        ESP_RETURN_ON_ERROR(uart_driver_install(REMOTE_UART, 512, 0, 0, NULL, 0), TAG, "uart driver");
    }
    BaseType_t ok = xTaskCreatePinnedToCore(remote_task, "remote", 6 * 1024, NULL, 2, NULL, 0);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_FAIL, TAG, "task");
    ESP_LOGI(TAG, "serial remote ready (type help)");
    return ESP_OK;
}
