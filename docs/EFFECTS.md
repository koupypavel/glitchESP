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
    const fx_param_t *params;          // each: id, label, min, max, default, step
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
chain ping-pongs between a temp buffer and the output. The order is counted back from the end
so that the last enabled effect lands in `out`: with two effects it is temp, out; with three
it is out, temp, out. (An earlier version counted from the front and ran the third effect in
place, which quietly wrecked every three-effect chain.) With nothing enabled the input is
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

### Psychedelic set

These follow the named open-eye phenomena in the psychedelic-replication literature
(Wikipedia "Psychedelic replication", PsychonautWiki "Visual effects").

**Tracers** (`fx_tracers.c`, phenomenon: tracers / after images). Temporal: the frame is
combined with a decayed copy of what the effect produced for the previous frame. "Echo" mode keeps the brighter of the
live pixel and the faded old one, so the live frame stays crisp and older copies fade behind
it; "blend" mode cross-fades for smooth ghosting. "Rainbow" fades the three channels at
slowly cycling rates so trails drift through hues. Its history is its *own* previous output:
the chain copies what tracers produced into `ctx->keep` before any later effect runs, and
hands it back as `ctx->prev` on the next frame. If the chain's final output were fed back
instead, every effect after tracers would be applied to the trail again on each frame, and
hue rotation, contrast and displacement would compound until the picture dissolves.

**Hue drift** (`fx_hueshift.c`, phenomena: colour shifting, colour enhancement). A hue
rotation matrix combined with saturation and contrast, folded into a 64K-entry RGB565 lookup
table in internal RAM that is rebuilt only when the angle has drifted a few degrees. Per
pixel it is a single table load, so it is one of the cheapest effects.

**Kaleido** (`fx_kaleido.c`, phenomena: symmetrical texture repetition, recursion). Mirrors
one side of the frame onto the other (2-way) and optionally the top onto the bottom (4-way);
a slow sweep moves the axis so the symmetry breathes.

