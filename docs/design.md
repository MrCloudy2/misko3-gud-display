# Design notes

Why the firmware is built the way it is. None of this is needed to use the
board; see the [README](../README.md) for that.

## The two numbers that shaped everything

Both were measured before any architecture was chosen.

**A frame does not fit in RAM.** 320 × 240 in RGB565 needs 153,600 bytes. The
STM32G474QET6 has 131,072 bytes in total. A framebuffer is not possible, even
with the memory otherwise empty, so the image has to stream through the device
in bands.

**The cable is slower than the panel.** USB bulk measured 0.768 MB/s; the FMC
parallel bus to the panel measured 48.56 MB/s. A ratio of 63 : 1. With no
compression the panel was busy 1.6 % of the time and USB 98.4 %, so making the
device draw faster achieves nothing. Only sending fewer bytes helps.

## The path a pixel takes

```
KWin damage rect  ->  kernel splits into bands, LZ4-compresses each
                  ->  SET_BUFFER on EP0, then the bytes on bulk EP1
                  ->  in-place LZ4 decode
                  ->  column-ordered write over the FMC  ->  ILI9341
```

The device presents one USB address with two interfaces: interface 0 is class
`0xFF` with a bulk OUT endpoint for pixels, interface 1 is class `0x03` (HID)
with an interrupt IN endpoint for the buttons and stick.

## Four decisions

**Banded transfer.** The descriptor declares `max_buffer_size = 76,800`, which
is 120 lines, exactly half a frame. The kernel splits every damage rectangle by
that number, so there is no splitting code on the device.

**LZ4.** Enabled by one bit in the display descriptor. The host compresses; the
device only decompresses. The decoder is written from the format specification
and bounds-checks every input read and every output write, because the data
arrives over a wire and the part has no MMU.

**In-place decompression.** Two separate buffers would need 153,600 bytes and
do not fit. The compressed block is received flush against the far end of the
output buffer and decoded forwards into its front. This works because every LZ4
sequence produces at least as many bytes as it consumes: a match is at least
four bytes and is written in three. It bought bands twice as large for *less*
RAM than the two-buffer version used.

**Column-ordered writes.** `GET_SCANLINE` reports a counter that reaches 323
while the image is only 240 tall, which means it counts the 320 direction:
after the `MADCTL` rotation the panel refreshes **column by column**, not top to
bottom. Writing row-major therefore crosses the whole scan range and no
starting moment avoids tearing. Writing one column at a time, in the scan's own
direction, does: a column takes 5.4 µs against the beam's 38.1 µs, so it never
catches up, and nothing has to wait.

## Results

Measured on hardware with the Cortex-M4 cycle counter. Counters accumulate in
RAM and are printed once a second with the debug bus idle, so the reporting
does not distort what is being measured. Same content throughout: scrolling
text in a terminal.

| | frames/s | ratio | on the wire | pixels delivered | CPU |
|---|---:|---:|---:|---:|---:|
| uncompressed | 5.00 | 1.0× | 768 kB/s | 768 kB/s | 2 % |
| LZ4, 64-line bands | 29.00 | 20.7× | 215 kB/s | 4,454 kB/s | 24 % |
| **in-place, 120-line bands** | **40.00** | **37.9×** | **159 kB/s** | **6,144 kB/s** | 36 % |

Eight times faster while pushing **less** data down the cable than at the start.
Zero decompression errors across the whole test session.

By content, at the final version:

| content | ratio | frames/s |
|---|---:|---:|
| black screen | 238.5× | 5.0 |
| terminal, scrolling text | 37.9× | 40.0 |
| cmatrix | 3.2× | 13.0 |
| photograph | 2.1× | 5.0 |

## Known limits

A horizontal seam sits at y = 120 because the two bands arrive about 25 ms
apart while the panel refreshes every 12.4 ms. Removing it means holding the
first band compressed until the second arrives and drawing both back to back,
which costs one frame of latency.

Photographic content does not compress, so it stays at 5 fps. That is the
ceiling of full-speed USB on this part, not a defect.

## Per-stage measurements

Each development stage has its own record with the numbers that justified the
next one:

- [M1 LZ4 decompression](milestones/M1-lz4-decompression.md)
- [M2 GUD enumeration](milestones/M2-gud-enumeration.md)
- [M3 composite device](milestones/M3-composite-device.md)
- [M4 first pixels](milestones/M4-first-pixels.md)
- [M5 compression](milestones/M5-compression.md)
- [M6 tear-free output](milestones/M6-tear-free.md)
- [M7 optimisation](milestones/M7-optimisation.md)

The feasibility study that preceded all of it is in [FINDINGS.md](FINDINGS.md).
