# M4 — First pixels, uncompressed: measured

Run 2026-08-25 on the board, kernel 7.2.0-1-cachyos, Wayland + KDE.

**Verdict: PASS. The board works as a monitor.** Confirmed by eye: the
compositor's image is visible, windows dragged onto it appear and behave, and
the colours are correct.

---

## 1. Exit criterion

> A real image from the compositor is visible on the panel. Measure and report
> achieved fps (expect ~4.4).

Both halves met. Visual confirmation from the person looking at the panel:
image visible, windows draggable onto it, colours correct. Two observations
from the same look, both expected and both already on the plan:

- **Frame rate is the thing to improve** — that is M5 (LZ4).
- **Tearing on vertical sweeps** — that is M6. Nothing in M4 synchronises the
  blit to the panel's scan-out, so a write front crossing the scan front is
  exactly what should be visible.

## 2. Measured rate

Sustained, with the whole output repainting continuously:

```
[rate] 20 rect/s 768 kB/s = 5.00 full fps | blit 15903 us/s (1%) | last 320x48 at 0,192 | err 0
[rate] 20 rect/s 768 kB/s = 5.00 full fps | blit 15903 us/s (1%) | last 320x48 at 0,192 | err 0
[rate] 20 rect/s 768 kB/s = 5.00 full fps | blit 15904 us/s (1%) | last 320x48 at 0,192 | err 0
```

29 of 41 one-second samples read exactly `5.00 full fps`; the remainder are
ramp-up and wind-down. 20 rectangles per second is 5 frames of 4 bands, which
is the four-band split predicted by `max_buffer_size = 40960` against a
640-byte line pitch.

| Quantity | Measured | Predicted |
|---|---|---|
| Bulk throughput | **0.768 MB/s** | 0.67 MB/s (FINDINGS §4) |
| Full-frame rate, RGB565 uncompressed | **5.00 fps** | 4.4 fps (FINDINGS §7) |
| Blit time per full frame | **3.18 ms** | 3.162 ms (phase 1) |
| Blit share of wall clock | **1.59%** | — |
| Errors, device and kernel | **0** | — |

Two independent corroborations fall out of this:

- The 3.18 ms blit reproduces phase 1's 3.162 ms on a completely different code
  path — there, a synthetic tile in a tight loop; here, real compositor pixels
  arriving over USB in four bands with a `lcd_window()` between each.
- 0.768 MB/s beats phase 2's 0.67 MB/s composite figure by 15%. That is
  expected rather than surprising: a dedicated bulk OUT endpoint armed for a
  whole 40,960-byte band has far less per-transfer overhead than CDC's
  64-byte FIFO plumbing, and no CDC interface is competing.

## 3. The number that decides M5

**The blit is 1.59% of the wall clock. USB is the other 98.4%.**

Per full frame: 3.18 ms blitting, ~196 ms waiting for bytes. Compressing what
goes over the wire is the only thing that can help, and there is an enormous
amount of CPU sitting idle to do it with — M1 measured LZ4 decode at
4.5–7.2 ms per frame, which drops into that 196 ms without touching the
budget.

## 4. Idle costs nothing

```
[rate] 20 rect/s 768 kB/s = 5.00 full fps | blit 15903 us/s (1%) | err 0
[rate] idle - nothing on screen is changing
[rate] 8 rect/s 307 kB/s = 2.00 full fps  | blit 6361 us/s (0%)  | err 0
[rate] idle - nothing on screen is changing
```

Both ends of the range are measured: saturated is 5.00 fps, idle is exactly
zero bytes. This confirms damage-driven updating is already working — see §5.

## 5. Damage rectangles already work

GUD only sends whole frames if the device asks for it by setting
`GUD_DISPLAY_FLAG_FULL_UPDATE` in the display descriptor. We deliberately do
not, so the host sends damage rectangles from the very first frame, and
`gud_panel_write_buffer()` honours `x`, `y`, `width` and `height` by programming
them into the ILI9341's CASET/RASET window before blitting. Partial-width
updates work because the controller wraps within the window it was given.

So M5's remaining work is genuinely just LZ4, not damage tracking.

## 6. Reception and display are strictly serial

The blit runs inside the bulk endpoint's completion callback, so no USB
transfer is serviced while it is in progress. That is a real cost and it is
visible in the arithmetic: the theoretical ceiling would be
`1 / (196 ms + 3.18 ms)` versus `1 / max(196, 3.18)`, a difference of 1.6%.

Overlapping the two would need a second 40 KB band buffer to receive into while
the first is being blitted, and at 40,960 bytes a band there is no RAM for one
— 3 × 40,960 with compression would be 122,880 of 131,072 bytes. Since the
cost is 1.6% and the RAM cost is the whole budget, this stays as it is. Noted
in case M6 changes the arithmetic.

## 7. Design decisions

**The panel comes up before USB, showing colour bars.** A blank panel from that
point on means the display path is broken, not that the host has yet to
connect. Colour bars also make a wrong `MADCTL` or a crossed data line obvious
at a glance — a check that paid for itself immediately, since the colours were
confirmed correct by eye on the first run.

**`MADCTL = 0x60`** sets MV (row/column exchange) and MX (column order
reversed), rotating the panel into landscape so the addressable area is 320×240
— matching the mode advertised over GUD.

**DPMS drives the backlight on PB6, not the ILI9341's DISPOFF.** DISPOFF blanks
the controller while leaving the backlight burning, which looks like a white
rectangle rather than an off display.

**No `__DSB()` in the blit loop.** Phase 1 measured the stock driver's
per-write barrier at 9 HCLK against 7 without — 22% slower for no benefit,
since ordering between consecutive writes to the same device is already
guaranteed.

**`CONTROLLER_ENABLE` clears the screen to black** so the first real frame does
not appear over the boot splash or a stale desktop image.

## 8. RAM and flash

```
RAM:   48,168 B of 131,072  (36.8%)   -- 40,960 of it is the band buffer
FLASH: 19,756 B of 524,288  ( 3.8%)
```

## 9. Reproducing

```bash
cd m4_pixels
cmake -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=arm-toolchain.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build
probe-rs run --chip STM32G474QE --speed 1000 build/m4_pixels
```

To watch the console again later without resetting the board — which would
disconnect it from the desktop mid-use — use `attach` rather than `run`:

```bash
probe-rs attach --chip STM32G474QE --speed 1000 build/m4_pixels
```