**Diffraction** (`fx_diffract.c`, phenomenon: diffraction, "rainbows and spectrums of colour
embedded within the brighter parts of the visual field"). Pixels above a brightness threshold
feed a running decay swept along each row in both directions; each colour channel gets its own
delay and decay (blue short and immediate, green medium, red long and offset), so the halo is
ordered like a spectrum. Cost O(1) per pixel; ~3 KB of row buffers on the stack.

**Drift** (`fx_drift.c`, phenomena: flowing, morphing). A displacement field made of several
sine waves with unrelated frequencies and phase speeds, plus a slower cross-term for
"morph", so the deformation never repeats. The field is evaluated every 16 pixels and
interpolated in fixed point; per pixel it is two adds, a clamp and one load.

**Breathe** (same file, phenomenon: breathing). The same sampler with a radial zoom about the
centre that oscillates between normal size and `depth` percent expanded, at `rate` breaths
per minute (assuming the 19 fps preview), with a small wobble. It only expands, so the
sampler never leaves the frame.

### Media damage set

**VHS** (`fx_vhs.c`). What a worn tape does. Tape stores colour with far less bandwidth than
brightness, so colour smears to the right: green, which carries most of the brightness, is
kept sharp while red and blue go through a one-pole low-pass along each row. On top of that
the rows wander sideways on a slow sine, a noisy "tracking bar" drifts up the picture, the
bottom rows are skewed where the heads switch, white dashes are scattered as snow, and
every other row is slightly darker.

**Slit scan** (`fx_slitscan.c`). Temporal. A narrow slit sweeps across the picture and only
the part under it is refreshed from the camera; the rest stays as it was when the slit last
passed. The picture is therefore assembled from many moments, and anything that moves
while the slit crosses it comes out stretched or bent. The slit position follows from the
frame number, so the effect keeps no state of its own beyond the history frame. Because
there is a single history buffer, it cannot run together with Tracers (the UI refuses).

**Databend** (`fx_databend.c`). The look of a JPEG with damaged bytes. A JPEG stores each
block's brightness and colour as the difference from the previous block, so one bad byte
shifts everything after it sideways and offsets its colours until the next restart marker.
The effect draws that result directly: a few "breaks" are placed along the block stream and
each changes the running shift and colour offset for what follows, occasionally smearing a
stuck block across the row. This is an imitation, not real file corruption: it is fast,
repeatable from the seed and works at any frame size, which a real encode, corrupt and
decode round trip would not be.

**Palette** (`fx_palette.c`). A camcorder tape played through an old graphics card: the
picture is redrawn with four to six fixed colours and an ordered dither in horizontal
lines. Seven palettes: acid (magenta, cyan, lime, pink on dark), crimson and lime with
steel blue, CGA, Game Boy green, vapour, ember, and plain ink (black and white, where the
dither does all the work). Every pixel goes to the nearest palette colour; the dither adds
a per-pixel offset from a 4×4 pattern before the match, so flat areas break into lines of
the two nearest colours. "Hue" turns the picture before matching, which decides what
colour the sky or a face ends up as: since a hue rotation keeps distances, the palette is
turned the other way when the table is built instead of turning every pixel. The pattern
cell ("size", 2 by default) is in reference pixels, so the lines are as tall in a photo as
on the preview. The match is a 4096-entry table reached through per-channel tables that
already hold the dither offset: four loads per pixel. The amount knob walks through the
palettes.

### Painter's set

**Squint** (`fx_squint.c`). What a painter does to judge a scene: half-close the eyes so the
detail goes and only the big shapes of light and shadow remain. The frame is averaged into
a grid of cells (half the blur size), the grid is softened with its neighbours, and every
output pixel is interpolated from the four nearest cells, which together is close to a
Gaussian blur for the price of one pass. Each cell carries its brightness next to its
colour; the interpolated brightness is snapped to one of `values` steps (with a soft ramp
between them) and the colour shifted by the same amount, so the shapes keep a hint of
colour and `color` 0 gives a pure value study. The steps are spread between the 3rd and
97th percentile of the picture, so a dim room and a bright street both split into the same
number of shapes. This is the first effect that needs the whole frame before it can draw
any row: it uses the `analyze()` hook (once per frame, with the input) and runs the averaging
on both cores with `fx_rows_parallel()`. The grid lives in PSRAM, which the two cores do not
see coherently, hence `fx_mem_publish()` / `fx_mem_fetch()` around it. Everything per pixel
happens once per 2x2 block; the blur hides that. It also declares `FX_COST_SOFT`, so the
preview always runs it at half resolution: full resolution adds nothing to a blur.

**Van Gogh** (`fx_vangogh.c`). The picture redrawn in short, thick brush strokes. Not a
neural style transfer but a painter's recipe: one dab per small cell in that cell's average
colour (pushed, and every dab a little off its neighbours), longer than wide, laid along
the edges of what it paints; where the picture is flat the strokes fall into slow swirls
around a few centres, the Starry Night sky. Each stroke has a lit flank, a shaded flank
and a dark rim, which reads as thick paint. `analyze()` averages the frame into cells on
both cores and turns each into a stroke record (centre, direction from the brightness
gradient blended with the swirl field, four ready shades); `apply()` paints the strokes
as overlapping ellipses row by row, in two layers, solving for the span where the row
cuts each ellipse. The stroke positions come from the seed alone, so the paint does not
boil; the swirl centres drift with the frame number, which makes flat areas turn slowly.
104 ms (9 fps) at the default stroke size, 80 ms (12 fps) with large strokes; a first
version that searched the nearest stroke for every pixel took 312 ms.

### Night set

Made for dark scenes with a few bright lights, where most other effects have little to
work with.

**Starburst** (`fx_starburst.c`). The star filter: every light sends out four or eight
rays in its own colour. `analyze()` marks the highlights on a grid (one cell per 6 pixels),
then drags them into streaks with a decaying maximum along each ray direction. All
directions are done in two passes down and up the grid, a row at a time, working on copies
of the row in on-chip memory; cells with neither a ray nor a light are skipped four at a
time, so a night scene costs much less than a bright room. `apply()` interpolates the ray
map and adds it, copying rows no ray reaches. Measured in a bright room, half-resolution
preview: 90 ms per frame with eight rays (9 fps), the worst case.

**Light trails** (`fx_lighttrails.c`). Temporal. Like Tracers, but only light leaves a
trail: the faded previous output is kept where it is brighter than the live picture and
above a threshold, so the dark parts of the scene stay sharp and current. The trail's
blue fades first and its red last, so it cools from white to orange to red. 63 ms (12 fps).

**Neon** (`fx_neon.c`). Edges glow in a slowly turning rainbow over the darkened picture.
Edge strength is the brightness difference across two pixels horizontally plus
vertically; the three rows of brightness needed per output row are kept in a small ring
so each input row is converted once. 74 ms (11 fps).

Starburst and Neon declare `FX_COST_SOFT` like Squint: preview at half resolution, photos
at full.

Not yet built: visual haze/glow (blend with a blurred low-resolution copy), environmental
orbism (radial block displacement), melting (feedback warp that accumulates).

## 7. Adding an effect (checklist)

1. Copy `fx_bitcrush.c` to `fx_yourname.c`. Fill the parameter table, `from_amount()`, `apply()`.
   The table also drives the on-screen parameter editor: `step` 0 gives a slider (whole
   numbers when the range is 20 or more), `step` 1 with a 0..1 range gives an on/off switch,
   `step` 1 otherwise a slider in whole steps. Keep the labels short.
   Honour `ctx->y0`/`ctx->y1` and seed randomness per band/row with `fx_rng_init_at()`; then
   set `row_parallel = true` so both cores share the work. Per-frame tables go in `prepare()`;
   anything that must look at the whole input first goes in `analyze()` (see `fx_squint.c`,
   also for spreading such work over both cores and for sharing PSRAM data between them).
   Parameters measured in pixels (shifts, band heights, tile sizes, wavelengths) are defined
   for a 720-pixel-wide frame: pass them through `fx_px(in, value, min)` or multiply by
   `fx_scale(in)`. The same chain runs on 360-wide preview frames, 720-wide full frames and
   1088-wide stills, and this is what makes it look the same on all three.
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
