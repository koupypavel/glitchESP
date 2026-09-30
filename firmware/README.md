# glitchESP firmware

ESP-IDF v5.5.5 project for the Waveshare ESP32-P4-WIFI6-Touch-LCD-5 with an OV5647 camera.

## Build and flash

```powershell
.\build.ps1              # build (rev1_3 silicon profile)
.\build.ps1 COM10        # build + flash over the USB-UART port
.\build.ps1 COM10 rev3_x # other silicon profile (untested)
```

`esptool` refuses a `rev3_x` image on a rev v1.x chip and the other way round, so pick the
profile that matches your board (`esptool.py chip_id` prints the revision).

## Layout

| Path | Role |
|---|---|
| `main/app_main.c` | Boot order: NVS → display → LVGL → SD → pipeline → capture → UI → camera → buttons |
| `main/camera/app_video.*` | V4L2 wrapper for the MIPI-CSI camera (from the Espressif/Waveshare example, modified) |
| `main/camera/cam_ctrl.*` | Sensor modes (wide binned / 1:1) and zoom; restarts the stream when the mode changes |
| `main/camera/ov5647_ctl.*` | Direct sensor register access: exposure, gain, frame timing |
| `main/pipeline/frame_pipeline.*` | Per camera frame: pick the view, scale, run the effect chain, stamp the UI, show it |
| `main/pipeline/auto_exposure.*` | Software auto exposure (the sensor's own does not adapt in this setup) |
| `main/pipeline/fx_parallel.*` | Splits row-parallel work across both CPU cores |
| `main/effects/` | The effect engine and the effects (plain C, also built on the PC by `tools/fxlab`) |
| `main/display/` | The panel's three frame buffers: acquire, submit, hold for the video encoder |
| `main/ui/` | LVGL widgets drawn into a separate layer that is stamped onto each frame; the gallery |
| `main/storage/` | Stills (hardware JPEG + `.json` recipe), Motion-JPEG AVI, fast SD writer |
| `main/input/` | BOOT button (GPIO35): short press = photo, hold 0.7 s = start/stop video |
| `main/system/` | Settings and presets in NVS, serial remote |
| `main/bench.c` | On-device benchmark, enabled with `GLITCH_BENCH` in `app_main.c` |

## How a frame gets to the screen

1. The camera driver delivers an RGB565 frame (640×960 in the wide mode, 800×1280 in the
   1:1 mode) into one of five buffers in PSRAM.
2. `frame_pipeline` works out the centered rectangle to show for the current zoom.
3. With no effect active it is copied or scaled straight into a free panel frame buffer.
   With effects, the chain runs either at full resolution or at 360×640 and is then
   pixel-doubled (chosen from the effects' cost, or forced in the settings).
4. The UI layer's non-transparent pixels are stamped on top, and the frame buffer is handed
   to the panel (no copy; the panel just switches buffers).
5. A photo copies the finished frame (before the UI stamp) to the JPEG encoder; video
   encodes straight from the frame buffer that was just shown.

The camera task, and with it all per-frame work, runs on core 1; a worker on core 0 takes
the upper half of the rows for anything that can be split. LVGL, capture, SD writes and
buttons live on core 0.

## Zoom and sensor modes

The screen is 9:16, the sensor 4:3, so the picture is always a slice of the sensor.

| Zoom | Sensor mode | What is shown |
|---|---|---|
| 1.0× | wide: 2×2 binning, 640×960 frame | 540×960 binned pixels (1080×1920 on the sensor), enlarged 1.33× |
| 1.5× | 1:1: no binning, 800×1280 frame | 720×1280 sensor pixels, one per screen pixel |
| 2× to 6× | 1:1 | a smaller rectangle, enlarged by the bilinear scaler |
| photo at 1.0× | still: no binning, 1088×1920 frame | the wide view with every sensor pixel |

The wide mode sees more, gathers about twice the light per pixel and is much less noisy;
the 1:1 mode resolves more detail in a narrower view. Switching between them restarts the
camera stream (about 50 ms). All modes are built at run time from the driver's 800×1280
register table plus a few overrides (window, binning, frame timing), see `cam_ctrl.c`.

## Photos

Zoomed in, a photo is the frame on screen (720×1280), rendered at full resolution for that
one frame even if the preview runs the effects at half resolution.

At zoom 1.0 the preview only has binned pixels, so the shutter does more (it can be turned
off in the settings): the preview stops, the sensor is switched to the 1088×1920 mode, the
seventh frame is kept, the effect chain runs on it with the seed and frame number of the
last preview frame, the JPEG is written, and the preview resumes. This takes about 0.7 s
without effects and 1 to 1.5 s with a heavy chain. The five preview buffers are one 10 MB
block of PSRAM that doubles as the two 4.2 MB buffers this needs. Exposure time, gain
(doubled, because binning collects twice the light) and white balance are carried over from
the preview so the photo matches it. Effects that need the previous frame (tracers) have no
history at that size, so with those the on-screen frame is saved instead.

## Presets

The list button opens eight slots. The disk button on a row stores the current look (effects
with their parameters, the amount and the seed); tapping the row recalls it. Presets live in
NVS as small blobs that name effects by id, so they survive firmware updates that add or
reorder effects (`system/presets.c`).

## Gallery

The picture button opens the gallery (`ui/gallery.c`). It stops the camera and borrows its
10 MB buffer block: the hardware JPEG decoder decodes into it (anything up to 1088x1920),
the result is scaled to the screen once and then redrawn ten times a second so the buttons
stay live. Videos are read frame by frame from the AVI and play at their recorded rate.
The info line shows the recipe from the `.json` sidecar; "Use look" makes that recipe the
current effect setup and returns to the camera. Delete needs two taps. Closing the gallery
(or pressing BOOT) reprograms the sensor and restarts the preview.

## Video

Hold BOOT for 0.7 s to start recording, hold again to stop. Frames are the same 720×1280
frames you see (effects burned in), JPEG quality 80, written as a Motion-JPEG AVI that any
player opens. About 12 fps without effects (the JPEG encoder is the limit), 1 MB/s on the
card. While recording, the control bar is not drawn, so the video stays clean. The frame
rate in the AVI header is measured at stop, and a `.json` sidecar records the recipe.

## Serial remote

The console UART (115200 baud) accepts text commands, so the camera can be driven and
checked from a PC:

```
photo | video | dump | stilldump | uidump | zoom [1..6] | fx <id> | amount <0..1>
recipe | preset list|save N|load N|clear N|panel 0/1 | settings 0/1 | ae
gallery open|close|next|prev|play|look|delete
ls | get <file> | reg <hex> [hex] | tele <x0> <y0> | sdbench | help
```

`serial_capture.py` resets the board, logs for a while and can send commands at given
times; `decode_jpeg_dump.py` and `decode_file_dump.py` turn a logged `dump` or `get` back
into files (run them with the ESP-IDF python environment):

```powershell
python serial_capture.py COM10 30 --out run.log 5:dump 20:"zoom 1.5" 22:photo 25:ls
python decode_jpeg_dump.py run.log frame.jpg
```

## Notes on the hardware

- **SD card speed** depends on how the data is handed over. Writing 4 MB: `fwrite` through
  stdio 1.8 MB/s, `write()` from a cache-aligned PSRAM buffer 2.8 MB/s, `write()` of 32 KB
  from an on-chip DMA buffer 5.2 MB/s. `storage/sd_writer.c` does the last.
- **Sensor timing** must be programmed before the stream starts; changing the frame length
  while streaming stalls the sensor. Exposure and gain, on the other hand, only take effect
  when written while streaming.
- **The sensor takes its output from the middle of the readout window.** The stock 800×1280
  mode reads a 2110-pixel-wide window that is not centered on the sensor, so its picture is
  off-center; `cam_ctrl.c` uses a window just larger than the output, centered.
- **The hardware ISP accepts at most 1920 pixels per line**, so the full 2592-wide sensor
  frame cannot go through it. 1088×1920 is the largest frame with the screen's shape.
- **The sensor's auto white balance starts from neutral after every mode change** and takes
  about a second to settle; until then the picture is green. Its settled gains can be read
  back (0x5190..0x5195) and applied as manual gains (0x5186..0x518B, enable bit 3 of
  0x5180). `auto_exposure.c` does that for the first 40 frames of a new mode, and the auto
  white balance keeps converging underneath, so handing back to it is seamless.
- More measurements (memory bus limits, per-effect timings) are in `../docs/EFFECTS.md`.
