# glitchESP — Glitch Camera on the Waveshare ESP32-P4-WIFI6-Touch-LCD-5

Implementation plan, draft for review. Written 2026-09-29.

> **Historical document.** This is the plan the project started from. Milestones M0–M2 and
> parts of M3 are done and several decisions changed along the way (the preview does not use
> the PPA, effects run on both cores, video is Motion-JPEG). For the current state see
> `README.md`, `firmware/README.md` and `docs/EFFECTS.md`.

Everything in section 2 was checked against the schematic PDF, the Waveshare wiki, the
unzipped example repo in `doc/examples_unzipped/`, and the ESP32-P4 datasheet in `doc/`.
Section 3 is web research. Sections 4 onward are proposals: please push back on anything.

---

## 1. Goal

A handheld glitch camera. The 5" portrait display shows a live, already-glitched preview.
A physical shutter button burns the effect into the shot and saves a JPEG to microSD.
Effects and their parameters are chosen on the touch screen and, optionally, with a rotary
knob for the "how broken" amount. Shots are reproducible because every JPEG gets a sidecar
"recipe" file with the effect chain, parameters and random seed.

Non-goals for v1: video recording, Wi-Fi upload, hi-res (5 MP) stills. These are listed as
stretch phases at the end.

---

## 2. What the hardware gives us

### 2.1 Board facts

| Block | Part / interface | Notes for this project |
|---|---|---|
| SoC | ESP32-P4NRW32, dual-core RISC-V 360 MHz + LP core | 768 KB SRAM, 32 MB in-package PSRAM, 32 MB NOR flash. **Silicon rev v1.3** (PSRAM at 200 MHz) |
| Display | 5" IPS 720×1280 portrait, 2-lane MIPI-DSI, HX8394 | Backlight PWM GPIO26, reset GPIO27, triple frame buffer in PSRAM |
| Touch | GT911 capacitive, 5 points, I2C 0x5D/0x14 | TP_RST GPIO23, TP_INT GPIO2 (only if R108 populated) |
| Camera | 15-pin 0.5 mm MIPI-CSI, 2-lane, OV5647 (sold separately) | SCCB shares board I2C (SDA GPIO7, SCL GPIO8) |
| Storage | microSD, SDMMC 4-bit | D0–D3 GPIO39–42, CMD GPIO44, CLK GPIO43 |
| Audio | ES8311 codec + ES7210 mic ADC, speaker connector 8 Ω 2 W | I2S MCLK13 BCLK12 LRCK10 DOUT9 DIN11, PA enable GPIO53 |
| Wireless | ESP32-C6-MINI-1 over SDIO (esp_hosted) | SDIO on GPIO14–19, C6 reset GPIO54, wake GPIO6 |
| USB | Type-C USB-UART (programming) + Type-C USB 2.0 HS OTG | OTG on dedicated pins, usable for USB mass storage later |
| Power | ETA6098 charger, MX1.25 LiPo connector, RTC battery header | Battery voltage on GPIO20 (BAT_ADC), PWR LED |
| Buttons | RESET and BOOT on board | BOOT is GPIO35 (strapping pin), usable as an interim shutter |
| Header | 40-pin, RPi-HAT-like layout | See 2.3 for which pins are free |

### 2.2 Hardware accelerators we will lean on

| Unit | What it does | Where it helps |
|---|---|---|
| ISP | Bayer denoise, demosaic, 3×3 color matrix, gamma LUT, brightness/contrast/saturation/hue, sharpen, AE/AWB | Free "in-sensor" color glitches by abusing the CCM, gamma and hue controls |
| PPA | Scale (1/16 steps), rotate 0/90/180/270, mirror, alpha blend, fill; RGB565/RGB888/ARGB8888/YUV | Crop 800→720, half-res preview, feedback zoom, kaleidoscope, UI compositing |
| JPEG codec | HW encode RGB565/888 → JPEG, decode JPEG → RGB565/888, quality 1–100 | Saving shots; JPEG databending (encode → corrupt → decode) |
| H.264 encoder | HW, via V4L2 `/dev/video11` | Datamosh (stretch) |

