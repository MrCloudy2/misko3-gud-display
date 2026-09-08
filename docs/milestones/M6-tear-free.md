# M6 — Tear-free output: measured

Run 2026-08-25 on the board.

**Verdict: accepted, with an honest caveat.** Tearing is substantially reduced
— the observer's words were "panel looks great, I notice very little tear but
can't really tell". The sync costs almost nothing; the tear-free write costs
+29% of blit time.

The caveat is stated plainly because it matters: the A/B comparison was not
conclusive enough to claim tearing is *eliminated*. See §5.

---

## 1. The geometry, which changed the whole design

CLAUDE.md's plan for M6 was: sync once with `GET_SCANLINE`, run a timer at
80.95 Hz, and start each blit at the top of vertical blanking, relying on the
blit being 3.9× faster than the scan-out.

That plan assumes the panel refreshes top to bottom and that a whole frame is
blitted in one go. Neither is true here.

**`GET_SCANLINE` counts 324 lines. The image is only 240 tall.** A counter that
exceeds 240 cannot be counting our rows. It is counting the 320-direction,
which is our image *x*.

That follows from the hardware. The ILI9341 drives a 240×320 portrait panel:
320 gate lines, each showing 240 RGB sources. `MADCTL = 0x60` sets MV
(row/column exchange), rotating it into landscape, so those 320 gate lines lie
**horizontally** across the screen. The panel refreshes column by column, left
to right — not top to bottom.

The consequence for M4/M5's write path is severe. With MV set, a write sweeps
the CASET direction first, and CASET is now the 320-axis, so **writing a single
image row touches every one of the 320 gate lines**. Every band write crosses
the entire scan range, and no choice of starting moment can keep the write
front ahead of the scan front. Waiting for vertical blanking would buy exactly
nothing.

Phase 1 measured the 0..323 range and did not draw this conclusion. It is the
single most important fact in this milestone.

## 2. What was built instead

**Column-major writes, issued in scan order, starting just ahead of the
refresh.**

For each gate line in turn: set a 1-pixel-wide window, write that column of the
band, move to the next. Start at the column the scan is about to reach and walk
forward, wrapping at the end of the rectangle.

- Columns ahead of the scan are rewritten long before it arrives — it sees new
  content.
- Columns behind it were read moments ago and will not be read again for a full
  frame, 12.48 ms away, while the whole rectangle takes under 1.1 ms.

So no column is ever read half-written, **and there is nothing to wait for**.
The cost is not idle time; it is one extra CASET/RASET/RAMWR sequence per
column.

The timer half of CLAUDE.md's plan survives and does real work: it is
`scan_line_now()`, which answers "where is the scan?" from the DWT cycle
counter instead of a bus read, so no `GET_SCANLINE` is needed per band.

## 3. Self-calibration

Nothing is taken from the datasheet. At boot, `scan_calibrate()` polls
`GET_SCANLINE` for half a second with the bus otherwise idle, takes the line
count from the highest value seen and the period from the interval between
wraps, timed with DWT.

Two consecutive boots:

```
scan-out measured: 324 lines, 12468 us/frame = 80.20 Hz, 6542 cycles/line
scan-out measured: 324 lines, 12484 us/frame = 80.10 Hz, 6550 cycles/line
```

Phase 1 measured 80.95 Hz. Three measurements spanning 80.10–80.95 Hz is
consistent with an RC oscillator inside the ILI9341 that nothing disciplines —
which is also why the phase is re-anchored against the panel every 100 ms
rather than free-running.

## 4. Cost

| Quantity | Value |
|---|---|
| Scan sync (re-anchor at 10 Hz) | **8–25 µs per second** |
| Blit, row-major (M5) | ~790 µs per rectangle |
| Blit, column-major (M6) | ~1019 µs per rectangle |
| Penalty for tear-free | **+29%** |
| Total CPU, cmatrix workload | 13% → 14% |
| fps, cmatrix workload | 11.00 → 11.00 |

