# glitchESP

[![Firmware](https://github.com/koupypavel/glitchESP/actions/workflows/firmware.yml/badge.svg)](https://github.com/koupypavel/glitchESP/actions/workflows/firmware.yml)
[![Release](https://img.shields.io/github/v/release/koupypavel/glitchESP)](https://github.com/koupypavel/glitchESP/releases/latest)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

A handheld glitch-art camera built on the Waveshare **ESP32-P4-WIFI6-Touch-LCD-5**: a 5-inch
720×1280 touch display, an OV5647 MIPI-CSI camera and a microSD slot. The preview is glitched
live, the shutter burns the effect into the saved photo or video, and every shot gets a small
"recipe" file so its look can be reproduced.

![the camera in its printed case](docs/photos/enclosure_front.jpg)

Photos straight from the camera, one scene through different effects:

| Drift | Hue drift | Hue drift | Diffraction |
|---|---|---|---|
| ![drift](docs/photos/car_drift.jpg) | ![hue drift](docs/photos/car_hue_green.jpg) | ![hue drift](docs/photos/car_hue_magenta.jpg) | ![diffraction](docs/photos/car_diffraction.jpg) |

| Databend | Blocks + bit crush + pixel sort | Squint | Tracers + hue drift + drift |
|---|---|---|---|
| ![databend](docs/photos/car_databend.jpg) | ![blocks, bit crush, pixel sort](docs/photos/car_blocks_sort.jpg) | ![squint](docs/photos/car_squint.jpg) | ![tracers, hue drift, drift](docs/photos/car_tracers_drift.jpg) |

At night:

| Channel shift | Tracers + hue drift + drift | VHS + light trails |
|---|---|---|
| ![channel shift](docs/photos/night_chanshift.jpg) | ![tracers, hue drift, drift](docs/photos/night_tracers_hue.jpg) | ![VHS, light trails](docs/photos/night_vhs_trails.jpg) |

Video recorded on the camera (clips of 7 to 9 seconds, scaled down to half size):

| Drift + breathe | Kaleido | Night: hue drift + drift |
|---|---|---|
| ![drift and breathe](docs/photos/video_drift_breathe.webp) | ![kaleido](docs/photos/video_kaleido.webp) | ![hue drift and drift at night](docs/photos/video_night_hue_drift.webp) |

More effects, rendered by the PC harness on synthetic test scenes:

| Channel shift | Kaleido | Drift | Diffraction |
|---|---|---|---|
| ![channel shift](tools/fxlab/samples/out_chanshift.png) | ![kaleido](tools/fxlab/samples/out_kaleido.png) | ![drift](tools/fxlab/samples/out_drift.png) | ![diffraction](tools/fxlab/samples/out_diffract.png) |

| VHS | Databend | Palette | Squint |
|---|---|---|---|
| ![VHS](tools/fxlab/samples/out_vhs.png) | ![databend](tools/fxlab/samples/out_databend.png) | ![palette](tools/fxlab/samples/out_palette.png) | ![squint](tools/fxlab/samples/out_squint.png) |

## What it does

- **Live preview** at about 20 fps (10 to 18 fps with effects, depending on the chain), with
  up to three effects chained. Twenty effects so far:
  channel shift, scanline smear, bit crush, blocks, wave, pixel sort, tracers, hue drift,
  kaleido, diffraction, drift, breathe, VHS, slit scan, databend, palette (a handful of
  fixed colours with a line dither, seven palettes from acid to Game Boy), squint (a
  painter's squint: blur away the detail, keep the big shapes of light and shadow), and three made
  for the night: starburst (rays from every light), light trails (moving lights leave
  glowing trails) and neon (glowing outlines on a dark picture).
- **Low light**: when it gets dark the camera slows down by itself, from 20 to 10 and then
  7 frames a second, to expose two and three times longer, and averages frames to take the
  noise down. A photo taken then is the average of six frames: hold still or use the tripod
  thread.
- **Battery level** on the status line, a warning at 10 %, and a recording is closed
  properly before the battery runs out.
- **One knob for "how broken".** An amount slider drives every active effect through its own
  mapping; a seed button re-rolls the randomness. Every parameter can also be set by hand.
- **Photos**: hardware JPEG saved as `GLITCH/IMG_nnnn.jpg` with a `.json` sidecar listing
  the effect chain, parameters, seed and frame number. At 1× zoom the sensor is re-read at
  full resolution for the shot and the effects are applied again at that size: 1088×1920
  (2.1 MP), about 0.7 s per photo. Zoomed in, the 720×1280 frame on screen is saved.
- **Burst**: 3, 5 or 10 photos per press, each with a new random seed, so one press gives
  several variations of the same look.
- **Video** recorded from the same frames you see, in one of two formats: Motion-JPEG AVI
  (`GLITCH/VID_nnnn.avi`, every preview frame, about 2 MB/s, plays in the gallery) or
  H.264 MP4 (`VID_nnnn.mp4`, about six times smaller, 7 to 9 fps, plays on a computer).
- **Zoom** from 1× to 6×. 1× uses the sensor's 2×2 binned mode (widest view, least noise),
  1.5× shows sensor pixels one to one, beyond that the picture is enlarged digitally.
- **Presets**: eight slots for effect recipes (effects, amount, seed), stored in flash;
  four starter looks are filled in on first boot.
- **Gallery**: browse the photos and play the videos on the card, delete them, or take the
  look of any picture back into the camera ("Use look" reads its recipe sidecar).
- **USB storage**: Settings has a "USB storage" button that hands the SD card to a
  computer as a USB drive over the board's USB OTG port; "Done" takes it back.
- **Sounds**: a shutter click and recording beeps through the board's speaker connector.
- **Settings** for mirror, flip, photo size, idle dimming, sound, burst, video format and
  preview quality, stored in flash. The panel also shows the firmware version.

## Controls

| Input | Action |
|---|---|
| Effect chips (bottom bar) | Toggle an effect; up to three run in order |
| Handle (tab above the bar) | Hide the controls (bar, status line, zoom) for a clear view, or bring them back; remembered |
| Amount slider | Intensity of all active effects |
| Edit button (pencil) | A slider or switch for every parameter of each active effect, with a reset |
| Seed button (arrows) | New random seed |
| Presets button (list) | Eight slots: the disk icon stores the current look, tapping a row recalls it |
| Gallery button (picture) | Browse with the arrows or by swiping; play, "Use look", delete (tap twice), close |
| + / − (right edge) | Zoom in and out |
| Gear button | Settings: mirror, flip, full-resolution photos, idle dimming, shutter sound, burst, video format, low light, preview quality, USB storage |
| BOOT button, short press | Take a photo, or a burst if one is set (in the gallery: back to the camera) |
| BOOT button, hold 0.7 s | Start or stop video recording |

### Optional wired controls

Buttons and a rotary encoder can be wired to the 40-pin header. Every input has a pull-up
and switches to ground, so nothing needs to be connected for the camera to work. Check the
header's orientation against the board's silkscreen before wiring.

| Control | GPIO | Header pin | Action |
|---|---|---|---|
| Shutter button | 21 | 15 (ground: 13) | Click = photo, hold = video, as BOOT |
| Re-roll button | 22 | 17 (ground: 19) | New random seed |
| Encoder A / B | 29 / 30 | 20 / 22 (common to ground: 26) | Amount knob; in the gallery, previous / next |
| Encoder push | 31 | 24 | Click = next preset (in the gallery: play), hold = hide / show the controls |

These are implemented but have not been tried with real switches yet.

## Hardware

- Waveshare ESP32-P4-WIFI6-Touch-LCD-5 (ESP32-P4, 32 MB PSRAM, 32 MB flash).
- OV5647 camera module on the MIPI-CSI connector (sold separately).
- A microSD card (FAT32) for photos and video.

Developed and tested on ESP32-P4 silicon **revision v1.3**. Revision 3.x boards need the
`rev3_x` build profile and have not been tested.

### Battery and charging

The board has a connector (MX1.25) for a single-cell 3.7 V lithium battery and a charger
chip on it; the enclosure's bay takes one or two LP653454 pouch cells in parallel (every
cell must have its own protection circuit). The camera runs from the battery alone and
shows its level on the status line.

- **Charging:** plug a USB-C cable from any 5 V USB charger or computer into the
  **USB-UART port** (the one used for flashing). The board's charger does the rest; the
  camera can be on or off. Two cells take a few hours from nearly empty.
- **Indication:** the board has no charging signal, so the firmware watches the voltage:
  while it rises the status line shows a `+` before the percentage (about a minute after
  plugging in), and a full battery on USB shows 100 % without the `+`. When charging is
  first detected the camera shows "charging: do not leave unattended".
- **Low battery:** a warning at 10 %; below 3.4 V a running recording is closed so the file
  stays readable.
- **Power LED:** the board's power LED sits on the 5 V rail, not behind the power button,
  so it may stay lit with the camera switched off. If that matters, put a slide switch in
  the battery's red lead; a switched-off battery does not charge.

Lithium cells are not toys: use protected cells, do not leave the first charges
unattended, and stop if the cells get warm or swell.

## Install a release

Each [release](https://github.com/koupypavel/glitchESP/releases) carries images built for
the tested board (ESP32-P4 revision v1.x). You need Python with esptool (`pip install
esptool`) and the board connected through its USB-UART port; replace `COM10` with your port
(`/dev/ttyUSB0`, `/dev/cu.usbserial-...`).

A first install, from the single merged image (this also clears the settings and presets
stored in flash):

```bash
python -m esptool --chip esp32p4 -p COM10 -b 460800 write_flash 0x0 glitchesp-v0.2.0-rev1_3-merged.bin
```

An update that keeps settings and presets: unpack `glitchesp-v0.2.0-rev1_3-parts.zip` and
run this inside the unpacked folder:

```bash
python -m esptool --chip esp32p4 -p COM10 -b 460800 write_flash @flash_args
```

## Enclosure

[`hardware/enclosure`](hardware/enclosure) has a printable case with a battery bay, a
speaker pocket, openings for every port and button, a shutter switch, a rotary encoder,
a tripod thread and an adjustable lens hood, as STL files and as the CadQuery script that
generates them. The current version (4) has been printed and is in use.

| Back: lens hood, speaker grille | Inside: two LP653454 cells in the bay |
|---|---|
| ![back of the case](docs/photos/enclosure_back.jpg) | ![inside the case](docs/photos/enclosure_inside.jpg) |

![enclosure model](hardware/enclosure/img/back.png)

## Build and flash

You need ESP-IDF **v5.5.5**. On Windows, from `firmware/`:

```powershell
.\build.ps1              # build (rev1_3 profile)
.\build.ps1 COM10        # build and flash over the USB-UART port
```

On Linux and macOS, with the ESP-IDF environment active:

```bash
./build.sh                # build
./build.sh /dev/ttyUSB0   # build and flash
```

The component versions are pinned in `firmware/dependencies.lock`, and the GitHub workflow
in `.github/workflows/firmware.yml` builds every push the same way.

Details, the module layout and design notes are in [`firmware/README.md`](firmware/README.md).

## Developing effects

Effects are plain C with no ESP-IDF dependency, so the same files run on a PC in
milliseconds. [`tools/fxlab`](tools/fxlab) builds them into a small command-line tool:

```powershell
tools\fxlab\build.ps1
tools\fxlab\out\fxlab.exe --list
tools\fxlab\out\fxlab.exe in.ppm out.ppm --amount 0.6 chanshift scanline
```

(`tools/fxlab/build.sh` does the same with gcc or clang.)

[`docs/EFFECTS.md`](docs/EFFECTS.md) explains the engine, each effect, how to add one, and
what was learned about performance on the ESP32-P4.

## Repository layout

| Path | Contents |
|---|---|
| `firmware/` | The ESP-IDF project |
| `tools/fxlab/` | PC harness for the effect code, with sample renders |
| `docs/EFFECTS.md` | Effect engine guide and measured performance model |
| `CHANGELOG.md` | What each release contains |
| `.github/workflows/` | Build of the firmware and the harness on every push; releases on a version tag |
| `hardware/enclosure/` | Printable case: STL files, the CadQuery model that makes them, assembly notes |
| `PLAN.md` | Original plan, hardware facts and research notes |
| `m0/` | Hardware bring-up notes and the two stock examples used for it |
| `doc/` | Board schematic and pointers to vendor documentation |

## Status

Working on the device (ESP32-P4 revision v1.3): preview, all twenty effects on both cores,
parameter editing, presets, zoom, settings, photos (including full-resolution stills and
bursts), video (Motion-JPEG and H.264) saved to the card, the gallery, the battery level
and the low-light mode. The low-light mode and the three night effects were tried indoors
only, not yet outside at night.

## To do

Waiting for hardware or a test:

- [ ] Try the low-light mode and the night effects outside at night; tune the levels
- [ ] Battery: the percentage comes from a typical LiPo curve, check it against a full
      discharge; the board has no charging signal, so "+" (charging) is a guess from the
      voltage rising (charging over the USB-UART port itself works, a few hours for two cells)
- [ ] USB storage works with a Windows PC; the copy speed has not been measured, and
      macOS and Linux have not been tried
- [ ] Try the wired controls on the header with real switches and an encoder
- [ ] Build and try the `rev3_x` profile on a revision 3.x board; add it to the workflow
- [ ] Flash the single merged release image to a board (so far it has only been compared,
      byte for byte, with the three images the normal flash writes)

Camera:

- [ ] Reorder the effect chain on screen (effects now always run in the order of the chips)
- [ ] More than one history effect at a time (tracers and slit scan share one buffer)
- [ ] A clean, unglitched copy next to each photo, as an option
- [ ] Larger stills: the ISP takes at most 1920 pixels per line, so the full 5 MP frame
      would need RAW capture and demosaicing in software
- [ ] Drawing the control bar costs about 4 fps while it is shown; stamp only what changed

Effects:

- [ ] Feedback (zoom and rotate the previous frame into the next)
- [ ] ISP glitches (colour matrix, gamma and hue of the image processor pushed out of range)
- [ ] Haze / glow, radial block displacement, melting (see the end of section 6 in
      `docs/EFFECTS.md`)
- [ ] Datamosh on the H.264 stream (dropped key frames)

Video and gallery:

- [ ] Faster H.264: the RGB to YUV conversion takes 60 to 70 ms per frame and limits it to
      7 to 9 fps
- [ ] Sound in videos (the board has a microphone input)
- [ ] Play H.264 recordings in the gallery (there is no hardware decoder)
- [ ] Thumbnail grid in the gallery

Connectivity:

- [ ] Wi-Fi gallery or transfer through the board's ESP32-C6

Project:

- [ ] Enclosure ([`hardware/enclosure`](hardware/enclosure), printed and in use): lanyard eye

## License

MIT, see [`LICENSE`](LICENSE). Third-party material and the components fetched at build time
are listed in [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md). Contributions are welcome:
see [`CONTRIBUTING.md`](CONTRIBUTING.md).

This project is not affiliated with Espressif Systems or Waveshare.