Constraint: the JPEG engine is either encoding or decoding at one time, never both.

### 2.3 GPIO budget on the 40-pin header

Datasheet restrictions: GPIO34–38 are strapping pins (37/38 are also UART0 console),
GPIO24/25 are USB-Serial/JTAG, GPIO2–5 are JTAG. Avoid those for our controls.

Free and safe for buttons/encoder: **GPIO21, 22, 28, 29, 30, 31, 32, 46, 47, 48, 49, 50, 51, 52**.

### 2.4 OV5647 modes available in esp_video

| Mode | Use |
|---|---|
| RAW8 800×1280 @ 50 fps | Default in Waveshare example 09. Portrait, matches the display. **Preview and capture mode for v1.** |
| RAW8 800×800 / 800×640 @ 50 fps | Lower-cost preview if needed |
| RAW10 1280×960 binned @ 45 fps | Landscape alternative |
| RAW10 1920×1080 @ 30 fps | Hi-res still option (stretch); mode switch costs a re-init |

### 2.5 Toolchains on this PC

| Found | Status |
|---|---|
| ESP-IDF v5.1.4 and v5.3.0 (already installed) | Too old. Waveshare examples are CI-built with **v5.5.5** and v6.0.2 and need `esp_video ~2.0`. |
| VS Code ESP-IDF extension 2.2.0, PlatformIO | Extension can install v5.5.x. PlatformIO has no usable P4 support. |
| CMake, Git, Python 3.11 | OK. Ninja and esptool come with the IDF install. |
| Arduino | Not installed. Not recommended for this board (see D1). |

---

## 3. Research: existing DIY glitch cameras and what to borrow

### 3.1 Hardware glitch cameras

**Raspberry Pi 4 glitch camera by sharkbiscuit101 (Reddit, May 2026).** Covered by Hackaday,
Hackster and Yanko Design. Pi 4B + Arducam module + small HDMI screen + USB battery, in a
clear acrylic shell. A rotary encoder sets how aggressively the script mangles the RGB
channels; an Adafruit Mini I2C Gamepad cycles presets and tweaks levels; a shutter button
saves the frame with the effect burned in. Runs fully offline. Source and parts list were
not released. Borrow: the control model (knob = amount, buttons = presets, shutter =
burn-in) and the WYSIWYG philosophy.

**Circuit-bent kids camera (Adafruit blog, June 2025).** Analog approach: crossing traces on
a cheap camera. Not applicable to our pipeline, but a reminder that "unpredictable but
repeatable" is the feel people like. We get that from seeded randomness.

**ESP32 glitch cameras: none found.** Every ESP32-CAM project (bkeevil/esp32-cam,
rzeldent/esp32cam-ready, hx-esp32-cam-fpv, etc.) is a streaming or recording webcam. There
is no public glitch-camera firmware for any ESP32, so this project fills a gap.

### 3.2 Algorithm references

| Effect | Reference |
|---|---|
| Pixel sorting | Kim Asendorf's ASDFPixelSort (Processing, 2010): sort intervals of a row/column bounded by a luma/hue threshold. satyarth.me and glitchology.com explain the interval variants. |
| JPEG databending | Corrupt bytes between the Start-of-Scan marker (FFDA) and End-of-Image (FFD9) while leaving markers intact, then decode. bitrot-canvas and bendr do this in the browser. |
| Channel shift / RGB split | Offset the R, G, B planes independently; optionally per row band. |
| Scanline / row smear | Random horizontal offsets per row band; "hold" rows to smear. |
| Slit-scan | Build the output one row (or column) per frame over time. scanner-distortion.com. |
| Feedback | Blend previous output back in with zoom/rotate/offset. bendr's feedback loop. |
| Bit-crush + dither | Reduce bits per channel, ordered (Bayer) dither. jkirchartz "glitchy 3-bit dither". |
| Effect taxonomy | Glitch Lab (ilixa) groups 100+ effects into Color, Streak/Repeat/Corruption, Pixel sort, Retro (pixelation/scanlines), 3D, Text, Art filters. Good vocabulary for the UI. |

### 3.3 ESP32-P4 code to reuse