The sync is effectively free. The whole cost is the extra window commands: 11
extra bus writes per column against 64 pixel writes.

Note the frame rate did not change at all on this workload, because that case
is limited by the wire (~600 kB/s), not by the blit. Tear-free is free in
frames here; it costs CPU headroom, of which there was plenty.

## 5. The honest caveat

The exit criterion was "visible tearing eliminated on a scrolling test pattern,
confirmed by me looking at the screen". What was actually reported was **"very
little tear but can't really tell"**, and the project was moved on to
optimisation work.

So this milestone is accepted as a clear improvement, not as a proven
elimination. What would settle it, if it is ever worth revisiting:

- A **device-generated** test pattern rather than a compositor one — a
  full-screen colour flipping every frame makes a tear seam unmistakable, and
  removes the host's variable frame pacing from the experiment.
- That would also confirm the **scan direction**. The current code assumes the
  gate-line counter increases in the same direction as image *x*. If it runs
  the other way, the writes land just *behind* the refresh instead of just
  ahead, and roughly the leading 22 columns would still tear — which is
  consistent with "very little tear". Reversing the loop is a one-line change,
  but there is no point making it without a test that can tell the difference.

The A/B toggle is still in the firmware (BTN_OK switches between column-major
and M5's row-major blit, logged as `TF` / `--`), so this can be picked up
later without rebuilding anything.

## 6. A robustness bug found and fixed

During the A/B test the firmware halted on a debugger breakpoint inside
TinyUSB's `usbd_edpt_xfer()`:

```
TU_ASSERT((_usbd_dev.ep_status[epnum][dir] & TU_EDPT_STATE_BUSY) == 0);
// Attempt to transfer on a busy endpoint, sound like a race condition!
```

**Cause, on the host side:** when a flush fails, `gud_flush_rect()` in
`gud_pipe.c` re-sends `GUD_REQ_SET_BUFFER` on the next attempt because
`prev_flush_failed` is set — without the previous bulk transfer ever having
completed. The device was then arming an endpoint that was still armed.

**Fix:** before arming, check `usbd_edpt_busy()`; if set, close and reopen the
endpoint (`usbd_edpt_close()` zeroes `ep_status`), discarding the abandoned
transfer. Discarding is correct — its data belongs to a rectangle the host has
already given up on. Occurrences are counted and reported as `rec N`.

Two things worth being precise about:

- **This bug has been present since M2**, not introduced by M6. It needed a
  failed flush to surface, and the column-major blit being 29% slower made that
  more likely.
- **It would not have crashed without a debugger attached.** `TU_BREAKPOINT()`
  reads `DHCSR` and only executes `BKPT` when a probe is connected; otherwise
  the assert returns false, the control transfer stalls, and the host retries.
  So it was a robustness hole rather than a field crash — but it would have
  looked like a mysterious hang during a demo with `probe-rs` running.

## 7. Gamepad naming

The gamepad is now `MiSKo3 Gamepad` in `/dev/input`, `js0` and Steam.

Getting that exact string is less obvious than it looks. `usbhid_probe()` in
`drivers/hid/usbhid/hid-core.c` builds the input device name by concatenating
the USB **device** manufacturer and product strings with a space, and ignores
`iInterface` entirely — so the "MiSKo3 Gamepad" string attached to the HID
interface descriptor never appears anywhere. The name has to be split across
the two device-level fields:

```c
"MiSKo3",     /* iManufacturer */
"Gamepad",    /* iProduct      */
```

The display half is unaffected: KDE names that output `USB-1` from the DRM
connector, not from any USB string.

## 8. RAM and flash

```
RAM:   89,208 B of 131,072  (68.1%)
FLASH: 21,772 B of 524,288  ( 4.2%)
```

## 9. Reproducing

```bash
cd m6_tearfree
cmake -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=arm-toolchain.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build
probe-rs run --chip STM32G474QE --speed 1000 build/m6_tearfree
```

Press BTN_OK to toggle tear-free mode; the console line starts with `TF` when
it is on and `--` when it is off.
