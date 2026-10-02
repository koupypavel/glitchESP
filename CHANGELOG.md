# Changelog

Each release has a section here; the GitHub release notes are taken from it.

## Unreleased

- Three effects for the night: starburst, light trails, neon. The effect buttons now scroll.
- Low-light mode: longer frames (10 and 6.7 fps) with two and three times the exposure when
  it is dark, frames averaged against noise, night photos averaged from six frames. Setting
  "Low light": Auto, Off, Always.
- Battery level on the status line, low-battery warning, recordings closed before the
  battery is empty.
- Enclosure in `hardware/enclosure` (printed and in use).

## [0.1.0] - 2026-10-01

First release. Built for the Waveshare ESP32-P4-WIFI6-Touch-LCD-5 with an OV5647 camera,
ESP32-P4 silicon revision v1.x (`rev1_3` profile), with ESP-IDF v5.5.5. Everything listed
was tried on a revision v1.3 board; the binaries attached to the release are built by the
GitHub workflow from the same source.

**Camera**

- Live preview on the 720×1280 touch display at about 20 fps, 14 to 18 fps with effects.
- Sixteen effects, up to three chained: channel shift, scanline smear, bit crush, blocks,
  wave, pixel sort, tracers, hue drift, kaleido, diffraction, drift, breathe, VHS, slit
  scan, databend, squint.
- One amount knob for all active effects, a seed button, and an editor for every parameter.
- Zoom 1× to 6× (2×2 binned sensor mode at 1×, one sensor pixel per screen pixel at 1.5×).

**Capture**

- Photos as JPEG with a `.json` recipe next to each. At 1× zoom the sensor is read again at
  full resolution and the effects are applied at that size: 1088×1920.
- Burst: 3, 5 or 10 photos per press, each with a new random seed.
- Video with the effects burned in: Motion-JPEG AVI (every preview frame, about 2 MB/s) or
  H.264 MP4 (about six times smaller, 7 to 9 fps).

**Around it**

- Eight preset slots, four starter looks.
- Gallery on the device: browse, play Motion-JPEG videos, delete, take the look of a photo
  back into the camera.
- Settings stored in flash, idle dimming, shutter click and recording beeps, SD card can be
  inserted at any time.
- Optional wired controls on the 40-pin header (shutter, re-roll, rotary encoder).
- Serial remote for driving and inspecting the camera from a PC.

**Known limits**

- H.264 recordings cannot be played on the device and reach 7 to 9 fps.
- The wired controls and the `rev3_x` build profile have not been tried on hardware.
- No battery indicator yet.
- The single merged image has not been flashed to a board yet. It contains the same three
  images as the `parts` archive, which is what a normal flash writes.

**Installing**

Write `glitchesp-v0.1.0-rev1_3-merged.bin` to address 0 with esptool (this also resets the
settings and presets), or flash the three images from the `parts` archive with
`esptool.py --chip esp32p4 write_flash @flash_args` to keep them. See "Install a release"
in the README.
