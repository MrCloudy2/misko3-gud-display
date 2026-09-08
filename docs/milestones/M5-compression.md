# M5 — Damage rectangles + LZ4: measured

Run 2026-08-25 on the board, kernel 7.2.0-1-cachyos, Wayland + KDE (KWin).

**Verdict: PASS. 5.8× faster than uncompressed on a scrolling terminal, with
zero decompression errors across the entire session.**

---

## 1. Exit criterion

> Measured fps for (a) a mostly-static desktop, (b) a scrolling terminal.

| Case | Ratio | fps | Wire | CPU | Limited by |
|---|---|---|---|---|---|
| **(a) Mostly-static desktop** (opaque terminal, idle) | **22.29×** | **5.00** | 34 kB/s | 4% | host repaint pacing |
| **(b) Scrolling terminal** (opaque, fullscreen) | **20.66×** | **29.00** | 215 kB/s | 24% | per-rectangle protocol overhead |

Against M4's uncompressed baseline of 5.00 fps, the scrolling terminal is
**5.8× faster**. Peak samples during the scroll:

```
[rate] 116 rect/s | wire 215 kB/s -> px 4454 kB/s (20.66x) = 29.00 full fps | dec 153906 us + blit  91749 us = 24% cpu | err 0/0
[rate]  88 rect/s | wire 152 kB/s -> px 3379 kB/s (22.10x) = 22.00 full fps | dec 111981 us + blit  69602 us = 18% cpu | err 0/0
[rate]  60 rect/s | wire  67 kB/s -> px 2304 kB/s (34.03x) = 15.00 full fps | dec  77753 us + blit  47456 us = 12% cpu | err 0/0
```

Static, same terminal, nothing scrolling:

```
[rate] 20 rect/s | wire 34 kB/s -> px 768 kB/s (22.29x) = 5.00 full fps | dec 26485 us + blit 15818 us = 4% cpu | err 0/0
```

`err 0/0` — decompression errors / total errors — held at zero for the whole
session, across every content type. The hand-written decoder from M1 is correct
against live output from the kernel's `LZ4_compress_default`, not just against
the M1 test corpus.

## 2. Every content type measured

All through the live pipeline, on real compositor output:

| Content | Ratio | fps | Wire | CPU |
|---|---|---|---|---|
| Black screen (`CONTROLLER_ENABLE` clear) | **238.5×** | 5.0 | 3 kB/s | ~0% |
| Opaque terminal, static | 22.3× | 5.0 | 34 kB/s | 4% |
| Opaque terminal, scrolling | 17–34× | 15–**29.0** | 67–215 kB/s | 12–24% |
| Static photographic wallpaper | 2.08× | 5.0 | 368 kB/s | 7% |
| Transparent terminal over wallpaper | 2.4–3.2× | 5.0–12.5 | 606 kB/s | 13% |
| cmatrix (dense animated glyphs) | 2.8× | 11.0 | 610 kB/s | 13% |

FINDINGS.md §5 predicted 240× for flat content and 26.8× for terminal text.
Measured on hardware: **238.5×** and **17–34×**. Both predictions hold. (The
9.3× in M1's corpus was pessimistic — that image was a dense `ls -la` listing
at a small point size, denser than a normal terminal.)

## 3. Two findings that change how the result should be read

### KWin never sends a partial damage rectangle

Across the entire session — thousands of transfers, six content types — the
only rectangles ever received were `320x48` and `320x64`. Those are the four
bands of a *full-screen* update. Not one narrower rectangle appeared, including
during a static desktop with the mouse pointer parked on another monitor.

**KWin damages the whole output on every commit for this device.** The device
honours `x/y/width/height` correctly and would blit a partial rectangle if it
got one; it simply never gets one.

So M5's gain is compression alone, not compression *plus* damage tracking. It
also means an unchanging screen is not free: a static photographic wallpaper
still costs 368 kB/s, because the host keeps resending all of it.

### One transparent window sets the compression floor for the whole panel

The first attempt at the scrolling-terminal measurement produced 2.43×, worse
than cmatrix, which made no sense for text. The cause was a terminal with
background transparency: the wallpaper showed through, so every "text" frame
was really a photograph with text drawn on it, and LZ4 saw a photograph.