| Source | What we take |
|---|---|
| Waveshare example `09_video_lcd_display` (in `doc/`) | Whole V4L2 camera wrapper (`app_video.c`), PPA crop to LCD, BSP display start, sdkconfig for OV5647 800×1280 |
| Waveshare example `08_lvgl_demo_v9` | LVGL 9 + touch setup via `bsp_display_start_with_config` |
| Waveshare example `05_sdmmc` | SD card mount on this board's pins |
| Waveshare example `06_I2SCodec` / `bsp_extra` | Shutter sound through ES8311 |
| espressif/esp-video-components examples | `image_storage` (JPEG to SD), `m2m` (HW JPEG via `/dev/video10`), `capture_stream` |
| espressif/esp32_p4_eye `display_camera_csi` | Camera frame inside an LVGL canvas |
| ESP-IDF `examples/peripherals/jpeg/jpeg_encode`, `jpeg_decode`, `ppa` | Direct HW codec and PPA usage |

---

## 4. Key decisions (recommendations)

**D1 — Framework: ESP-IDF v5.5.5, C.**
Waveshare says Arduino support on the P4 is limited and recommends IDF. Everything we need
(esp_video V4L2 driver, HW JPEG, PPA, ISP controls, LVGL BSP, SDMMC, esp_hosted) is IDF
first. The Arduino ESP_Video wrapper exists since core 3.3.10 but has no PPA/LVGL BSP for
this board and the sample sketches blit the preview row by row. Pick v5.5.5 over v6.0.2
because it is the version the Waveshare examples target first and the component version
ranges are simpler.

**D2 — Resolution and WYSIWYG capture.**
Sensor runs RAW8 800×1280 @ 50 fps, ISP outputs RGB565. Preview and capture use the same
frame, so what you see is what you get, exactly like the Pi build. A shot is the current
effected frame encoded to JPEG (quality 90). Optionally also save the clean frame. Hi-res
1920×1080 stills are a stretch item because the mode switch costs a camera re-init.

**D3 — Effects are plain C on the CPU, hardware-assisted where it is free.**
Effects operate on RGB565 buffers with no IDF dependencies so they compile on the PC too
(see D7). PPA handles all scale/rotate/mirror/blend steps, the JPEG engine handles the
databend round-trip, and the ISP handles color-matrix/gamma/hue abuse at zero CPU cost.

**D4 — UI with LVGL 9 through the Waveshare BSP.**
The BSP already gives display, touch and LVGL. Open question to spike in M1: how to
composite the LVGL UI over a full-screen video that changes every frame. Two candidates,
in order of preference:
1. Keep example 09's "dummy draw" path for the video and render LVGL into a separate
   ARGB8888 layer, then PPA-blend it over the video frame into the LCD buffer (hardware
   compositing, LVGL only redraws widgets that change).
2. Put the video in an `lv_canvas`/`lv_image` behind the widgets and let LVGL redraw
   (simplest, but LVGL then redraws the full screen every frame).

**D5 — Controls: touch + one shutter button, encoder optional.**
Shutter on GPIO21 with the on-board BOOT button (GPIO35) as an interim shutter so
development can start before any wiring. Rotary encoder for the amount knob via the PCNT
peripheral. A second small button for "re-roll seed". Wiring in section 8.

**D6 — Storage format.**
`/sdcard/GLITCH/IMG_0001.jpg` plus `IMG_0001.json` sidecar with effect chain, parameters,
seed, camera settings and timestamp. Optional `IMG_0001_clean.jpg`. Counter persisted in
NVS. Sidecars make presets, "apply this recipe to the live view" and later PC tooling
trivial.

**D7 — Host-side effect lab.**
`tools/fxlab/` is a tiny C program that loads a PPM/PNG, runs the same `main/effects/*.c`
files and writes the result. Iterating on a glitch algorithm takes seconds on the PC
instead of a 30 s flash cycle, and it doubles as a unit-test harness.

---

## 5. Architecture

