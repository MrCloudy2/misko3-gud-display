# M7 (optimisation) — In-place LZ4 decode, 120-line bands: measured

Run 2026-08-25 on the board.

**Verdict: PASS. 40 fps on a scrolling terminal, up from 29, using less RAM
than before and with zero errors.**

---

## 1. What changed and why

M5 established where the limits actually were, and they were different for
different content:

- Low-ratio content (photos, dense animation) is limited by **the wire**,
  ~615 kB/s.
- High-ratio content is limited by **per-rectangle protocol overhead** — at
  29 fps the wire carried only 215 kB/s and the CPU sat at 24%, but every one
  of the 116 rectangles per second needed its own `GUD_REQ_SET_BUFFER` control
  transfer, and a full-speed control transfer costs several 1 ms USB frames.

Both are attacked by the same change: **fewer, larger bands**. Fewer rectangles
means less control overhead, and larger bands compress better because LZ4
matches cannot cross a band boundary.

The obstacle was RAM. M5 needed two buffers of `max_buffer_size` — one for the
compressed bytes arriving, one for the decompressed pixels — because an LZ4
match can reach 64 KB backwards into output already produced, so a band cannot
be streamed. 2 × 40,960 = 81,920 of 131,072 bytes left no room to grow.

**In-place decompression removes the second buffer.** The compressed block is
received flush against the far end of the output buffer and decoded forwards
into the front of it.

## 2. Why in-place is safe here

Both pointers travel forwards; the write pointer must never overtake the read
pointer.

Per LZ4 sequence, the input advances by 1 (token) + 2 (offset) + literals + any
extended-length bytes, while the output advances by literals + match — and a
match is at least 4 bytes. So the output outruns the input by **at least one
byte per sequence, monotonically**. The gap is therefore narrowest at the very
end, where it equals exactly (decompressed − compressed). Starting the read
pointer that far ahead keeps it ahead throughout.

That argument holds only because this decoder copies exactly the number of
bytes it was asked for. The reference LZ4 decoder overshoots by up to 32 bytes
("wildcopy"), which is why upstream defines a margin of `(csize >> 8) + 32`. We
use the same margin anyway — 332 bytes — so the buffer stays correctly sized if
this is ever swapped for the reference implementation.

**Verified on the host before going near the device:** 20,000 in-place blocks
round-tripped byte-exact under AddressSanitizer with guard bands either side,
5,790 of them barely compressible, which is the case where the two pointers
come closest (`m1_lz4/host/fuzz_lz4.c`, part 3).

## 3. Why 120 lines

120 lines is 76,800 bytes — exactly half a frame, so a full-screen update
arrives as **two** rectangles instead of four.

It is also the *smallest* size that achieves that: 240 / 2 = 120. Anything
larger costs RAM without reducing the rectangle count further, and one
rectangle per frame would need 153,600 bytes against the part's 131,072.

Compression ratio measured on the M1 corpus at each band size:

| Content | 64 lines (4 bands) | **120 lines (2 bands)** | 240 (1 piece) |
|---|---|---|---|
| solid | 237.0× | 245.4× | 250.2× |
| flat_ui | 26.8× | **38.2×** | 39.2× |
| terminal_text | 9.3× | **11.4×** | 12.4× |
| gradient | 4.1× | 4.4× | 4.4× |

The band-splitting penalty collapses from +46%/+33% of the whole-frame ideal to
+2.7%/+9.2%. Nearly all of the loss recovered for half the rectangles.

## 4. Results on hardware

### Scrolling terminal (fullscreen, opaque)

```
[rate] TF 80 rect/s | wire 162 kB/s -> px 6144 kB/s (37.89x) = 40.00 full fps | dec 213928 + blit 153810 + sync 33 us = 36% cpu | err 0/0 rec 0
[rate] TF 79 rect/s | wire 159 kB/s -> px 6067 kB/s (37.93x) = 39.50 full fps | dec 211245 + blit 151888 + sync 33 us = 36% cpu | err 0/0 rec 0
```

| | M5 (2 buffers, 64 lines) | **M7 (in-place, 120 lines)** |
|---|---|---|
| Frame rate | 29.00 fps | **40.00 fps** (+38%) |
| Compression ratio | 20.66× | **37.89×** (+83%) |
| Bytes on the wire | 215 kB/s | **159 kB/s** |
| Pixels delivered | 4,454 kB/s | **6,144 kB/s** |
| Rectangles/s | 116 | **79** |
| CPU | 24% | 36% |

Note the wire carries *less* data for *more* frames.

### Dense animated content (cmatrix), same session

| | M6 | **M7** |
|---|---|---|
| Rectangles/s | 44 | **25** |
| Ratio | 2.80× | **3.16×** |
| Frame rate | 11.00 fps | **13.00 fps** (+18%) |
| CPU | 14% | 14% |

Here the gain is entirely the better compression ratio — this content is
wire-limited (~615 kB/s either way), so halving the rectangle count buys
nothing. That is the expected result and it is worth stating: the two halves of
this optimisation help different content.

### RAM

```
M6:  89,208 B of 131,072  (68.1%)   -- two 40,960 B buffers
M7:  84,424 B of 131,072  (64.4%)   -- one 77,132 B buffer
```

Nearly double the band size for 4,784 bytes *less* RAM.

## 5. Where the limit is now

At 40 fps: the wire carries 159 kB/s against a ~615 kB/s ceiling, and the CPU
is at 36%. Neither is the constraint.

79 rectangles per second is 12.6 ms each, of which decode is 2.67 ms, blit
1.92 ms and the bulk transfer about 2.6 ms — roughly 7.2 ms accounted for. The
remaining ~5.4 ms is still the `SET_BUFFER` control transfer and the host's own
per-rectangle work.

Going further would mean one rectangle per frame, which needs 153,600 bytes and
cannot be done on this part. The panel's own ceiling is 80.07 Hz, so 40 fps is
half the physical maximum for content that changes completely every frame — and
real desktop content changes far less than a `yes` loop.

## 6. The whole frame-rate story

| Milestone | Scrolling terminal |
|---|---|
| M4 — RGB565, uncompressed | 5.00 fps |
| M5 — LZ4, 64-line bands | 29.00 fps |
| **M7 — in-place, 120-line bands** | **40.00 fps** |

**8× over the uncompressed baseline**, on a part that cannot hold a single
frame of its own display.

## 7. Reproducing

```bash
cd m1_lz4/host && make fuzz          # proves in-place decode is safe
cd ../../m7_opt
cmake -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=arm-toolchain.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build
probe-rs run --chip STM32G474QE --speed 1000 build/m7_opt
```

Band-size comparison, for the table in §3:

```bash
cd m1_lz4/host
for BL in 64 120 240; do cc -O2 -w -DBANDL=$BL -o /tmp/gt gen_testdata.c ../src/lz4_dec.c -llz4 && /tmp/gt; done
```
