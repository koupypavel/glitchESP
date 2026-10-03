# glitchESP firmware

ESP-IDF v5.5.5 project for the Waveshare ESP32-P4-WIFI6-Touch-LCD-5 with an OV5647 camera.

## Build and flash

```powershell
.\build.ps1              # build (rev1_3 silicon profile)
.\build.ps1 COM10        # build + flash over the USB-UART port
.\build.ps1 COM10 rev3_x # other silicon profile (untested)
```

On Linux and macOS, with the ESP-IDF environment active, `./build.sh [port] [profile]` does
the same and also writes `build/<profile>/glitchesp-merged.bin`, the single image attached
to releases. The GitHub workflow uses that script.

`esptool` refuses a `rev3_x` image on a rev v1.x chip and the other way round, so pick the
profile that matches your board (`esptool.py chip_id` prints the revision).

The firmware version comes from `version.txt` and is shown in the settings panel; component
versions are pinned in `dependencies.lock`. To make a release: add a section to
`../CHANGELOG.md`, set `version.txt`, and push a tag `vX.Y.Z`; the workflow builds the images
and publishes them with that section as the release notes.

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
| `main/storage/` | Stills (hardware JPEG + `.json` recipe), Motion-JPEG AVI and H.264 MP4 video, fast SD writer |
| `main/input/` | BOOT button, plus optional buttons and a rotary encoder on the header (pins in `buttons.h`) |
| `main/system/` | Settings and presets in NVS, sounds, serial remote |
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

**Burst** (settings: off, 3, 5 or 10 photos) repeats the shot with a new random seed each
time and puts the original seed back afterwards. Full-resolution bursts stay in the still
mode between shots, so each further photo takes about a third of a second; zoomed in, the
shots are taken from the preview one after another.

## Editing parameters

The pencil button opens a sheet over the control bar (`ui/ui_editor.c`) with one tab per
active effect and a slider or switch per parameter, built from the effect's parameter
table. Changes apply to the running preview at once. They stay when other effects are
switched on or off, and they are part of the recipe, so presets, photo sidecars and "Use
look" carry them. Moving the amount slider maps every parameter from the one knob again,
and "Reset" does that for the effect being edited.

## Presets

The list button opens eight slots. The disk button on a row stores the current look (effects
with their parameters, the amount and the seed); tapping the row recalls it. Presets live in
NVS as small blobs that name effects by id, so they survive firmware updates that add or
reorder effects (`system/presets.c`).

## Gallery

The picture button opens the gallery (`ui/gallery.c`). It stops the camera and borrows its
10 MB buffer block: the hardware JPEG decoder decodes into it (anything up to 1088x1920),
the result is scaled to the screen once and then redrawn ten times a second so the buttons
stay live. Videos are read frame by frame from the AVI and play at their recorded rate;
H.264 recordings are listed but cannot be played (the chip has no H.264 decoder).
The info line shows the recipe from the `.json` sidecar; "Use look" makes that recipe the
current effect setup and returns to the camera. Delete needs two taps. Closing the gallery
(or pressing BOOT) reprograms the sensor and restarts the preview.

## Video

Hold BOOT for 0.7 s to start recording, hold again to stop. Frames are the same 720×1280
frames you see (effects burned in). While recording, the control bar is not drawn, so the
video stays clean. The frame rate written into the file is measured at stop, and a `.json`
sidecar records the recipe. The format is chosen in the settings:

| | Motion-JPEG (default) | H.264 |
|---|---|---|
| File | `VID_nnnn.avi` | `VID_nnnn.mp4` |
| Frame rate | every preview frame: 20 fps plain, about 14 fps with effects | every second frame: 9 fps plain, about 7 fps with effects |
| Size | about 2 MB/s (JPEG quality 80) | about 0.3 MB/s (3 Mbit/s target, a key frame every 24 frames) |
| Plays in the gallery | yes | no |

