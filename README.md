# MiSKo3 as a USB display and gamepad

Firmware that makes an STM32G474 development board appear to Linux as a **real
monitor** and a **HID gamepad** at the same time, over a single USB cable.

Nothing to install on the computer. The display is driven by `gud`, in the
Linux kernel since 5.13, and the gamepad by the standard `usbhid`. Plug it in
and your desktop offers it as a second screen you can drag a window onto.

![The board running as a second display](docs/photo.jpg)

## What you get

- **A second monitor**, 320×240, that your compositor treats like any other
  output.
- **A gamepad**: four face buttons, an analogue stick and two triggers.
- **A touch screen.** The board's XPT2046 panel appears as a HID digitizer.
  Linux places it in `HID_GROUP_MULTITOUCH`, so `hid-multitouch` claims it and
  it behaves as a real touch screen rather than a pointer.
- **An optional on-screen frame counter**, off by default, toggled with a
  button chord. It is the number visible in the photo above.

## Before you start

| | |
|---|---|
| Board | MiSKo3 with an ILI9341 panel on the FMC bus |
| To flash | an ST-LINK, and one of STM32CubeProgrammer, `st-flash` or OpenOCD |
| To use the display | **Linux**, kernel 5.13 or newer |
| To use the gamepad | Linux, Windows or macOS |
| To use the touch screen | Linux, with `hid-multitouch` available |

The display needs the kernel's `gud` driver and there is no Windows or macOS
equivalent. On those the board still enumerates and the gamepad works, but no
screen appears.

## Install

### 1. Get the firmware

