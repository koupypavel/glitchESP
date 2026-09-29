# The glitchESP effect engine, explained

This is the "how it works" companion to `firmware/main/effects/`. It is written for someone
who knows C but has not done image processing before.

## 1. A frame is just an array

The camera hands us a frame as one flat array of 16-bit numbers, one per pixel, row after row.
Each 16-bit value packs the colour as **RGB565**: 5 bits red, 6 bits green, 5 bits blue
(green gets the extra bit because our eyes are most sensitive to it).

```
bit 15 ........ 11 10 ........ 5 4 ........ 0
    R R R R R      G G G G G G    B B B B B
```

`fx.h` gives you helpers to take a pixel apart and put it together again:
`fx_r5()`, `fx_g6()`, `fx_b5()`, `fx_rgb565()`, and `fx_luma()` for an approximate brightness.

The frame struct is:

```c
typedef struct {
    uint16_t *px;        // pixel data
    uint16_t w, h;       // size in pixels (720 x 1280 on the display path)
    uint32_t stride_px;  // pixels per row; lets a frame be a window into a wider buffer
} fx_frame_t;
```

Pixel (x, y) lives at `px[y * stride_px + x]`. Every effect is built around that one formula.

## 2. An effect is one function plus a description

```c
typedef struct fx_desc {
    const char *id, *name;             // "chanshift", "Channel shift"
    uint8_t n_params;                  // how many sliders
    const fx_param_t *params;          // each: id, label, min, max, default
    bool in_place, temporal;           // capabilities (see below)
    void (*from_amount)(float amount, float *params);   // one-knob mapping
    void (*apply)(const fx_frame_t *in, fx_frame_t *out,
                  const float *params, const fx_ctx_t *ctx);
} fx_desc_t;
```

- `apply()` reads `in` and writes `out`. Both have the same size. Never assume they are the same
  buffer unless you set `in_place = true` and you really only touch pixel (x,y) from pixel (x,y).
- `params` is a float array in the order of the description table. The UI builds sliders from
  the table, the sidecar JSON records the values, and `fxlab` lets you set them from the command
  line. No effect needs to know about any of that.
- `from_amount()` turns the single "amount" knob (0 to 1) into a full parameter set. This is how
  one rotary encoder can drive a whole chain: each effect decides what "more" means for itself.

Effects are pure C99 with no ESP-IDF headers. That is why the same files compile in
`tools/fxlab` on the PC and let you iterate in seconds.

## 3. Randomness that can be replayed

Glitches need randomness, but a saved shot must be reproducible. `fx_rng.h` provides a tiny
xorshift generator. It is seeded from a hash of `(seed, frame_no, salt)`:

- `seed` changes only when the user presses re-roll,
- `frame_no` is the camera frame counter, so the live preview keeps changing,
- `salt` is a constant per effect so two effects in the same chain do not produce the same
  random sequence.

Because the sidecar stores `seed` and `frame_no`, running the same chain with the same values
on the same input gives the identical picture.

## 4. The chain

`fx_chain_t` holds up to three slots (effect + its parameters + enabled flag).
`fx_chain_apply()` runs them in order. Since an effect must not write into the buffer it reads
(channel shift samples pixels to the left and right that would already be overwritten), the
chain ping-pongs: effect 1 writes to a temp buffer, effect 2 reads temp and writes the output,
and so on. The last enabled effect always lands in `out`. With nothing enabled the input is
simply copied.

## 5. Where it runs on the device

```
camera (800x1280) --PPA crop--> crop_buf (720x1280) --fx_chain_apply--> ring[n] --> LVGL canvas
                       (hardware, ~free)               (CPU, core 1)
```

- With an empty chain the PPA crops straight into the display ring and the CPU does nothing.
- The UI never touches the chain that the camera task is using. It sends a new copy through
  `frame_pipeline_set_chain()`; the camera task copies it under a critical section at the start
  of each frame. That is the whole thread-safety story: tiny copies, no locks held while working.
- The effect stage is timed; the status bar shows it as `fx N ms`.
- On the shutter press, the finished frame *and* the recipe that made it are snapshotted
  together, so the JPEG and its JSON always agree.

## 6. The first three effects

**Channel shift** (`fx_chanshift.c`). Output red comes from the input at one offset, green from
another, blue from a third; the bit masks `0xF800 / 0x07E0 / 0x001F` merge them. Every `band`
rows the random offsets are re-rolled, which is what makes it tear in bands instead of shifting
uniformly.

**Scanline smear** (`fx_scanline.c`). The image is processed row by row. At the start of each
band the RNG picks one of three outcomes: shift the band sideways (two `memcpy` calls with
wrap-around, extremely cheap), repeat the previous row for a while (the vertical smear), or copy
the band unchanged.

