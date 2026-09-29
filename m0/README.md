# M0 — Environment and hardware check

Goal: prove the toolchain, the display, touch, and the OV5647 camera work on this exact
board before writing any glitchESP code. Two unmodified Waveshare examples are used:

| Folder | What it proves |
|---|---|
| `09_video_lcd_display` | ESP-IDF v5.5.5 + esp_video + BSP build; OV5647 live preview on the LCD |
| `08_lvgl_demo_v9` | LVGL 9 + GT911 touch |

## Toolchain

ESP-IDF v5.5.5 is installed at `%USERPROFILE%\esp\v5.5.5\esp-idf` (same layout as the
older 5.1.4 and 5.3 installs). Tools live in `%USERPROFILE%\.espressif`.

Open an IDF shell in PowerShell:

```powershell
. $env:USERPROFILE\esp\v5.5.5\esp-idf\export.ps1
```

## Build / flash

`build.ps1` wraps the Waveshare recipe (separate build dir + sdkconfig per silicon profile):

```powershell
.\build.ps1 09_video_lcd_display          # build only
.\build.ps1 09_video_lcd_display COM7     # build + flash (use your USB-UART port)
```

**This board's chip is ESP32-P4 revision v1.3** (probed 2026-09-29), so always pass the `rev1_3`
profile. esptool refuses a `rev3_x` image on this chip ("bootloader requires chip revision v3.0 - v3.99").

## Checks

1. Connect the board's **USB-UART** Type-C port (not the OTG one). Find the COM port:
   `Get-CimInstance Win32_PnPEntity | ? Name -match 'COM\d+'`.
2. Silicon revision: `esptool.py --port COMx chip_id` → expect `Chip is ESP32-P4 (revision v3.x)`.
3. Flash 09, then capture the log: `python serial_capture.py COMx 10` (run with the IDF python env).
   Expect `app_video: Video Stream Start` and no `E (` lines. The LCD shows the camera image.
4. Flash 08. The LVGL benchmark runs; touch is verified in M1 with a widgets demo if needed.

## Results (2026-09-29)

- ESP-IDF v5.5.5 installed; both examples configure and build.
- Board serial: CH343 bridge, now **COM10**. It was unusable at first because a com0com
  virtual port owned the same COM name (writes blocked forever); renaming the board's port
  fixed it. Auto-reset into download mode works but sometimes needs `--connect-attempts 8`.
- Chip: `ESP32-P4 (revision v1.3)`, 32 MB flash (GD25Q256).
- Example 09 (rev1_3) flashed: boot log shows `ov5647: Detected Camera sensor PID=0x5647`,
  `width=800 height=1280`, `Video Stream Start`, no `E (` lines. Live preview confirmed on the
  LCD by eye. Touch controller reported: `Touch 0x5d found`, GT911 config version 70.
- Warnings worth remembering for M1: `ledc: GPIO 26 is not usable, maybe conflict with others`
  (backlight still works), `i2c.master: Please check pull-up resistances` (touch/camera work).
- Example 08 changed to `lv_demo_widgets()`; touch confirmed working by the user (taps land where pressed).
- Serial log capture: `python serial_capture.py COM10 10` (uses esptool's reset; a bare
  pyserial RTS pulse does not reset this board).

