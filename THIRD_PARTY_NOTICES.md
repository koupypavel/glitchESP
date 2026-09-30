# Third-party notices

glitchESP's own code is released under the MIT License (see `LICENSE`). The repository also
contains, or builds against, material from other parties. Their terms apply to those parts.

## Files in this repository

| Path | Origin | License |
|---|---|---|
| `firmware/main/camera/app_video.c`, `app_video.h` | Espressif Systems, from the `video_lcd_display` example shipped in Waveshare's example repository; modified for glitchESP (manual buffer release, flip control) | CC0-1.0 (see file headers) |
| `m0/09_video_lcd_display/` | Waveshare `ESP32-P4-WIFI6-Touch-LCD-5` examples, originally by Espressif Systems; unmodified copy used for hardware bring-up | CC0-1.0 (file headers), repository Apache-2.0 |
| `m0/08_lvgl_demo_v9/` | Waveshare `ESP32-P4-WIFI6-Touch-LCD-5` examples; one line changed to run the widgets demo | Apache-2.0 (`components/bsp_extra/LICENSE`) |
| `doc/ESP32-P4-WIFI6-Touch-LCD-5-Schematic.pdf` | Waveshare, from https://github.com/waveshareteam/ESP32-P4-WIFI6-Touch-LCD-5 | Apache-2.0 (repository license) |

## Components downloaded at build time (not stored here)

The ESP-IDF Component Manager fetches these into `firmware/managed_components/`:

| Component | License |
|---|---|
| ESP-IDF (Espressif Systems) | Apache-2.0 |
| `espressif/esp_video`, `esp_cam_sensor`, `esp_ipa`, `esp_h264`, `button`, and related Espressif components | Apache-2.0 (some parts are distributed as binary libraries under Espressif's terms; see each component) |
| `waveshare/esp32_p4_wifi6_touch_lcd_5`, `waveshare/esp_lcd_hx8394` | Apache-2.0 |
| `lvgl/lvgl` | MIT |

## Ideas and references

No code was taken from these, but they shaped the project:

- Kim Asendorf's ASDFPixelSort (the pixel-sorting idea).
- The Raspberry Pi glitch camera by sharkbiscuit101 (control scheme: knob for amount, shutter burns the effect in).
- PsychonautWiki "Visual effects" and Wikipedia "Psychedelic replication" (names and descriptions of tracers, drifting, breathing, diffraction, colour shifting, symmetrical texture repetition).
- The Linux kernel `ov5647` driver (register semantics for manual exposure and gain, consulted as documentation).

## Trademarks

ESP32, ESP-IDF and Espressif are trademarks of Espressif Systems. Waveshare is a trademark of
Waveshare Electronics. This project is not affiliated with or endorsed by either.
