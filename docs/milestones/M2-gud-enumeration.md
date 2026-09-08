# M2 — GUD enumeration: measured

Run 2026-08-25 on the board, kernel 7.2.0-1-cachyos, Wayland + KDE.

**Verdict: PASS, first attempt, no kernel errors.** The `gud` driver binds, a
DRM card appears, and KDE has already adopted the board as a real output.

---

## 1. Kernel messages

```
usb 1-14.2: new full-speed USB device number 43 using xhci_hcd
usb 1-14.2: New USB device found, idVendor=1209, idProduct=4fb3, bcdDevice= 1.00
usb 1-14.2: New USB device strings: Mfr=1, Product=2, SerialNumber=3
usb 1-14.2: Product: MiSKo3 GUD Display
usb 1-14.2: Manufacturer: FE Ljubljana
usb 1-14.2: SerialNumber: 003600244D41501520343639
[drm] Initialized gud 1.0.0 for 1-14.2:1.0 on minor 2
gud 1-14.2:1.0: [drm] fb1: guddrmfb frame buffer device
usbcore: registered new interface driver gud
```

Nothing else was logged. No errors, no retries, no timeouts.

The serial number is read from the STM32's 96-bit factory unique ID at
`0x1FFF7590`, so two boards on one machine cannot collide.

## 2. The DRM card

`/sys/class/drm/` gained `card2` and `card2-USB-1`:

| Path | Value |
|---|---|
| `card2/device/driver` | `/sys/bus/usb/drivers/gud` |
| `card2/device/uevent` → `INTERFACE` | `255/0/0` — vendor class, as the driver's table requires |
| `card2/device/uevent` → `MODALIAS` | `usb:v1209p4FB3d0100dc00dsc00dp00icFFisc00ip00in00` |
| `card2-USB-1/status` | `connected` |
| `card2-USB-1/enabled` | `enabled` |
| `card2-USB-1/modes` | `320x240` |
| `card2-USB-1/dpms` | `On` |

## 3. KDE sees it as a display

`kscreen-doctor -o`:

```
Output: 1 USB-1 328fdee8-eef2-4b76-9f72-952b9595e0b3
	enabled
	connected
	priority 3
	Modes:  1:320x240@60.00*!
	Geometry: 4480,0 320x240
	Scale: 1
```

This is more than M2's exit criterion asked for. The compositor has placed the
panel in the desktop at x=4480 and is already sending it pixels — it just has
nowhere to put them yet.

## 4. The device's own view

RTT console during the same run:

```
=== MiSKo3 M2: GUD display enumeration ===
SYSCLK 170 MHz | USB clock = HSI48 + CRS (SOF-disciplined)
mode 320x240 RGB565, max_buffer_size 40960 B (64 lines)
CRS->CR=00004060 CRS->CFGR=2022BB7F CLK48SEL=0
tusb_init() done, waiting for the host...
[usb] MOUNTED (CRS ISR=001A020D)
[gud] state check: 320x240 format 0x40
[gud] controller ENABLE
[gud] state commit
[gud] display ON
...
[gud] buffers=364 bytes=13977600 errors=0 | last rect 320x48 at 0,192 | ctrl=1 disp=1
```

- `CRS->CFGR = 2022BB7F` — RELOAD 47999, FELIM 34, SYNCSRC 10 (USB SOF).
- `CRS->CR = 00004060` — CEN and AUTOTRIMEN set: HSI48 is being disciplined.
- `CRS->ISR = 001A020D` after mount — the SOF sync is arriving and the trim is
  live, which is what makes crystal-less USB in-spec rather than merely lucky.
- Format `0x40` is `GUD_PIXEL_FORMAT_RGB565`, so the host committed the format
  we advertised.
- **364 bulk transfers, 13,977,600 bytes, 0 errors** over 47 seconds.

The rectangles arriving are `320x64 at 0,0`, `320x64 at 0,64`, `320x64 at
0,128`, `320x48 at 0,192` — exactly the four-band split predicted from
`max_buffer_size = 40960` and a 640-byte line pitch. The host's band logic is
doing what M1 assumed it would.

## 5. Bonus observation: bulk throughput

Not an M2 criterion, and not measured properly on the device yet, but the
byte counter gives a preview. The fastest sustained one-second windows were:

| Window | Bytes | Rate |
|---|---|---|
| 0.927 s | 808,960 | 0.873 MB/s |
| 0.927 s | 798,720 | 0.862 MB/s |
| 1.030 s | 768,000 | 0.746 MB/s |

**Treat these as indicative only.** The window boundaries come from a 1 ms
software tick sampled once a second, so a few percent of the spread is
quantisation, and the compositor's traffic is bursty rather than continuous —
the 47-second average is much lower simply because the desktop was idle for
part of it. What can be said is that the real GUD bulk path is delivering at
least as much as phase 2's CDC measurement of 0.75 MB/s, and possibly a little
more now that no CDC or HID interface competes for the bus. M4 times it on the
device with the DWT counter and settles it.

At 0.75 MB/s, an uncompressed 153,600-byte frame takes 205 ms — about 4.9 fps,
consistent with FINDINGS.md's predicted 4.4.

## 6. Design decisions worth defending

**A custom TinyUSB class driver, not the built-in vendor class.** The built-in
class copies bulk data through its own FIFO, which would cost a third 40 KB
buffer this part does not have, and it offers no way to say "the next bulk
transfer is exactly N bytes and belongs in this buffer" — which is precisely
what `GUD_REQ_SET_BUFFER` announces. Registering through
`usbd_app_driver_get_cb()` gives direct control of the endpoint. `gud-pico`
takes the same approach.

**Control and bulk arrive by different routes.** TinyUSB hands *every*
vendor-type control request to `tud_vendor_control_xfer_cb()` regardless of
which interface it names (`process_setup_received()` in `usbd.c`), so that is
the hook, and the class driver's own `control_xfer_cb` is never used. The class
driver only handles the endpoint.

**`max_buffer_size = 40960` (64 lines).** Two buffers of this size are needed
in RAM — one for bytes as they arrive, one for decompressed pixels, because an
LZ4 match can reach 64 KB backwards into output already produced and therefore
cannot be streamed. 2 × 40,960 = 81,920 of 131,072 bytes. M1 measured that
budget on hardware. Larger bands would compress better (M1 measured 5–46% lost
to splitting) but there is no RAM for them.

**Bulk data is received and discarded rather than ignored.** With the endpoint
unarmed the host would push for three seconds, time out, log an error and
retry forever, burying the one kernel message this milestone is trying to read.

**RTT no longer blocks when no debugger is attached.** `rtt_write_char()` now
only spins while `RdOff` is non-zero, i.e. while something has demonstrably
drained the buffer. Console writes happen inside USB callbacks, and the old
50-million-iteration spin would have stalled enumeration on any machine with no
debugger connected. Verified: the board stays enumerated and error-free with
`probe-rs` detached.

## 7. RAM and flash

```
RAM:   48,064 B of 131,072  (36.7%)   -- 40,960 of it is the band buffer
FLASH: 15,256 B of 524,288  ( 2.9%)
```

## 8. Reproducing

```bash
cd m2_gud
cmake -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=arm-toolchain.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build
probe-rs run --chip STM32G474QE --speed 1000 build/m2_gud
```

Then, on the host:

```bash
journalctl -k -n 20
ls /sys/class/drm/
kscreen-doctor -o
```
