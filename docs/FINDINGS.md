# MiSKo3 — GUD USB display + HID gamepad: feasibility findings

Spike run 2026-08-24/25 on the actual board (STM32G474QET6, serial
`066DFF515570514867121945`). Every number below was measured on hardware
unless explicitly marked as arithmetic or as an assumption.

---

## 1. Summary table

| # | Unknown | Criterion | Measured | Verdict |
|---|---|---|---|---|
| 0 | Toolchain / board identity | usable | G474 + 512 KB confirmed from silicon | **PASS** |
| A | FMC blit throughput | ≥ 20 MB/s | **48.56 MB/s** (3.162 ms/frame) | **PASS** (2.4×) |
| B | USB enumeration | 10 replugs | **10/10**, 0.2–0.8 s | **PASS** |
| B | USB composite | both bind | CDC + HID both bound, 7/7 buttons verified | **PASS** |
| B | USB bulk throughput | ≥ 0.8 MB/s | **0.67 MB/s** (composite) | **FAIL** (−16%) |
| C | Host GUD support | `gud` present | in-tree module, kernel 7.2.0-1-cachyos | **PASS** |
| C | BC1 format | reachable | **does not exist in GUD** | **N/A** |
| D | TE pin | reachable | **not routed** — 4 independent confirmations | **FAIL** |
| D | TE workaround | — | `GET_SCANLINE` works, 80.95 Hz, 1.2 µs/poll | **PASS** |

---

## 2. Silicon identity (Phase 0)

| Register | Address | Value | Meaning |
|---|---|---|---|
| `DBGMCU_IDCODE` | `0xE0042000` | `0x20036469` | DEV_ID `0x469` = G4 category 3; REV_ID `0x2003` |
| Flash size | `0x1FFF75E0` | `0x0200` | 512 KB (`0x200` = 512, register is in KB) |
| Unique ID | `0x1FFF7590` | `240036001550414D39363420` | wafer X=36 Y=54, wafer 21, lot `PAM964` |

`DEV_ID 0x469` covers G471/G473/G474/G483/G484, so it does not by itself
prove "G474". Closed that gap by toggling **only** the HRTIM1 clock-enable bit
(`RCC_APB2ENR` bit 26) and watching the peripheral appear:

- clock gated → writes to `HRTIM1_MPER` are dropped
- clock enabled → bit 26 sticks, and `0x1234` / `0xABCD` / `0x5A5A` all read back exactly

HRTIM1 exists ⇒ **G474 (or G484), not G473**. G484 differs only by an AES
block and was not tested; it is functionally irrelevant here.

128 KB RAM corroborated independently: the factory firmware's initial stack
pointer is `0x20020000`, which is only legal on a part with the full
SRAM1(80K) + SRAM2(16K) + CCM(32K) contiguous map.

**Factory firmware backup:** `backup/misko3-flash-backup-20260824-191118.bin`,
524,288 bytes, sha256 `04cb7dd63af1c35f91e6d382c03c7b248f9cd54e991d86073e44cf761afc0ffe`,
verified against silicon at six offsets after conversion.

---

## 3. Unknown A — FMC blit throughput

### The two magic addresses

`LCD_RS` = PG5 = `FMC_A15`; NE1 selects NOR/PSRAM bank 1 at `0x60000000`.
On a 16-bit bus the FMC scales the CPU byte address by one bit:

```
FMC_A[n]  <-  HADDR[n+1]
A15       <-  HADDR[16]  =  1 << 16  =  0x00010000

command (RS=0):  0x60000000
data    (RS=1):  0x60010000
```

Matches the stock driver's `FMC_BANK1_REG` / `FMC_BANK1_MEM` exactly.

### Measurements (100 runs each, DWT CYCCNT, HCLK 170 MHz)

