# Odpiranje v STM32CubeIDE

Mapa `final/` je pripravljena tako, da jo CubeIDE prevede z Makefilom, ki je
tukaj. Rezultat je enak kot pri CMake: 24 300 B programskega in 84 472 B
podatkovnega pomnilnika.

**Pomembno:** mapa `tinyusb/` mora ostati poleg `final/`, torej

```
misko/
  final/
  tinyusb/
```

Če jo prestaviš drugam, popravi `TUSB` na vrhu Makefila.

---

## 1. Uvoz projekta

`File` &rarr; `Open Projects from File System...` &rarr; izberi mapo `final`
&rarr; `Finish`.

Če CubeIDE mape ne sprejme, uporabi zanesljivejšo pot, pri kateri si projektne
datoteke ustvari sam:

`File` &rarr; `New` &rarr; `Makefile Project with Existing Code`

- Existing Code Location: mapa `final`
- Toolchain: **MCU ARM GCC**
- `Finish`

Obe poti pripeljeta do istega: CubeIDE kliče `make` v tej mapi.

## 2. Prevajanje

`Project` &rarr; `Build Project`, ali <kbd>Ctrl</kbd>+<kbd>B</kbd>.

Nastanejo `build/misko3.elf`, `build/misko3.hex` in `build/misko3.bin`.

Iz terminala je isto:

```
make          # prevedi
make clean    # pobriši build/
make flash    # naloži na ploščico prek STM32CubeProgrammer
```

## 3. Nalaganje na ploščico

### Najhitreje

```
make flash
```

Makefile sam poišče `STM32_Programmer_CLI` znotraj namestitve CubeIDE. Če ga ne
najde, mu pot povej:

```
make flash CUBEPROG=/pot/do/STM32_Programmer_CLI
```

### Iz CubeIDE

`Run` &rarr; `Debug Configurations...` &rarr; dvoklik na
`STM32 C/C++ Application`

- Project: `final`
- C/C++ Application: `build/misko3.elf`
- Zavihek `Debugger`: Debug probe **ST-LINK (OpenOCD)** ali **ST-LINK GDB
  server**, Board: `Custom`, MCU: `STM32G474QETx`
- Zavihek `Debugger`, SWD frequency: **1000 kHz**

Nato `Debug`.

**Frekvenca SWD ni podrobnost.** Pri polni hitrosti približno dvajset nožic
vodila FMC preklaplja tik ob liniji SWDIO in razhroščevalni prenosi se
pokvarijo. Vse v tem projektu je bilo narejeno pri 1000 kHz ali manj.

### Samo naložiti, brez razhroščevanja

Odpri `build/misko3.hex` v programu STM32CubeProgrammer, poveži se prek SWD in
klikni `Download`. Datoteka `.hex` nosi svoje naslove, zato ni treba vnašati
ničesar. Če bi uporabil `.bin`, moraš ročno vpisati naslov `0x08000000`.

## 4. Preverjeno

Nalaganje prek `STM32_Programmer_CLI` z zastavico `-rst` je preizkušeno in po
njem se USB pravilno vzpostavi:

```
File download complete
Software reset is performed

$ lsusb
Bus 001 Device 054: ID 1d50:614d OpenMoko, Inc. Generic Display
$ ls /dev/input/js*
/dev/input/js0
```

To je vredno omeniti, ker `probe-rs reset` na tej ploščici **ne** deluje: po
njem naprava ne dokonča vzpostavitve USB. Reset iz CubeProgrammerja te težave
nima.