```
 OV5647 ──MIPI-CSI──> ISP (HW) ──> V4L2 /dev/video0 ──> cam_buf[2]  RGB565 800×1280 (PSRAM)
                                                              │
                        core 1 ── fx_task ────────────────────┘
                        │  apply effect chain (≤3 fx) + temporal state (prev frame)
                        │  full-res, or half-res via PPA downscale when fx is heavy
                        ▼
                     fx_buf[2] RGB565 800×1280
                        │
                        ├──> PPA SRM crop/scale 800×1280 → 720×1280 → lcd_fb[3] ──> MIPI-DSI
                        │         (+ PPA blend of LVGL UI layer, if D4 option 1)
                        │
   shutter ─────────────┴──> capture: copy fx_buf → JPEG HW encode → SD write + sidecar
                                                 (storage_task, core 0)

 core 0: lvgl_task (UI, touch), input_task (button/encoder, debounce), storage_task,
         battery/adc timer, audio (shutter click)
```

Data flow per frame: DQBUF camera frame → effect chain writes into `fx_buf` → PPA to LCD →
QBUF camera frame back. Capture never blocks the preview: the shutter copies the current
`fx_buf` into a `capture_buf` and hands it to the storage task.

### 5.1 Memory budget (PSRAM, 32 MB available)

| Buffer | Size |
|---|---|
| Camera buffers 2 × 800×1280×2 | 4.1 MB |
| Effect buffers 2 × 800×1280×2 | 4.1 MB |
| Previous frame (temporal fx) | 2.0 MB |
| Capture buffer | 2.0 MB |
| LCD frame buffers 3 × 720×1280×2 | 5.5 MB |
| LVGL layer ARGB8888 720×1280 (option 1) | 3.7 MB |
| JPEG bitstream + decode scratch | 2.5 MB |
| **Total** | **≈ 24 MB** |

Fits. If tight, drop LCD to double buffering or make the LVGL layer RGB565 + A8.

### 5.2 Performance expectations

1 M pixels per frame. A single-pass per-pixel effect in C at ~10 cycles/pixel on one 360 MHz
core is ~30 ms; PSRAM traffic of 4 MB per pass is another 10–20 ms. Expect 15–25 fps for
one light effect at full resolution, dropping with chain length. Heavy effects (pixel sort,
databend) run at half resolution for preview (PPA downscale 2× → 4× fewer pixels) and at
full resolution only on capture. Measure in M2 before optimizing; the P4's PIE SIMD
instructions are a later option.

---

## 6. Effect engine

### 6.1 Interface (pure C, no IDF headers)

```c
typedef struct { uint16_t *px; uint16_t w, h; } fx_frame_t;          // RGB565

typedef struct { const char *name; float min, max, def; } fx_param_desc_t;

typedef struct {
    uint32_t seed;                 // per-shot randomness, stored in the sidecar
    const fx_frame_t *prev;        // previous output, for temporal effects (may be NULL)
    uint32_t frame_no;
    void *scratch; size_t scratch_len;
} fx_ctx_t;

typedef struct {
    const char *id;                // "chanshift"
    const char *name;              // "Channel shift"
    uint8_t  n_params;
    const fx_param_desc_t *params;
    bool     in_place;             // can run with in == out
    bool     temporal;             // needs ctx->prev
    void (*apply)(const fx_frame_t *in, fx_frame_t *out, const float *p, fx_ctx_t *ctx);
} fx_desc_t;
```

A chain is up to three `(fx_id, params[])` entries. A global **amount** (0–100, the knob)
maps onto each effect's main parameter through a per-effect curve, so one knob is enough
for casual use while the drawer exposes every parameter.

### 6.2 Effect catalog

