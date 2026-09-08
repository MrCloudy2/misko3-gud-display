# M3 — GUD display + HID gamepad on one cable: measured

Run 2026-08-25 on the board, kernel 7.2.0-1-cachyos.

**Verdict: PASS. Both functions bind at once, all seven buttons verified on
hardware, zero kernel errors.**

---

## 1. Both drivers bind

```
usb 1-14.2: New USB device found, idVendor=1209, idProduct=4fb3, bcdDevice= 1.00
usb 1-14.2: Product: MiSKo3 Display + Gamepad
usb 1-14.2: Manufacturer: FE Ljubljana
usb 1-14.2: SerialNumber: 003600244D41501520343639
[drm] Initialized gud 1.0.0 for 1-14.2:1.0 on minor 2
gud 1-14.2:1.0: [drm] fb1: guddrmfb frame buffer device
input: FE Ljubljana MiSKo3 Display + Gamepad as .../1-14.2:1.1/0003:1209:4FB3.0020/input/input82
hid-generic 0003:1209:4FB3.0020: input,hidraw6: USB HID v1.11 Gamepad
    [FE Ljubljana MiSKo3 Display + Gamepad] on usb-0000:00:14.0-14.2/input1
```

Note the interface suffixes: `1-14.2:1.0` went to `gud`, `1-14.2:1.1` went to
`hid-generic`. One device, one cable, two drivers.

Resulting nodes: `/sys/class/drm/card2`, `card2-USB-1`, `/dev/input/js0`,
`/dev/input/event24`, `/dev/hidraw6`.

## 2. All seven buttons

Pressed by hand, observed simultaneously on the host (`/dev/input/js0`) and on
the device's own RTT console.

| Button | Pin | Device reports | Host reports |
|---|---|---|---|
| BTN_OK | PC15 | `buttons=00000001` | `BUTTON 0 PRESS` |
| BTN_ESC | PC14 | `buttons=00000002` | `BUTTON 1 PRESS` |
| JOY_BTN | PC13 | `buttons=00000004` | `BUTTON 2 PRESS` |
| UP | PG0 | `hat=1` | `ABS_HAT0Y = -32767` |
| RIGHT | PG8 | `hat=3` | `ABS_HAT0X = +32767` |
| DOWN | PG1 | `hat=5` | `ABS_HAT0X`… `ABS_HAT0Y = +32767` |
| LEFT | PG6 | `hat=7` | `ABS_HAT0X = -32767` |

**7 / 7.** The hat values are TinyUSB's 8-way encoding (0 centred, 1 up, 3
right, 5 down, 7 left), so the four direction switches are being collapsed
correctly before transmission.

## 3. The display keeps working while the gamepad talks

```
[gud] buffers=20 bytes=768000 errors=0 | last rect 320x48 at 0,192 | ctrl=1 disp=1
```

Zero GUD errors across the run, and zero kernel messages matching
`error|fail|timeout|reset`. `card2-USB-1` and `js0` were both still present
afterwards.

The bulk counter stops advancing once the compositor goes idle. That is correct
behaviour, not a stall: GUD is damage-driven, so a display whose contents are
not changing receives nothing. It resumes the moment something repaints.

## 4. The design decision this milestone existed to test

CLAUDE.md suggested carrying over phase 2's approach —
`bDeviceClass = Misc/Common/IAD (0xEF/0x02/0x01)` — and confirming it holds for
vendor+HID. **It was not used, deliberately, and the milestone passes without
it.**

An Interface Association Descriptor exists to say "these N *consecutive*
interfaces are one function". CDC needs it because a serial port is two
interfaces, a control interface and a data interface, that a single driver must
claim; without the IAD, and without the device class that tells the host to go
looking for one, the host binds the first interface as the whole device and the
second function never appears. That is why phase 2 needed it for CDC+HID.

This device has two functions of **one interface each**: GUD on interface 0,
gamepad on interface 1. There is nothing for an IAD to group. Linux binds
interface drivers per-interface from the configuration descriptor regardless of
`bDeviceClass`, and the gud driver's own match string wildcards the device
class:

```
usb:v1209p4FB3d*dc*dsc*dp*icFFisc*ip*in*
                ^^^ device class: wildcard
                            ^^^^ interface class: must be FF
```

Declaring `Misc/Common/IAD` while emitting no IAD descriptors would have been a
false statement about the device. `bDeviceClass = 0x00` is the honest
description of a composite device whose functions are one interface apiece, and
section 1 above is the evidence it works.

## 5. The other thing that had to be right

TinyUSB offers each interface to **application** class drivers before the
built-in ones (`get_driver()` in `usbd.c` searches `_app_driver` first). Our GUD
driver's `open()` therefore has to *decline* anything that is not
`bInterfaceClass == 0xFF`:

```c
TU_VERIFY(itf_desc->bInterfaceClass == TUSB_CLASS_VENDOR_SPECIFIC, 0);
```

Returning 0 means "not mine", and interface 1 falls through to TinyUSB's HID
driver. Had that check been missing, the GUD driver would have swallowed the
HID interface and the gamepad would have vanished.

## 6. Bus sharing

The HID endpoint is interrupt IN with a 10 ms `bInterval`, which reserves a
slot in every frame whether or not it is used. `buttons_task()` therefore only
transmits when the button state actually changes, leaving that bandwidth to
pixels. An idle gamepad costs the display nothing.

USB packet memory on the G4's fsdev peripheral is a single 1024-byte pool:
endpoint 0 takes 64 in + 64 out, GUD bulk OUT another 64, HID interrupt IN 16.
224 of 1024 bytes — nowhere near a constraint.

## 7. RAM and flash

```
RAM:   48,144 B of 131,072  (36.7%)   -- 40,960 of it is the band buffer
FLASH: 17,536 B of 524,288  ( 3.3%)
```

The gamepad cost 80 bytes of RAM and 2,280 bytes of flash on top of M2.

## 8. Reproducing

```bash
cd m3_composite
cmake -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=arm-toolchain.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build
probe-rs run --chip STM32G474QE --speed 1000 build/m3_composite
```

On the host, to watch button events without installing `jstest`:

```bash
python3 - <<'EOF'
import struct
with open("/dev/input/js0","rb") as f:
    while True:
        t,v,typ,num = struct.unpack("<IhBB", f.read(8))
        if typ & 0x80: continue
        print(("BUTTON" if typ==1 else "AXIS"), num, v, flush=True)
EOF
```
