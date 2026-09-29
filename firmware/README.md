# glitchESP firmware

ESP-IDF v5.5.5 project for the Waveshare ESP32-P4-WIFI6-Touch-LCD-5. See `../PLAN.md` for
the roadmap; this folder is the code.

## Build and flash

```powershell
.\build.ps1              # build (rev1_3 silicon profile, matches this board)
.\build.ps1 COM10        # build + flash over the USB-UART port
python serial_capture.py COM10 10    # reset and capture 10 s of log (IDF python env)
```

The chip on this board is ESP32-P4 **rev v1.3**; `rev3_x` images are refused by esptool.

## Layout

| Path | Role |
|---|---|
| `main/app_main.c` | Boot order: NVS → display/LVGL → SD → pipeline → capture → UI → camera → buttons |
| `main/camera/` | V4L2 wrapper for the MIPI-CSI camera (from Waveshare example 09, CC0) |
| `main/pipeline/` | Camera frame → PPA center-crop 800×1280 → 720×1280 RGB565 ring (3 buffers); capture snapshots |
| `main/storage/` | Capture task: stills (hardware JPEG → `/sdcard/GLITCH/IMG_nnnn.jpg` + `.json` recipe) and video (Motion-JPEG AVI → `VID_nnnn.avi` + `.json`, encoded straight from the panel frame buffer); counters in NVS |
| `main/ui/` | LVGL live view: full-screen canvas fed from the ring, status bar, flash, toast |
| `main/input/` | BOOT button (GPIO35 for now): short press = photo, hold 0.7 s = start/stop video |
| `main/system/` | Persistent settings (mirror/flip, preview quality, JPEG quality) in NVS |
| `main/display/` | Direct DPI frame-buffer path with hold/release for the video encoder |

## M1 status

- [x] Project builds against the Component Registry (esp_video, LVGL 9.5, Waveshare BSP, button)
- [ ] Live preview through LVGL canvas, fps in the status bar
- [ ] BOOT shutter → JPEG + sidecar on SD, opens on a PC
- [ ] Orientation check (mirror/flip)

## Video

Hold BOOT for 0.7 s to start recording, hold again to stop. Frames are the same 720×1280
frames you see (effects burned in), JPEG quality 80, written as a Motion-JPEG AVI that any
player opens. Expect roughly 15 fps and 1.5 to 2.5 MB/s on the card, so a fast card matters.
While recording, the control bar is not drawn, so the video stays clean. The frame rate in the
AVI header is measured at stop, and a `.json` sidecar records the effect recipe.

## Design notes

- **Compositing:** LVGL owns the panel frame buffers (triple full-frame mode). The video is an
  `lv_canvas` whose buffer pointer is swapped to the newest ring entry every LVGL tick, so the
  UI draws on top for free. The adapter's "dummy draw" mode was rejected because it discards
  LVGL's own output.
- **Threads:** camera + PPA + snapshot copy run on core 1 in the esp_video stream task; LVGL,
  capture/JPEG/SD and buttons run on core 0.
- **Cache:** ring buffers are cache-line aligned in PSRAM; the snapshot is written back
  (`esp_cache_msync` C2M) before the JPEG DMA engine reads it.
