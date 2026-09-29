# glitchESP

A handheld glitch-art camera on the Waveshare **ESP32-P4-WIFI6-Touch-LCD-5** (5" 720×1280 touch
display, OV5647 MIPI-CSI camera, microSD). Live preview with real-time glitch effects, a shutter
button, JPEG capture with a reproducible "recipe" sidecar.

| Folder | What |
|---|---|
| `PLAN.md` | Roadmap, hardware facts, decisions, milestones |
| `firmware/` | ESP-IDF v5.5 project (the camera app) — see its README for build/flash |
| `tools/fxlab/` | PC harness: runs the same effect code on images in milliseconds |
| `docs/EFFECTS.md` | How the effect engine works and how to add an effect |
| `m0/` | Hardware bring-up notes and helper scripts |
| `doc/` | Schematic, datasheet, pointers to Waveshare material |

Status: hardware verified, camera preview + six effects running on the device, JPEG capture
implemented (SD card test pending), performance work in progress. Built with ESP-IDF v5.5.5 for
ESP32-P4 silicon rev v1.3.