| Test | Bytes | ms (min/mean/max) | MB/s | cycles/write |
|---|---|---|---|---|
| Full frame, CPU | 153,600 | 3.162 / 3.162 / 3.162 | **48.56** | 7.00 |
| Full frame, CPU + `DSB` | 153,600 | 4.066 / 4.066 / 4.066 | 37.77 | 9.00 |
| Full frame, DMA mem2mem | 153,600 | 3.168 / 3.168 / 3.168 | 48.48 | 7.01 |
| 32×32 window, CPU | 2,048 | 0.036 | 56.38 | ~6 |
| 32×32 window, DMA | 2,048 | 0.042 | 48.05 | ~7 |

Jitter is essentially zero because this is an uninterrupted deterministic loop
— these are best-case numbers with no interrupt load.

### Three findings

1. **The stock driver's per-write `__DSB()` costs 22%.**
   `FMC_BANK1_WriteData()` barriers after every half-word: exactly +2
   cycles/write. Removing it is free performance.

2. **DMA gives no throughput gain** (48.48 vs 48.56 MB/s) because the FMC bus,
   not the CPU, is the limit. Its value is *freeing the CPU* — which matters
   once LZ4 decompression is in the loop.

3. **FMC timing tuning is pointless here.** All eight ADDSET/DATAST/BUSTURN
   combinations from (1,1,1) down to (0,0,0) produced identical 6 cyc/write,
   with ID/MADCTL/PIXFMT verifying OK every time. Confirmed from silicon that
   the writes land (`BTR1 = 0x000101F1`). The floor is fixed FMC+AHB
   per-access overhead, ~6–7 HCLK per 16-bit write. **No glitching could be
   induced even at minimum timing.**

**Datasheet note:** ILI9341 spec tWC is 66 ns minimum write cycle; we run
41 ns. The panel is already overclocked ~1.6× at stock settings and works, but
that is outside datasheet — a risk, not a guarantee.

---

## 4. Unknown B — USB

### Clock: PLLQ at 48 MHz is impossible alongside 170 MHz

```
SYSCLK = VCO / PLLR,  PLLR in {2,4,6,8}
170 MHz -> VCO in {340, 680, 1020, 1360};  G4 VCO range is 96..344 MHz
        -> VCO = 340 MHz is the ONLY legal choice

PLLQ in {2,4,6,8}:  340/2=170  340/4=85  340/6=56.67  340/8=42.5
                    none is 48
```

So the brief's "PLLQ to 48 MHz" cannot be done at 170 MHz. **HSI48 + CRS is
forced, not chosen.** (Alternative if you ever want a crystal-derived USB
clock: PLLM=1, PLLN=36 → VCO 288, PLLQ=6 = exactly 48 MHz, but PLLR=2 then
caps SYSCLK at 144 MHz. Phase 1 has margin to spare, so this is viable.)

**CRS is mandatory.** HSI48 is an RC oscillator: ~±1% trimmed at 25 °C,
drifting to ~±3% over voltage and temperature; USB full speed requires
±0.25%. CRS counts HSI48 cycles between the host's 1 ms SOF packets and trims
HSI48 to null the error. Verified live: `CRS->CFGR = 0x2022BB7F`
(RELOAD 47999, FELIM 34, SYNCSRC=10=USB SOF), `CRS->CR = 0x4060`
(CEN + AUTOTRIMEN).

*The stock MiSKo3 firmware enables HSI48 but never configures CRS — it works
on a warm desk and is out of spec over temperature.*

### Results

- **Enumeration: 10/10**, 0.2–0.8 s per cycle.
- **CDC echo:** 96/96 bytes exact.
- **Composite:** all three interfaces bind —
  `If0 Communications → cdc_acm`, `If1 CDC Data → cdc_acm`,
  `If2 HID → usbhid`, giving `/dev/ttyACM1` + `js0` + `event24`.
  All 7 buttons verified on hardware: BTN_OK/BTN_ESC/JOY_BTN →
  `BTN_GAMEPAD+0/1/2`, D-pad → `ABS_HAT0X/Y` ±1.
