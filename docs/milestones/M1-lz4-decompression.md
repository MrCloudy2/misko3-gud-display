# M1 — LZ4 decompression on the STM32G474: measured

Run 2026-08-25 on the board (STM32G474QET6, 170 MHz, DWT cycle counter,
20 timed frames per image after one warm-up frame, no RTT traffic during a
measurement).

**Verdict: PASS. LZ4 decompression is not the bottleneck — USB still is.**

---

## 1. What was measured

For each test image the program does what the real system will do:

1. Copy one compressed band from flash into RAM (untimed — USB does this in the
   real system, and reading literals out of flash would measure the flash).
2. `lz4_decompress_block()` it into the band buffer (timed).
3. Blit that band to the panel over the FMC (timed separately).
4. Repeat for all four bands, then hash the result and compare against the
   hash the host computed before compressing.

A band is 64 lines = 40,960 bytes, so a 320×240 RGB565 frame is four bands of
40960 / 40960 / 40960 / 30720. That split is not arbitrary: it is exactly what
the Linux `gud` driver does once we declare `max_buffer_size`
(`gud_flush_damage()` in `drivers/gpu/drm/gud/gud_pipe.c` chops each damage
rectangle into `bulk_len / pitch` lines).

## 2. Correctness

The decompressor is hand-written (`src/lz4_dec.c`, 380 bytes of code), not
vendored, so it needed proving three separate ways:

| Check | Result |
|---|---|
| Host fuzz, valid blocks (`LZ4_compress_default` + `LZ4_compress_HC`, 5 data shapes, lengths 1 B – 70 KB) | **20,000 / 20,000 byte-exact** |
| Host fuzz, corrupt and truncated blocks with guard bands, under ASan + UBSan | **40,000 cases, 0 buffer overruns**, 27,210 correctly rejected |
| Undersized output buffer must be refused | 2,000 / 2,000 refused, no overrun |
| On hardware: FNV-1a of the decompressed frame vs the host's hash | **5 / 5 images OK** |

The corrupt-input half matters because this runs on a part with no MMU: a
one-byte overrun would silently corrupt a neighbouring variable. Every read of
the input and every write to the output is bounds-checked.

## 3. Decompression throughput

| Image | Compressed B/frame | Ratio | Decode ms/frame | In MB/s | Out MB/s |
|---|---|---|---|---|---|
| solid | 648 | 237.0× | 5.463 | 0.118 | 28.12 |
| flat_ui | 5,737 | 26.8× | 4.541 | 1.263 | 33.82 |
| terminal_text | 16,500 | 9.3× | 5.488 | 3.006 | 27.99 |
| gradient | 37,396 | 4.1× | 7.247 | 5.159 | 21.19 |
| noisy_gradient | 153,600 | 1.0× | n/a — host sends it uncompressed | — | — |

Jitter is zero to the microsecond (min = mean = max on every image): this is a
deterministic loop with no interrupts and no cache misses after the warm-up
frame.

**Output throughput is 21–34 MB/s.** That is the number that characterises the
decompressor, because the work LZ4 does is proportional to the bytes it
*produces*, not the bytes it consumes.

## 4. Decompress + blit together

| Image | Decode ms | Blit ms | Total ms | fps |
|---|---|---|---|---|
| solid | 5.463 | 3.615 | 9.078 | 110.2 |
| flat_ui | 4.541 | 3.615 | 8.156 | 122.6 |
| terminal_text | 5.488 | 3.615 | 9.103 | 109.8 |
| gradient | 7.247 | 3.615 | 10.863 | 92.1 |
| noisy_gradient | 0.000 | 3.615 | 3.615 | 276.6 |

Every case clears the panel's own 80.95 Hz refresh. The panel period is
12.353 ms and the worst case here is 10.863 ms, so decode + blit fits inside
one refresh with room to spare.

(The blit measures 3.615 ms here against phase 1's 3.162 ms. The difference is
the four `lcd_window()` command sequences a banded frame needs instead of one,
plus the fact that the source is now a RAM buffer being read while it is also
being written — 14% for a real banded write path is expected, not a regression.)

## 5. RAM footprint

