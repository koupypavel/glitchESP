# Contributing

Issues and pull requests are welcome. This is a hobby project, so please keep changes small
and focused.

## Before you open a pull request

- **Build it.** `firmware/build.ps1` builds the firmware with ESP-IDF v5.5.5 (see
  `firmware/README.md`). The default silicon profile is `rev1_3`; if your ESP32-P4 is
  revision 3.x, build with the `rev3_x` profile.
- **Effects run on the PC first.** New or changed effects must build and produce sensible
  output in `tools/fxlab` before they go near the device. `docs/EFFECTS.md` explains the
  engine, the row-parallel contract and the performance rules (no per-pixel division, no
  large tables in on-chip RAM, prefer two pixels per 32-bit word).
- **Say what you measured.** For anything touching the frame pipeline, include the stats line
  from the serial log (`cam N fps | ... fx N us ...`) before and after.
- **No private data in logs.** Remove MAC addresses, Wi-Fi credentials and machine-specific
  paths from anything you paste.

## Style

- C99, four-space indentation, LF line endings (`.editorconfig` and `.gitattributes` enforce
  the basics).
- Effect code stays free of ESP-IDF headers so it compiles in `fxlab`; use `FX_HOT` and
  `fx_big_alloc()` from `fx.h` for the two things that differ on the device.
- Comments explain why, not what.

## Licensing

By contributing you agree that your contribution is released under the MIT License of this
repository. Do not add code under incompatible terms; third-party material must be listed in
`THIRD_PARTY_NOTICES.md`.