**Bit crush** (`fx_bitcrush.c`). Keeps only the top `bits` of each channel, which posterizes the
colours. To avoid ugly flat bands it first adds a small position-dependent bias from a 4x4
Bayer matrix: the classic ordered-dither look.

**Blocks** (`fx_blocks.c`). Starts from a copy of the input, then moves `count` rectangular
tiles to random places, mostly snapped to the tile grid so it reads as corrupted video
macroblocks. Tiles are always read from the untouched input, so copies stay crisp.

**Wave** (`fx_wave.c`). Each output pixel is fetched from a position displaced by a sine of its
row (horizontal wobble, the "VHS tracking" look) and optionally of its column. The sine comes
from a 1024-entry integer lookup table with fixed-point phase stepping, because a float `sinf`
per pixel would be far too slow on the P4. The phase advances with `frame_no` so it animates.

**Pixel sort** (`fx_pixelsort.c`). Along each column (or row) it finds runs of pixels whose
brightness lies between `lo` and `hi` and sorts each run by brightness. The sort is a counting
sort over the 256 brightness levels: linear time, no recursion, scratch in static buffers.
Widening the window with the amount knob makes runs longer, hence longer streaks. Note that on
the device this reads and writes columns, which is cache-unfriendly; it is the first candidate
for the half-resolution preview path.

## 7. Adding an effect (checklist)

1. Copy `fx_bitcrush.c` to `fx_yourname.c`. Fill the parameter table, `from_amount()`, `apply()`.
2. Add `extern const fx_desc_t fx_yourname;` and the pointer to the array in `fx_registry.c`.
3. Add the file to `firmware/main/CMakeLists.txt`.
4. Build the PC harness and look at it:
   ```
   tools\fxlab\build.ps1
   tools\fxlab\out\fxlab.exe tools\fxlab\samples\synth.ppm out.ppm --amount 0.6 yourname
   python tools\fxlab\img.py out.ppm out.png
   ```
   `fxlab --list` prints every effect and parameter. `--repeat 50` gives a rough timing.
5. Flash. The chip appears in the control bar automatically.

## 8. Performance on the ESP32-P4 (measured, rev v1.3, PSRAM at 200 MHz)

Everything below was measured on the device with the camera running and the display active.

**The memory bus is the budget, not the ALU.** Frames live in PSRAM. One pass that reads and
writes a 720x1280 RGB565 frame costs about 27 ms even as a plain `memcpy`; a loop doing
one 16-bit load and store per pixel costs 43 ms, and a loop doing 32-bit loads and stores
(two pixels per word) costs 28 to 30 ms. The same loops on internal RAM take 6 to 12 ms. So:

- process two pixels per 32-bit word wherever the effect allows it (`fx_bitcrush.c` does),
- never divide or modulo per pixel (35 cycles each), precompute per row or use lookup tables,
- prefer row-oriented access; a column walk touches a cache line per pixel and costs ~1 s/frame,
- one pass is the unit of cost: a three-effect chain is three passes.

**Do not let anything else stream through PSRAM while effects run.** The first design drew the
video through an LVGL canvas, which copied every frame a second time on core 0 and slowed the
effects on core 1 by 3x. The current design writes video straight into the panel frame buffers
(`display/`) and LVGL only renders widgets into a side layer that is stamped on (`ui/ui_lvgl.c`).

**Half-resolution preview.** With `FP_QUALITY_AUTO` the pipeline runs a chain whose summed
`cost` reaches `FX_COST_HEAVY` at 360x640 (2x2 point sampling in, pixel doubling out), a
quarter of the work. The saved JPEG is the same doubled frame, so capture still matches the
preview. Light chains stay at full resolution.

**Measured per-frame cost at full resolution** (fx stage only): scanline 17 ms, wave 29 ms,
blocks 39 ms, channel shift 92 ms, bit crush ~94 ms before the two-pixel rewrite, pixel sort
(rows) 122 ms. The camera delivers a frame every 52 ms (19 fps), so anything under ~45 ms keeps
the preview at full rate.

**Other lessons that cost hours:** the OV5647 mode table's "50 fps" is really 16 fps (frame
length 1732 lines x line length 2394 clocks at a 64 MHz pixel clock); frame length can go down
to ~1470 (the readout window is 1447 lines tall), line length cannot go below ~2250; the
sensor's built-in AEC does not adapt in this setup, so `pipeline/auto_exposure.c` drives
exposure and gain manually; gain and exposure registers must be written while streaming
(values written before stream-on produce black or saturated frames); the gain register is
linear in 1/16 steps (0x10 = 1x, 10 bits).