Because the full screen is sent every frame, **one photographic layer anywhere
on the panel puts a floor under the ratio for everything on it.** Switching to
an opaque terminal took the same workload from 2.43× / 12.5 fps to
20.66× / 29 fps — a 5.8× swing from a window setting.

Practical consequence for the demo: on this output, use an opaque terminal and
a plain wallpaper. That is worth more than any device-side optimisation
available.

## 4. What limits each case

The three limits are now clearly separable, because all three are measured
independently every second:

- **Static content: the host paces it.** 5.00 fps at 4% CPU and 34 kB/s on a
  ~700 kB/s wire. Neither USB nor the G474 is close to its limit; KWin simply
  chooses to re-flush the unchanged screen five times a second.
- **Low-ratio content: the wire.** cmatrix and the transparent terminal both
  pin at ~610 kB/s regardless of what they contain.
- **High-ratio content: per-rectangle protocol overhead.** At 29 fps the wire
  carries only 215 kB/s and the CPU is at 24%, so neither is the constraint.
  116 rectangles per second is 8.6 ms each, of which decode is 1.33 ms, blit
  0.79 ms and the bulk transfer ~2.4 ms. The remaining ~4 ms is the
  `GUD_REQ_SET_BUFFER` control transfer that must precede every single
  rectangle — and a full-speed control transfer costs several 1 ms USB frames.

## 5. Why the remaining ~18% on low-ratio content was not chased

Under load the wire runs at 606 kB/s against M4's 768 kB/s. The gap is
decode+blit blocking USB service inside the completion callback (128 ms/s) plus
the extra control transfers from 44 rect/s instead of 20.

Recovering it means receiving band N+1 while decoding band N, which needs a
third 40 KB buffer. Only ~42 KB of RAM is free, so the bands would have to
shrink to 48 lines to make three fit — and M1 measured that splitting a frame
into four bands already costs between 5% and 46% of the compression ratio.
Five bands would cost more than the 18% it would buy, and §4 shows the ratio
matters more than the wire for the content this project is aimed at.

The arithmetic says leave it alone.

**The optimisation the data actually points at is the opposite one: larger
bands, not smaller.** Fewer, bigger rectangles would cut the per-rectangle
control-transfer overhead that limits the 29 fps case *and* improve the
compression ratio. That needs a bigger `max_buffer_size`, which needs the two
40 KB buffers to become one — possible with LZ4's in-place decompression
(placing the compressed block at the end of the output buffer with a
`(size >> 8) + 32` byte margin). Not attempted in M5; noted as the highest-value
remaining lever.

## 6. What changed from M4

Three edits, and nothing else:

1. `gud_device.c` — `desc.compression = GUD_COMPRESSION_LZ4` in the display
   descriptor. This is what makes the host call `LZ4_compress_default()` on
   every rectangle.
2. `gud_device.c` — `req_set_buffer()` now accepts compressed transfers and
   bounds-checks `compressed_length` against both `length` and
   `GUD_MAX_BUFFER_SIZE`. The host decides *per rectangle* whether compression
   helped, so both paths must be valid on every transfer.
3. `gud_usbd.c` — a second band buffer, and a decompress step in the bulk
   completion callback. Compressed transfers land in `gud_comp_buf` and are
   decoded into `gud_band_buf`; uncompressed ones are received straight into
   `gud_band_buf` so they never pay for a copy they do not need.

A block that decodes to any size other than the `length` the host promised is
treated as a failure and the rectangle is dropped — the host notices and
resends, rather than leaving a corrupt stripe on the panel.

## 7. RAM and flash

```
RAM:   89,152 B of 131,072  (68.0%)   -- 81,920 of it is the two band buffers
FLASH: 20,352 B of 524,288  ( 3.9%)
```

This matches M1's hardware-measured prediction of 88,664 B almost exactly. The
decompressor itself contributes 380 bytes of flash, 32 bytes of stack and no
static RAM.

## 8. Reproducing

```bash
cd m5_lz4
cmake -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=arm-toolchain.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build
probe-rs run --chip STM32G474QE --speed 1000 build/m5_lz4
```

For the scrolling-terminal measurement, the terminal must be **fullscreen and
opaque** on the panel or the number is meaningless — see §3:

```bash
yes "the quick brown fox jumps over the lazy dog 0123456789" | head -200000
```

To watch the console later without resetting the board (which would disconnect
the display mid-use), use `attach` rather than `run`.