| Id | Effect | Main params | Cost | Phase |
|---|---|---|---|---|
| `chanshift` | RGB channel offset, optional per-band jitter | dx, dy per channel, band height | light | M2 |
| `scanline` | Row displacement / smear / hold | band height, max offset, hold probability | light | M2 |
| `bitcrush` | Bits per channel + ordered dither | bits R/G/B, dither on/off | light | M2 |
| `blocks` | Block shuffle / copy / displace | tile size, count, mode | light | M3 |
| `wave` | Sine displacement in x/y | amplitude, frequency, phase drift | light | M3 |
| `pixelsort` | Asendorf interval sort by luma/hue | threshold lo/hi, direction, key | heavy | M3 |
| `databend` | HW JPEG encode → corrupt N bytes after SOS → decode | quality, corruptions, region | medium | M3 (spike first) |
| `feedback` | Blend previous output with zoom/rotate/offset via PPA | decay, zoom, angle, offset | light (HW) | M3 |
| `mirror` | Mirror / kaleidoscope via PPA mirror + blend | axis, segments | light (HW) | M3 |
| `ispglitch` | Abuse ISP CCM, gamma LUT, hue, saturation, disable denoise | matrix preset, gamma shape, hue | free (HW) | M3 |
| `vhs` | Chroma bleed, noise lines, head-switch wobble | bleed, noise, wobble | medium | M4 |
| `slitscan` | Temporal slit-scan | direction, speed | light, temporal | M4 |
| `datamosh` | H.264 I-frame drop / P-frame reuse | — | stretch | M6 |

Presets are named chains, shipped with 8–10 defaults and user-saved to NVS.

---

## 7. UI design (portrait 720×1280)

**Live view.** Full-screen preview. Top bar: preset name, fps, SD status, battery icon
(GPIO20 ADC). Bottom: horizontal scroll of effect chips (tap = toggle in chain, long-press
= open params), amount slider, soft shutter button, thumbnail of last shot (tap = gallery).
Gestures: swipe left/right = next/previous preset, two-finger tap = re-roll seed, long-press
on preview = freeze frame.

**Effect drawer.** Slides up over the live view. Sliders for each parameter of the selected
effect, chain order (drag), randomize, save as preset.

**Gallery.** Grid of thumbnails from `/sdcard/GLITCH`, full view with recipe info, delete,
"load recipe into live view".

**Settings.** Save clean copy on/off, JPEG quality, shutter sound, preview quality tier
(full/half), backlight, camera flip, orientation lock.

---

## 8. Physical controls wiring (40-pin header)

| Function | GPIO | Header pin | Return |
|---|---|---|---|
| Shutter button | GPIO21 | 15 | GND pin 13 |
| Re-roll button | GPIO22 | 17 | GND pin 19 |
| Encoder CLK | GPIO29 | 20 | 3V3 pin 18, GND pin 26 |
| Encoder DT | GPIO30 | 22 | |
| Encoder SW | GPIO31 | 24 | |

All inputs use internal pull-ups, active low, 20 ms software debounce; encoder decoded with
the PCNT peripheral. The on-board BOOT button (GPIO35) doubles as shutter until the header
is wired. Header pins 21/23 (GPIO24/25) and 7/9 (GPIO37/38) stay free for USB-JTAG and the
serial console.

---

## 9. Repository layout

```
glitchESP/
├── CMakeLists.txt
├── sdkconfig.defaults              # from example 09 + our additions
├── sdkconfig.defaults.rev3_x       # silicon profile (rev1_3 variant kept too)
├── partitions.csv                  # nvs, phy, factory 15M (as example 09)
├── main/
│   ├── idf_component.yml           # esp_video ~2.0, lvgl 9.5.0, waveshare BSP ^1.0.4, hx8394 ^2.1.0
│   ├── app_main.c
│   ├── camera/   app_video.c/.h    # V4L2 wrapper lifted from example 09, plus ISP controls
│   ├── pipeline/ fx_task.c, compositor.c (PPA)
│   ├── effects/  fx.h, fx_registry.c, fx_chanshift.c, fx_scanline.c, ...   # pure C
│   ├── ui/       ui_live.c, ui_drawer.c, ui_gallery.c, ui_settings.c
│   ├── storage/  sd.c, capture.c (HW JPEG), sidecar.c, presets_nvs.c
│   ├── input/    buttons.c, encoder.c
│   └── system/   battery.c, audio_click.c
├── tools/fxlab/                    # PC harness: CMake, main.c, sample images
├── presets/default_presets.json    # embedded via EMBED_FILES
├── doc/                            # existing schematic, datasheet, examples
└── PLAN.md
```

---

## 10. Milestones

Effort is a rough guess in focused hours and assumes the hardware works first try.

