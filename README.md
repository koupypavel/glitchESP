# glitchESP

A handheld glitch-art camera built on the Waveshare **ESP32-P4-WIFI6-Touch-LCD-5**: a 5-inch
720×1280 touch display, an OV5647 MIPI-CSI camera and a microSD slot. The preview is glitched
live, the shutter burns the effect into the saved photo or video, and every shot gets a small
"recipe" file so its look can be reproduced.

| Channel shift | Kaleido | Drift | Diffraction |
|---|---|---|---|
| ![channel shift](tools/fxlab/samples/out_chanshift.png) | ![kaleido](tools/fxlab/samples/out_kaleido.png) | ![drift](tools/fxlab/samples/out_drift.png) | ![diffraction](tools/fxlab/samples/out_diffract.png) |

*Effects rendered by the PC harness on synthetic test scenes.*

## What it does

- **Live preview** at about 20 fps, with up to three effects chained. Twelve effects so far:
  channel shift, scanline smear, bit crush, blocks, wave, pixel sort, tracers, hue drift,
  kaleido, diffraction, drift and breathe.
- **One knob for "how broken".** An amount slider drives every active effect through its own
  mapping; a seed button re-rolls the randomness.
- **Photos**: hardware JPEG, 720×1280, saved as `GLITCH/IMG_nnnn.jpg` with a `.json` sidecar
  listing the effect chain, parameters, seed and frame number.
- **Video**: Motion-JPEG AVI (`GLITCH/VID_nnnn.avi`) recorded from the same frames you see,
  at roughly 12 fps.
- **Zoom** from 1× to 6×. 1× uses the sensor's 2×2 binned mode (widest view, least noise),
  1.5× shows sensor pixels one to one, beyond that the picture is enlarged digitally.
- **Settings** for mirror, flip and preview quality, stored in flash.

## Controls

| Input | Action |
|---|---|
| Effect chips (bottom bar) | Toggle an effect; up to three run in order |
| Amount slider | Intensity of all active effects |
| Seed button | New random seed |
| + / − (right edge) | Zoom in and out |
| Gear button | Settings: mirror left/right, flip up/down, preview quality |
| BOOT button, short press | Take a photo |
| BOOT button, hold 0.7 s | Start or stop video recording |

## Hardware

- Waveshare ESP32-P4-WIFI6-Touch-LCD-5 (ESP32-P4, 32 MB PSRAM, 32 MB flash).
- OV5647 camera module on the MIPI-CSI connector (sold separately).
- A microSD card (FAT32) for photos and video.

Developed and tested on ESP32-P4 silicon **revision v1.3**. Revision 3.x boards need the
`rev3_x` build profile and have not been tested.

## Build and flash

You need ESP-IDF **v5.5.5**. On Windows, from `firmware/`:

```powershell
.\build.ps1              # build (rev1_3 profile)
.\build.ps1 COM10        # build and flash over the USB-UART port
```

On other systems use `idf.py` with the same defaults:

```bash
idf.py -B build/rev1_3 -D SDKCONFIG=build/rev1_3/sdkconfig -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.rev1_3" build flash
```

Details, the module layout and design notes are in [`firmware/README.md`](firmware/README.md).

## Developing effects

Effects are plain C with no ESP-IDF dependency, so the same files run on a PC in
milliseconds. [`tools/fxlab`](tools/fxlab) builds them into a small command-line tool:

```powershell
tools\fxlab\build.ps1
tools\fxlab\out\fxlab.exe --list
tools\fxlab\out\fxlab.exe in.ppm out.ppm --amount 0.6 chanshift scanline
```

[`docs/EFFECTS.md`](docs/EFFECTS.md) explains the engine, each effect, how to add one, and
what was learned about performance on the ESP32-P4.

## Repository layout

| Path | Contents |
|---|---|
| `firmware/` | The ESP-IDF project |
| `tools/fxlab/` | PC harness for the effect code, with sample renders |
| `docs/EFFECTS.md` | Effect engine guide and measured performance model |
| `PLAN.md` | Original plan, hardware facts and research notes |
| `m0/` | Hardware bring-up notes and the two stock examples used for it |
| `doc/` | Board schematic and pointers to vendor documentation |

## Status

Working on the device: preview, all twelve effects on both cores, zoom, settings, and
photos and video saved to the card. Still planned: higher-resolution stills, presets, an on-device gallery, per-parameter effect
editing, a wired shutter button and rotary knob on the expansion header, and a smaller video
format using the P4's H.264 encoder.

## License

MIT, see [`LICENSE`](LICENSE). Third-party material and the components fetched at build time
are listed in [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md). Contributions are welcome:
see [`CONTRIBUTING.md`](CONTRIBUTING.md).

This project is not affiliated with Espressif Systems or Waveshare.
