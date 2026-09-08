#!/bin/sh
# ------------------------------------------------------------------
# Naloži firmware na ploščico MiSKo3.
#
# Sam poišče, katero orodje je na voljo, in uporabi ustrezno datoteko.
# Ne prevaja ničesar, zato na tem računalniku ni potreben ne
# arm-none-eabi-gcc ne TinyUSB. Potrebni so le datoteke v build/ in sonda.
#
#   ./flash.sh
# ------------------------------------------------------------------
set -e
cd "$(dirname "$0")"

B=build
VIDPID=1d50:614d

say() { printf '%s\n' "$*"; }
die() { printf 'NAPAKA: %s\n' "$*" >&2; exit 1; }

# ---- ali so datoteke tu -------------------------------------------------
[ -d "$B" ] || die "mape $B ni. Prenesi celotno mapo final, vključno z build/."

# ---- katero orodje imamo ------------------------------------------------
CUBE=$(ls "$HOME"/st/stm32cubeide*/plugins/com.st.stm32cube.ide.mcu.externaltools.cubeprogrammer.linux64_*/tools/bin/STM32_Programmer_CLI \
          /opt/st/stm32cubeide*/plugins/com.st.stm32cube.ide.mcu.externaltools.cubeprogrammer.linux64_*/tools/bin/STM32_Programmer_CLI \
          /opt/ST/STM32CubeProgrammer/bin/STM32_Programmer_CLI 2>/dev/null | head -1 || true)

if [ -n "$CUBE" ] && [ -f "$B/misko3.hex" ]; then
    TOOL="STM32CubeProgrammer"
elif command -v st-flash >/dev/null 2>&1 && [ -f "$B/misko3.bin" ]; then
    TOOL="st-flash"
elif command -v openocd >/dev/null 2>&1 && [ -f "$B/misko3.elf" ]; then
    TOOL="openocd"
elif command -v probe-rs >/dev/null 2>&1 && [ -f "$B/misko3.elf" ]; then
    TOOL="probe-rs"
else
    say "Ni ustreznega orodja ali datoteke."
    say ""
    say "V build/ so:"
    ls -1 "$B" 2>/dev/null | sed 's/^/  /'
    say ""
    say "Namesti eno od teh:"
    say "  sudo pacman -S stlink     (najmanjše, priporočeno)"
    say "  sudo pacman -S openocd"
    exit 1
fi

say "Orodje: $TOOL"
say ""

# ---- nalaganje ----------------------------------------------------------
# Frekvenca SWD je povsod omejena na 1000 kHz. Pri polni hitrosti nožice
# vodila FMC preklapljajo tik ob liniji SWDIO in prenos se pokvari.
case "$TOOL" in
STM32CubeProgrammer)
    "$CUBE" -c port=SWD freq=1000 -w "$B/misko3.hex" -rst
    ;;
st-flash)
    # .bin ne nosi naslovov, zato je 0x08000000 zapisan izrecno.
    st-flash --freq=1000k --reset write "$B/misko3.bin" 0x08000000
    ;;
openocd)
    openocd -f interface/stlink.cfg -c "transport select hla_swd" \
            -f target/stm32g4x.cfg -c "adapter speed 1000" \
            -c "program $B/misko3.elf verify reset exit"
    ;;
probe-rs)
    # Samo zapis. probe-rs reset na tej ploščici pusti USB nedelujoč,
    # zato po tem ploščico ročno odklopi in priklopi.
    probe-rs download --chip STM32G474QE --speed 1000 "$B/misko3.elf"
    say ""
    say "OPOZORILO: probe-rs ne resetira zanesljivo. Odklopi in priklopi USB."
    ;;
esac

# ---- ali se je naprava prijavila ----------------------------------------
say ""
say "Čakam, da se naprava prijavi na USB ..."
i=0
while [ $i -lt 10 ]; do
    if lsusb 2>/dev/null | grep -qi "$VIDPID"; then
        say ""
        say "USB: OK"
        lsusb | grep -i "$VIDPID" | sed 's/^/  /'
        [ -e /dev/input/js0 ] && say "  igralni plošček: /dev/input/js0"
        ls /sys/class/drm/ 2>/dev/null | grep -q . && \
          say "  zasloni DRM: $(ls /sys/class/drm/ | grep -c '^card[0-9]*$') kartic"
        exit 0
    fi
    i=$((i + 1))
    sleep 1
done

say ""
say "Naprava se ni prijavila na USB v 10 sekundah."
say "Nalaganje je najbrž uspelo, reset pa ne. Odklopi in priklopi kabel."
exit 1