Download `misko3-v1.2.0.hex` from the
[latest release](https://github.com/MrCloudy2/misko3-gud-display/releases/latest).
That is all you need; building from source is optional and covered further
down.

### 2. Get a programmer

You almost certainly have one of these already, or can install one in a
sentence:

```sh
sudo pacman -S stlink          # Arch, CachyOS
sudo apt install stlink-tools  # Debian, Ubuntu
brew install stlink            # macOS
```

On Windows, install
[STM32CubeProgrammer](https://www.st.com/en/development-tools/stm32cubeprog.html).

### 3. Flash it

Connect the ST-LINK and run one of these:

```sh
st-flash --freq=1000k --reset write misko3-v1.2.0.bin 0x08000000
```

```sh
openocd -f interface/stlink.cfg -c "transport select hla_swd" \
        -f target/stm32g4x.cfg -c "adapter speed 1000" \
        -c "program misko3-v1.2.0.hex verify reset exit"
```

Or open the `.hex` in the STM32CubeProgrammer GUI, connect over SWD, and press
**Download**.

Keep SWD at **1000 kHz or slower**. About twenty FMC bus pins switch right
beside SWDIO, and at full speed the debug transfers get corrupted.

Use the `.hex` where you can: it carries its own load addresses. The `.bin` does
not, which is why `0x08000000` has to be typed in above.

### 4. Check it worked

Plug the board's own USB cable into the computer:

```sh
lsusb | grep 1d50:614d
ls /sys/class/drm/          # a new card* appears
ls /dev/input/js*           # the gamepad
```

If the device appears but no screen does, your kernel's `gud` module may be
missing or unloadable:

```sh
modinfo gud | grep alias
sudo modprobe gud
```

## Using it

**The screen** shows up in your display settings as a new output, usually named
`USB-1`. Place it where you like and drag a window onto it. It is 320×240 at
60 Hz.

**The gamepad** appears as `js0`. The four direction switches are A, B, X and
Y, the thumbstick is the left stick, and OK and ESC are the triggers.

**The touch screen** appears as a second input device. Your compositor has to
be told which output it belongs to, or touches land on the primary monitor; in
KDE that is System Settings, Input Devices, Touchscreen. If the touch lands in
the wrong corner, flip the orientation flags at the top of
`firmware/src/touch.h` rather than using a host-side calibration matrix, so the
board stays correct on any machine.

**The frame counter** is off by default, since it draws over whatever the host
is showing. Hold **ESC + OK + right** to turn it on, and the same chord again
to turn it off. Switching it off restores the pixels it covered rather than
leaving a stale rectangle.

There is a second toggle on **ESC + OK + left**, which turns the tear-free blit
off and back on, if you want to see what it is doing for you.

Hold all three buttons together. Both triggers plus a direction is not
something that happens while playing.

## If something is wrong

**No screen, but `lsusb` finds the device.** The `gud` module is not loaded.
Check `modinfo gud`; if it reports the module is missing, your running kernel's
module tree is incomplete, which usually means a system update replaced it and
you have not rebooted yet.

**Nothing on the panel except colour bars.** That is the splash screen, and it
means the board is healthy but no host has connected. Same fix as above.

**The board does not enumerate after flashing.** Unplug and replug the USB
cable. `probe-rs reset` in particular leaves this board unable to complete USB
bring-up; every other programmer resets cleanly.

**Touch input lands on the wrong monitor.** Your compositor has to be told
which output the touch screen belongs to; in KDE that is System Settings,
Input Devices, Touchscreen.

**The stick click does nothing.** On the board this was developed against, the
thumbstick's push switch is not connected to PC13, though the schematic says it
should be. Nothing depends on it: both toggles use the triggers instead.

## Build from source

Only needed if you want to change something.

```sh
git clone --recurse-submodules https://github.com/MrCloudy2/misko3-gud-display
cd misko3-gud-display/firmware
make
```

Needs `arm-none-eabi-gcc`:

```sh
sudo pacman -S arm-none-eabi-gcc arm-none-eabi-newlib   # Arch, CachyOS
sudo apt install gcc-arm-none-eabi                      # Debian, Ubuntu
brew install --cask gcc-arm-embedded                    # macOS
```

On Windows, [STM32CubeIDE](https://www.st.com/en/development-tools/stm32cubeide.html)
brings the compiler, `make` and the programmer in one installer. The CMake
build is the better choice there because it needs no Unix shell:

```sh
cmake -B build -DCMAKE_TOOLCHAIN_FILE=arm-toolchain.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Both builds use identical flags and produce the same binary: 32,664 bytes of
flash and 92,904 bytes of RAM. You get `build/misko3.elf`, `.hex` and `.bin`.

To flash what you just built, `firmware/flash.sh` finds whichever programmer is
installed, picks the right file and waits for the board to enumerate.
`flash.ps1` is the PowerShell equivalent, translated but not tested on Windows.

For STM32CubeIDE, see [firmware/CUBEIDE.md](firmware/CUBEIDE.md).

## How it works, briefly

One USB address, two interfaces: class `0xFF` with a bulk endpoint for pixels,
class `0x03` for the buttons. A frame does not fit in the chip's 128 KB, so the
kernel is told a maximum buffer size and splits every screen update into two
120-line bands. Each is LZ4-compressed by the host, decompressed in place on
the device, and written to the panel column by column so the write front never
crosses the refresh.

The reasoning, the measurements behind each choice and the known limits are in
[docs/design.md](docs/design.md).

## Repository layout

```
firmware/          everything that runs on the board
  src/             ~1,800 lines of C
  Makefile         plain make, also usable from STM32CubeIDE
  CMakeLists.txt   the same build under CMake
  flash.sh         programmer-agnostic flashing
docs/
  design.md        why the firmware is built this way
  milestones/      measurements from each development stage
tools/             host-side verification of the LZ4 decoder
tinyusb/           submodule, pinned to the commit this was built against
```

## Attribution

Everything under `firmware/src/` is my own work apart from the ST startup file.

- **[TinyUSB](https://github.com/hathach/tinyusb)** (MIT) provides the USB
  stack, enumeration and the HID class. The GUD interface has no class driver
  in the library; that one is `firmware/src/gud_usbd.c`.
- **CMSIS** (ST) for register definitions, the startup file and the linker
  script.
- **ST's STM32G4 HAL**, for ADC4 and SPI1 only. Provenance and licence in
  [firmware/hal/README.md](firmware/hal/README.md). Everything else still
  writes registers directly.
- The ILI9341 initialisation sequence follows the board's factory firmware.
- The GUD protocol is defined by `include/drm/gud.h` in the Linux kernel.
  [gud-pico](https://github.com/notro/gud-pico) was read as a reference; no
  code was taken from it.
- The LZ4 decoder was written from the published format specification, not
  adapted from the reference implementation. It is verified by the harness in
  `tools/`: 20,000 valid blocks byte-exact, and 40,000 corrupted or truncated
  blocks with no buffer overrun, under AddressSanitizer.

The board is [MiSKo3](https://github.com/mjankovec/MiSKo3), designed at the
Faculty of Electrical Engineering, University of Ljubljana. Its schematics and
factory firmware are not reproduced here.

## Licence

MIT, see [LICENSE](LICENSE).