### M0 — Environment and hardware check (2–4 h)
- Install ESP-IDF v5.5.5 via the VS Code extension or the Windows installer (~3 GB).
- Build and flash Waveshare example `09_video_lcd_display` unchanged with the `rev3_x`
  profile. Confirm live camera on the LCD.
- Run `esptool.py chip_id` to confirm silicon revision. **Result: v1.3**, so the whole project
  uses the `rev1_3` profile.
- Build example `08_lvgl_demo_v9` and confirm touch.
- **Done when:** camera preview and touch both work on your board.

### M1 — Skeleton camera app (6–10 h)
- New project from the layout in section 9; camera wrapper from example 09.
- LVGL over video: spike D4 option 1 (PPA blend), fall back to option 2. Decide and record.
- BOOT button as shutter → copy frame → HW JPEG encode → write `/sdcard/GLITCH/IMG_nnnn.jpg`
  + JSON sidecar. Thumbnail flash on screen.
- Camera orientation (flip/mirror) fixed; fps counter.
- **Done when:** a plain (unglitched) photo taken with BOOT opens on a PC.

### M2 — Effect engine (8–12 h)
- `fx.h` interface, registry, chain, seed, amount mapping.
- Effects: `chanshift`, `scanline`, `bitcrush`.
- `tools/fxlab` PC harness; each effect runs on a sample image on the PC.
- fx_task on core 1, benchmark each effect at full and half resolution, log fps.
- Amount slider and effect chips in the live view.
- **Done when:** live glitched preview at ≥15 fps for one light effect; shot burns it in.

### M3 — Effect expansion and hardware tricks (10–16 h)
- `blocks`, `wave`, `pixelsort` (half-res preview, full-res on capture).
- `databend` spike: does the HW JPEG decoder accept corrupted scans? If it rejects them,
  use the software decoder (`esp_new_jpeg` or tjpgd) for the databend path only.
- `feedback` and `mirror` on PPA; `ispglitch` via V4L2 ISP controls.
- Half-res quality tier switching.
- **Done when:** 10 effects in the catalog, each with a PC test image and an on-device fps number.

### M4 — UI, presets, gallery (8–12 h)
- Effect drawer with per-parameter sliders and chain ordering.
- Presets: defaults embedded, user presets in NVS, swipe to switch.
- Gallery: thumbnails (HW JPEG decode + PPA downscale), view, delete, load recipe.
- Settings screen, battery indicator, clean-copy option.
- **Done when:** whole camera is usable without a serial cable.

### M5 — Physical controls and polish (4–8 h)
- External shutter + re-roll buttons and rotary encoder per section 8.
- Shutter click via ES8311; backlight dimming on idle; graceful SD-missing handling.
- Enclosure notes: header placement, button holes, LiPo bay. (3D model is out of scope
  unless you want it.)
- **Done when:** you can shoot a session on battery with no PC attached.

### M6 — Stretch (pick any)
- USB mass-storage mode over the OTG port so the SD card mounts on a PC (esp_tinyusb MSC).
- Wi-Fi web gallery via ESP32-C6 (esp_hosted + esp_wifi_remote).
- Hi-res 1920×1080 clean still on capture.
- Video: MJPEG/AVI recording of the glitched preview; H.264 datamosh effect.
- Burst mode with per-frame re-roll.

---

## 11. Risks and mitigations

| Risk | Impact | Mitigation |
|---|---|---|
| No OV5647 module on hand, or a wrong variant | Blocks M0 | Waveshare's OV5647 for this board, or a Raspberry Pi Camera v1.3-compatible 15-pin module. Check before M0. |
| Silicon rev1.x board | Different sdkconfig profile | **Confirmed in M0: this chip is rev v1.3.** Build everything with `rev1_3` (200 MHz PSRAM). Component pins that require rev3 (e.g. esp_audio_codec ≥ 2.6.0) must be avoided. |
| ESP-IDF 5.5 install on Windows | Half a day lost | Use the VS Code extension already installed; 80 GB free disk is plenty. |
| LVGL over live video compositing | UI fps or complexity | Spike both D4 options in M1, pick one, move on. |
| HW JPEG decoder rejects corrupted streams | Databend effect | Software decoder fallback for that path only; still encode with HW. |
| PSRAM bandwidth limits chain length | Preview fps | Half-res preview tier, single-pass effects, in-place where possible. |
| Camera and LCD both in PSRAM, cache coherence | Tearing / stale pixels | Follow example 09's aligned allocs and `esp_cache_msync` after CPU writes before PPA/DSI reads. |
| Shared I2C (touch, codec, camera SCCB) | Bus contention | Use the single BSP I2C handle for all three, as example 09 does. |