| Item | Bytes |
|---|---|
| `comp_buf` — one compressed band as received | 40,960 |
| `band_buf` — that band decompressed, ready to blit | 40,960 |
| **Two band buffers** | **81,920** |
| Decompressor static RAM | 0 |
| Decompressor stack (measured with `-fstack-usage`) | 32 |
| Decompressor code (flash) | 380 |
| Whole M1 image RAM (`.data` + `.bss`, includes the 4 KB RTT buffer) | 88,664 of 131,072 (67.7%) |

Both buffers must be full band size. The kernel compresses with
`dstCapacity == srcSize`, so a compressed band is never *larger* than the band
— but it can be nearly that size, and when LZ4 cannot shrink the data at all
the host sends it raw at full size.

Decompression cannot be streamed straight to the FMC, because an LZ4 match may
reach up to 65,535 bytes backwards into output already produced — the decoder
must be able to re-read what it wrote. That is the constraint that sets
`max_buffer_size`, and 64 lines leaves ~42 KB for TinyUSB, the stack and
everything else.

## 6. What band-splitting costs

Measured on the host, banded vs. the same frame compressed in one piece:

| Image | Banded | 1 piece | Cost of splitting |
|---|---|---|---|
| solid | 648 | 614 | +5.5% |
| flat_ui | 5,737 | 3,918 | +46.4% |
| terminal_text | 16,500 | 12,384 | +33.2% |
| gradient | 37,396 | 35,080 | +6.6% |
| noisy_gradient | 153,600 | 153,600 | +0.0% |

Splitting costs up to 46% of the compression ratio, because matches cannot
cross a band boundary. Since USB is the bottleneck, that is a direct fps cost —
so a larger `max_buffer_size` is worth having if the RAM can be found. This is
the trade-off to revisit in M4/M5.

## 7. End-to-end estimate

At the measured USB rate of 0.67 MB/s, and assuming a band's USB transfer can
overlap the previous band's decode and blit:

| Image | USB ms/frame | decode+blit ms | limit | fps |
|---|---|---|---|---|
| solid | 0.97 | 9.08 | decode+blit | 80.95 (panel) |
| flat_ui | 8.56 | 8.16 | USB | 80.95 (panel) |
| terminal_text | 24.63 | 9.10 | **USB** | 40.6 |
| gradient | 55.82 | 10.86 | **USB** | 17.9 |
| noisy_gradient | 229.25 | 3.62 | **USB** | 4.4 |

USB dominates in every case that matters. Decompression costs 4.5–7.2 ms
against USB transfers of 8.6–229 ms.

## 8. Honest caveats

- **This corpus is more conservative than FINDINGS.md §5.** That table quotes
  26.8× for text and 14.1× for a gradient; the same content classes here give
  9.3× and 4.1×. The text image is now rendered with a real antialiased
  monospace font instead of synthetic glyph bitmaps, and part of the difference
  is the band splitting in §6. Treat these numbers as the pessimistic end.
- **No damage rectangles.** Every measurement above is a full-frame update. The
  real win on a desktop is that only the changed region is sent at all — a
  blinking cursor is a few hundred bytes. That is M5, and it is where the
  numbers should improve sharply.
- **Still synthetic.** None of these is a capture of the actual desktop.
  `make_images.py` accepts a PNG on the command line specifically so a real
  screenshot can be added to the corpus and the whole measurement re-run.
- **Highly compressible content decodes more slowly per output byte than it
  should.** `solid` (237×) takes 5.46 ms while `flat_ui` (26.8×) takes 4.54 ms.
  Long runs are encoded as matches with offset 1 or 2, which overlap their own
  output and so must be copied one byte at a time. Widening that case would
  claw back roughly 2 ms/frame. Not needed today — 9.08 ms already fits inside
  the 12.353 ms panel period — but it is the obvious lever if a later milestone
  runs short of CPU.

## 9. Reproducing

```bash
cd m1_lz4/host && python3 make_images.py && make      # fuzz + regenerate corpus
cd .. && cmake -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=arm-toolchain.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build
probe-rs run --chip STM32G474QE --speed 1000 build/m1_lz4
```