H.264 is slower although the encoder itself needs under 30 ms per frame: it only accepts
YUV 4:2:0, and converting the RGB565 frame takes the pixel-processing accelerator (PPA)
another 60 to 70 ms. Doing that conversion on the CPU cost the preview more than it gained.
`storage/mp4_writer.c` writes the MP4 itself: the frames go into one `mdat` box as they
arrive and the index (`moov`) follows when recording stops, so a recording that is cut off
by a power loss has no index and will not play. The encoder needs a 52 KB block of on-chip
RAM, which is only reliably available at start-up, so it is created then and kept.

## Serial remote

The console UART (115200 baud) accepts text commands, so the camera can be driven and
checked from a PC:

```
photo | video | dump | stilldump | uidump | zoom [1..6] | fx <id> | amount <0..1>
bar 0/1 | knob <steps>|click|reroll | idle [poke] | eject | edit [fx]|close | param <fx> <param> <value>
recipe | preset list|save N|load N|clear N|panel 0/1 | settings 0/1 | set burst|h264|hires|quality <n> | ae
night auto|0|1|2 | bat
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

## Low light

The wide mode's frame time is a register away from being longer, and a longer frame allows
a longer exposure. `cam_ctrl.c` checks once a second: if exposure and gain are both at
their limit and the picture is still more than 20 % under target for three seconds, the
frame gets twice as long (10 fps, 100 ms exposure), then three times (6.7 fps, 150 ms, and
the gain limit goes from 16x to 24x). It steps back when the level below could deliver the
same brightness with half its range to spare. Auto exposure prefers exposure over gain, so
a longer frame is used to bring the gain down first.

At a night level every camera frame is averaged with the result before it
(`frame_pipeline.c`, two pixels per word), which takes the noise of the high gain down and
leaves a short trail behind moving things. A photo averages six frames exactly (running
mean with random rounding, because a 5-bit channel would otherwise lose every change
smaller than half a step). Full-resolution stills are not used at night: the binned mode
collects twice the light per pixel. The zoomed-in mode has no night levels.

Settings: "Low light" Auto, Off, Always (the longest frames). The status line shows
"night" or "night 2". Serial: `night auto|0|1|2`.

Not measured outside at night yet: the levels were forced and checked indoors.

## Battery

`system/battery.c` reads GPIO20 every two seconds: the board divides the cell voltage by
three (measured: 1.396 V at the pin for a cell at 4.19 V). The reading is smoothed and
mapped to a percentage with a typical LiPo discharge curve. The board has no signal for
"USB connected" or "charging", so charging is inferred: the voltage rising over a minute,
or held above 4.23 V. A full cell on USB therefore shows 100 % without the "+".
At 10 % a warning appears once; under 3.4 V a running recording is stopped so that its
file gets closed properly. When charging is first detected a toast reminds the user not to
leave the pack unattended. Serial: `bat`.

Charging is done by the board's charger (ETA6098, fed from the USB 5 V rail): plugging a
USB-C cable into the USB-UART port charges the cells on the BAT connector, with the camera
on or off. The charge current comes from the board's ISET resistor and has not been read
off the schematic; two LP653454 cells in parallel took a few hours from mostly empty.

## Small things

- **Idle dimming:** after a minute without touch or buttons the backlight goes to 15 %;
  any input brings it back. Recording counts as input. There is a switch in the settings.
- **The SD card can be put in at any time.** A card that is missing at boot is looked for
  every five seconds, and again whenever a photo, a recording or the gallery needs it. A
  failed write unmounts the card so that it can be mounted again.
- **Starter presets** go into empty slots 1 to 4 on the first boot only.
- **Sounds** (`system/sound.c`): a shutter click and two recording beeps, synthesized and
  played through the ES8311 codec. The amplifier is switched on for the first sound (0.1 to
  0.3 s, so the first click of a session is a little late) and off again after 30 s of
  silence. There is a switch in the settings.

## Notes on the hardware

- **The board library's SD mount cannot be called twice.** It creates a new power-control
  handle for the card supply on every call and never releases it, so after one failed
  mount (no card at boot) every later one fails with "Failed to create a new on-chip LDO
  power control driver". `storage/sd_card.c` mounts the card itself and keeps the handle.
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