---

## 12. Open questions for you

1. **Camera module.** Do you already have the OV5647 module for this board? If not, which will you order?
2. **Framework.** OK with ESP-IDF v5.5.5 in C, not Arduino?
3. **Capture philosophy.** WYSIWYG at 800×1280 with optional clean copy, or do you also want a hi-res clean still on every shot from the start?
4. **Controls.** Just a shutter button, or also the rotary encoder and re-roll button? Comfortable soldering to the header?
5. **Compositing.** Any preference between the two D4 options, or leave it to the M1 spike?
6. **Power.** Will you run from a LiPo on the MX1.25 connector? That decides whether battery UI lands in M4 or later.
7. **Effects priority.** Which 3 effects matter most to you for M2? Default proposal: channel shift, scanline smear, pixel sort.
8. **Enclosure.** Do you want a 3D-printable shell as part of the project or just mounting notes?

---

## 13. Sources

- Waveshare wiki: https://docs.waveshare.com/ESP32-P4-WIFI6-Touch-LCD-5
- Waveshare examples repo (unzipped in `doc/`): https://github.com/waveshareteam/ESP32-P4-WIFI6-Touch-LCD-5
- BSP component: https://components.espressif.com/components/waveshare/esp32_p4_wifi6_touch_lcd_5
- esp_video: https://github.com/espressif/esp-video-components/tree/master/esp_video
- ESP-IDF P4 PPA: https://docs.espressif.com/projects/esp-idf/en/stable/esp32p4/api-reference/peripherals/ppa.html
- ESP-IDF P4 JPEG: https://docs.espressif.com/projects/esp-idf/en/stable/esp32p4/api-reference/peripherals/jpeg.html
- ESP-IDF P4 ISP: https://docs.espressif.com/projects/esp-idf/en/stable/esp32p4/api-reference/peripherals/isp.html
- ESP-IDF P4 camera driver: https://docs.espressif.com/projects/esp-idf/en/stable/esp32p4/api-reference/peripherals/camera_driver.html
- Arduino ESP_Video (for comparison): https://developer.espressif.com/blog/2026/09/arduino-esp-video-camera-capture/
- esp32_p4_eye camera+LCD example: https://components.espressif.com/components/espressif/esp32_p4_eye/versions/2.0.0/examples/display_camera_csi
- P4 UVC webcam (OV5647): https://github.com/r4d10n/esp32p4-uvc-video and https://github.com/efloresaraya/ov5647-uvc-webcam-esp32p4
- Pi 4 glitch camera: https://hackaday.com/2026/05/08/easy-ish-glitch-camera-theres-a-pi-4-that/ , https://www.hackster.io/news/easy-glitch-photography-with-a-raspberry-pi-02a2b1b91e21 , https://www.yankodesign.com/2026/05/10/diy-raspberry-pi-camera-turns-your-photos-into-glitch-art-and-the-results-are-incredible/ , https://reddit.com/r/raspberry_pi/comments/1t5tuu3/built_a_glitch_camera_with_raspberry_pi/
- Circuit-bent glitch camera: https://blog.adafruit.com/2025/06/28/how-to-make-a-glitch-camera-photography-celebratephotography/
- Pixel sorting: https://satyarth.me/articles/pixel-sorting/ , https://glitchology.com/pixel-sorting/ , https://kimasendorf.com/mountain-tour/pixel-sorting-meeting-chaos-half-way/
- Databending / JPEG corruption: https://github.com/untruesudo/bitrot-canvas , https://github.com/mrjoshida/bendr
- Effect taxonomy: https://play.google.com/store/apps/details?id=com.ilixa.glitch