- **Throughput: 0.75 MB/s** (CDC only), **0.62–0.69 MB/s** (composite,
  mean 0.67). **FAILS the 0.8 MB/s bar by 16%.**

### Root cause of the shortfall

TinyUSB's own port header, `dcd_stm32_fsdev.c:58`:

```
 * - No double-buffering
```

Single-buffered bulk forces an interrupt and re-arm between every 64-byte
packet:

```
0.75 MB/s / 64 B = ~11.7 packets per 1 ms frame,  against the 19 USB FS allows
```

Ruled out the host side rather than assuming: host write size 4 KB → 256 KB
changed nothing (0.746–0.753 MB/s), and attaching/detaching the debugger
changed nothing (0.749 vs 0.751). Adding HID costs a further ~10%.

**Ceiling if fixed:** ~1.216 MB/s (USB FS bulk theoretical max).

---

## 5. Unknown C — host GUD readiness

| Item | Result |
|---|---|
| Kernel | 7.2.0-1-cachyos |
| `gud` | **present, in-tree** (`drivers/gpu/drm/gud/gud.ko.zst`) |
| Session | Wayland, KDE (KWin) |
| Kernel headers | installed; out-of-tree builds need **`make LLVM=1`** (kernel is Clang + ThinLTO + AutoFDO) |
| Module signing | `MODULE_SIG_FORCE` unset, `sig_enforce=N` → unsigned modules load |
| DKMS | 3.4.3 installed |
| gamescope | not installed; available (3.16.25-1) |

### Formats — BC1 does not exist

```
R1 0x01   R8 0x08   XRGB1111 0x20   RGB332 0x30
RGB565 0x40   RGB888 0x50   XRGB8888 0x80   ARGB8888 0x81

GUD_COMPRESSION_LZ4  BIT(0)   <- the only compression
```

| Format | Reachable | Host cost |
|---|---|---|
| RGB565 | yes, native | XRGB8888→RGB565 in `gud`, trivial |
| RGB332 | yes | trivial, but bands badly without dithering |
| **BC1** | **no — not in the protocol** | n/a |

**The bandwidth lever is LZ4, not block compression.** Drop BC1 from the plan.

### Two constraints worth knowing

- **VID/PID is fixed.** The driver binds only `1d50:614d`, `16d0:10a9`,
  `1209:4fb3`, all `icFF` (vendor-specific — GUD is raw bulk, not CDC). Use
  **`1209:4fb3`** (pid.codes), as gud-pico does.
- **`max_buffer_size`** in the display descriptor exists precisely for
  "devices that don't have a big enough buffer to decompress the entire
  framebuffer in one go" — which is exactly our situation (below).

### LZ4 ratios measured (lz4 1.10.0, synthetic 320×240 RGB565)

| Content | Compressed | Ratio |
|---|---|---|
| Solid colour | 629 B | 244× |
| Flat UI panels | 641 B | 240× |
| Terminal text | 5,733 B | **26.8×** |
| Smooth gradient | 10,910 B | **14.1×** |
| Random noise | 153,615 B | 1.0× |

These are synthetic, not a capture of your actual desktop — treat 10–25× as
the realistic working range and 1× as the hard floor for photo/video.

---

## 6. Unknown D — tearing effect

**TE is not routed on this board.** Four independent confirmations:

1. No TE net in any of the 13 Altium `.SchDoc` sheets.
2. Zero `TE`/`TEARING` hits across all 29 pages of the exported PDF.
3. The LCD sheet's complete port list is `FMC_D0..D15, CS, CS1, RS, RD, WR,
   RST, BKLT, FLASH_CS` — no TE.
4. J5's 36 pins are fully mapped with no spare.

`lcd.h` maps `LCD_TE` to PC14, which is `BTN_ESC` on this board — which is why
`LCD_TE_ENABLE` is commented out. It was aspirational, never wired.

**Note:** U60 pin 21 is `IM0`, the ILI9341 bus-width strap — *not* TE. R52
pulls it high (16-bit mode); the "LCD 8BIT" solder jumper would pull it low
for 8-bit. **Leave that jumper open.** Confirmed consistent with the working
16-bit FMC configuration.

### The workaround works

`GET_SCANLINE` (0x45) reports the panel's live scan position over the
parallel bus:

```
range 0..323 (320 active + ~4 blanking)
81 wraps in 1 s  ->  frame period 12,353 us  ->  80.95 Hz
poll cost 208 cycles = 1.223 us
```

**Full-frame blit is 3.162 ms against a 12.353 ms frame period — 3.9× faster
than the scan-out.** Start writing at the top of vertical blanking and the
write front stays ahead of the scan front for the whole frame: tear-free
without a second buffer. Sync once, run a timer at 80.95 Hz, re-sync
occasionally for drift. Continuous polling is not required.

---

## 7. Expected frame rate — the arithmetic

### Frame sizes at 320×240 = 76,800 pixels

```
RGB565:  76,800 x 2 B = 153,600 B
RGB332:  76,800 x 1 B =  76,800 B
BC1:     does not exist in GUD
```

### The RAM problem, and why it is survivable

```
RGB565 frame  = 153,600 B
G474 RAM      = 131,072 B    ->  a full RGB565 framebuffer CANNOT fit
RGB332 frame  =  76,800 B    ->  fits (59% of RAM)
```

GUD's `max_buffer_size` field is the designed answer: declare a smaller
buffer and the host chunks transfers to fit. Not a blocker, but it forces a
streaming/tiled architecture — no double-buffering in RAM.

### Pipeline ceilings (measured)

```
Panel refresh          80.95 fps    <- hard ceiling, cannot exceed
FMC blit               316 fps      (3.162 ms/frame)
USB @ 0.67 MB/s        depends on compression
```

### Effective full-frame rate = min(all three)

| Format / content | USB bytes | USB fps | Effective fps |
|---|---|---|---|
| RGB565, uncompressed | 153,600 | 4.4 | **4.4** |
| RGB332, uncompressed | 76,800 | 8.7 | **8.7** |
| RGB565 + LZ4, flat UI (240×) | 641 | ~1000 | **80.9** (panel-limited) |
| RGB565 + LZ4, text (26.8×) | 5,733 | 117 | **80.9** (panel-limited) |
| RGB565 + LZ4, gradient (14.1×) | 10,910 | 61.4 | **61.4** (USB-limited) |
| RGB565 + LZ4, photo/noise (1×) | 153,600 | 4.4 | **4.4** |

Worked example, gradient row:
```
153,600 B / 14.1 = 10,910 B compressed
10,910 B / 670,000 B/s = 16.28 ms  ->  61.4 fps
min(61.4 USB, 316 FMC, 80.95 panel) = 61.4 fps
```

**And this is before damage rectangles.** GUD sends only changed regions; a
blinking cursor is a few hundred bytes, not a frame. For typical desktop
content the effective rate is panel-limited, i.e. as good as the hardware
can physically go.

---

## 8. Verdict: **GO**

The project is feasible and the numbers support it, with one honest caveat
about what it will and will not do well.

**Why GO:**

- The two things that looked riskiest are comfortably solved. The FMC has
  2.4× margin over the bar and 3.9× over the panel's own refresh. TinyUSB
  enumerated first try, 10/10 reliably, and the composite CDC+HID descriptor
  approach — the architectural bet the whole project rests on — is proven on
  hardware with all seven buttons working.
- The host side needs nothing built: `gud` is in-tree and present.
- The missing TE pin, which looked like a hard hardware blocker, is fully
  worked around in software at 1.2 µs per poll.
- With LZ4, typical desktop content is **panel-limited at ~81 fps**, not
  bandwidth-limited.

**What it will not do:** full-screen video or photo slideshows. Incompressible
content runs at **4.4 fps** and no amount of engineering changes that — it is
the raw USB full-speed limit. If your demo is "a terminal, an editor, a status
panel on a second screen", it will look great. If it is "play a video on it",
it will not.

**The one failed criterion in context:** USB bulk missed 0.8 MB/s by 16%. That
matters far less than it looks, because LZ4 buys 14–27× on real content while
fixing double-buffering buys 1.8×. The bar was a proxy for "can USB feed the
display", and with compression the answer is yes.

### Riskiest remaining item, and how to retire it first

**LZ4 decompression on the G474 inside the RAM budget.** Everything above
assumes it works, and it is the only unmeasured link in the chain. Retire it
in this order:

1. **Measure LZ4 decompress throughput on the G474** (half a day). Feed it the
   compressed test frames from §5 and time with DWT. Needs to sustain well
   above 0.67 MB/s of *input*. Expect it to pass easily — LZ4 decode is
   simple byte-copy work — but measure it, do not assume.
2. **Fix the buffer strategy against `max_buffer_size`** (half a day). Decide
   the chunk size that fits alongside TinyUSB's FIFOs and the tile buffer,
   then declare it in the display descriptor.
3. **Only then port the GUD protocol** (~1000 lines from gud-pico). Use VID/PID
   `1209:4fb3` or the kernel will never bind.

Do **not** start with TinyUSB double-buffering. It is the tempting
optimisation and it is second-order.

### If it goes wrong

The measurement that would kill it is step 1: if LZ4 decode on the G474 cannot
sustain ~1 MB/s of input while also blitting, you fall back to uncompressed
RGB332 at 8.7 fps, which is not good enough to impress anyone. That is your
NO-GO trigger, and you would know within a day — well inside the two-week
window, with the CAN analyser still available as a fallback.

---

## 9. Practical notes for the build

- **Use OpenOCD, not probe-rs, for reset and recovery.** `probe-rs reset`
  leaves this target in a state where firmware does not complete USB
  bring-up (it cost an hour and a bogus 0/10 test result). `probe-rs run`
  for flash+RTT is fine.
  ```
  openocd -f interface/stlink.cfg -c "transport select hla_swd" \
    -f target/stm32g4x.cfg -c "reset_config srst_only srst_nogate connect_assert_srst" \
    -c "adapter speed 100" -c "init; reset halt; stm32l4x mass_erase 0; reset run; exit"
  ```
- **Connect SWD at reduced speed** (`--speed 1000`). At full speed, ~20 FMC
  pins slewing at VERY_HIGH next to SWDIO/SWCLK corrupt debug transactions.
- **Never let RTT/SWD polling overlap a benchmark.** Measure first, print
  afterwards with the bus idle.
- **Corrected pin facts** (the "MCU pinout" sheet of `Misko3 pin mapping.xlsx`
  is stale — three labels are rotated between PB5/PD3/PE0):
  - `LCD_RST` = **PD3** (proven functionally — the display works)
  - `FLASH_CS` = PB5, `LIN_SLEEP` = PE0
  - PF11 = `ARDUINO_CS`, a spare FMC chip select on J5 pin 21 — **not** a
    second display
- **8-bit mode is a free option.** Since USB is 36× slower than even a halved
  FMC, closing the "LCD 8BIT" jumper would free 8 GPIOs at no real cost. If
  you do, the data address changes: on an 8-bit bus `FMC_A[n] <- HADDR[n]`, so
  the data address becomes `0x60008000`, not `0x60010000`.
- **BOOT0 is PB8**, on the `USART3_RX` net; `nSWBOOT0` = 1 so the pin is live
  at reset. A test point labelled `USART3_RX` appears in the PCB layout —
  worth confirming it is exposed metal, as it is the last-resort recovery
  path. **Never write RDP level 2 (`0xCC`) — it permanently kills SWD.**
