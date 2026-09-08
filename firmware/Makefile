# ------------------------------------------------------------------
# MiSKo3: USB display and gamepad
#
# A plain Makefile, so the project builds inside STM32CubeIDE as a
# "Makefile Project with Existing Code" without needing CMake or Ninja.
# The flags match CMakeLists.txt exactly; either build produces the same
# binary.
#
#   make          build ELF, HEX and BIN into build/
#   make flash    program the board with STM32CubeProgrammer
#   make clean
# ------------------------------------------------------------------

TARGET   := misko3
BUILD    := build

# Toolchain. CubeIDE puts its own arm-none-eabi-gcc on PATH; a system one
# works just as well. Override with:  make PREFIX=/path/to/arm-none-eabi-
PREFIX   ?= arm-none-eabi-
CC       := $(PREFIX)gcc
OBJCOPY  := $(PREFIX)objcopy
SIZE     := $(PREFIX)size

# Directory commands differ between cmd.exe and a POSIX shell. GNU make sets
# OS=Windows_NT on Windows, including the make that ships with STM32CubeIDE.
ifeq ($(OS),Windows_NT)
  mkdir_p = @if not exist "$(subst /,\,$(1))" mkdir "$(subst /,\,$(1))"
  rm_rf   = @if exist "$(subst /,\,$(1))" rmdir /s /q "$(subst /,\,$(1))"
else
  mkdir_p = @mkdir -p $(1)
  rm_rf   = @rm -rf $(1)
endif

# TinyUSB lives next to this directory. Override if it is somewhere else.
TUSB     ?= ../tinyusb/src

LDSCRIPT := STM32G474QETX_FLASH.ld

# ------------------------------------------------------------------
# Sources
# ------------------------------------------------------------------

SRC_APP := \
  src/main.c \
  src/gud_device.c \
  src/gud_usbd.c \
  src/panel.c \
  src/lz4_dec.c \
  src/buttons.c \
  src/joystick.c \
  src/usb_descriptors.c \
  src/rtt.c

# TinyUSB, by basename. vpath below says where to find each one. Their objects
# go under build/tusb/ so that nothing is ever written into the library tree.
SRC_TUSB := \
  tusb.c \
  tusb_fifo.c \
  usbd.c \
  hid_device.c \
  dcd_stm32_fsdev.c \
  fsdev_common.c

vpath %.c $(TUSB) $(TUSB)/common $(TUSB)/device $(TUSB)/class/hid \
          $(TUSB)/portable/st/stm32_fsdev

SRC_S := src/startup_stm32g474qetx.s

# ------------------------------------------------------------------
# Flags. Identical to CMakeLists.txt.
# ------------------------------------------------------------------

CPU := -mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard

INC := -Isrc -I$(TUSB) -Icmsis/Include -Icmsis/Device/ST/STM32G4xx/Include

CFLAGS := $(CPU) -std=gnu11 -O2 -Wall -Wextra \
          -ffunction-sections -fdata-sections \
          -DSTM32G474xx $(INC)

LDFLAGS := $(CPU) -T$(LDSCRIPT) \
           --specs=nano.specs --specs=nosys.specs \
           -Wl,--gc-sections -Wl,-Map=$(BUILD)/$(TARGET).map \
           -Wl,--print-memory-usage

OBJ := $(addprefix $(BUILD)/,$(SRC_APP:.c=.o) $(SRC_S:.s=.o)) \
       $(addprefix $(BUILD)/tusb/,$(SRC_TUSB:.c=.o))
DEP := $(OBJ:.o=.d)

# ------------------------------------------------------------------

.PHONY: all clean flash flash-only
all: $(BUILD)/$(TARGET).elf $(BUILD)/$(TARGET).hex $(BUILD)/$(TARGET).bin
	@$(SIZE) $(BUILD)/$(TARGET).elf

$(BUILD)/tusb/%.o: %.c
	$(call mkdir_p,$(patsubst %/,%,$(dir $@)))
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/%.o: %.c
	$(call mkdir_p,$(patsubst %/,%,$(dir $@)))
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/%.o: %.s
	$(call mkdir_p,$(patsubst %/,%,$(dir $@)))
	$(CC) $(CPU) -c $< -o $@

$(BUILD)/$(TARGET).elf: $(OBJ)
	$(CC) $(OBJ) $(LDFLAGS) -o $@

$(BUILD)/$(TARGET).hex: $(BUILD)/$(TARGET).elf
	$(OBJCOPY) -O ihex $< $@

$(BUILD)/$(TARGET).bin: $(BUILD)/$(TARGET).elf
	$(OBJCOPY) -O binary $< $@

# STM32CubeProgrammer, as bundled inside STM32CubeIDE. Point CUBEPROG at it
# if the path differs. freq=1000 keeps SWD slow enough to survive the FMC
# pins switching right next to SWDIO.
CUBEPROG ?= $(firstword $(wildcard \
  $(HOME)/st/stm32cubeide*/plugins/com.st.stm32cube.ide.mcu.externaltools.cubeprogrammer.linux64_*/tools/bin/STM32_Programmer_CLI \
  /opt/st/stm32cubeide*/plugins/com.st.stm32cube.ide.mcu.externaltools.cubeprogrammer.linux64_*/tools/bin/STM32_Programmer_CLI))

# Flash whatever build/final.hex already holds, without building anything.
# This is the target to use after copying a prebuilt tree to another machine:
# it needs neither an ARM toolchain nor the TinyUSB sources, only the probe.
.PHONY: flash-only
flash-only:
	@test -f $(BUILD)/$(TARGET).hex || { echo "missing $(BUILD)/$(TARGET).hex"; exit 1; }
	@test -n "$(CUBEPROG)" || { echo "STM32_Programmer_CLI not found, set CUBEPROG="; exit 1; }
	"$(CUBEPROG)" -c port=SWD freq=1000 -w $(BUILD)/$(TARGET).hex -rst

flash: $(BUILD)/$(TARGET).hex
	@test -n "$(CUBEPROG)" || { echo "STM32_Programmer_CLI not found, set CUBEPROG="; exit 1; }
	"$(CUBEPROG)" -c port=SWD freq=1000 -w $< -rst

# ------------------------------------------------------------------
# The flash targets below use a POSIX shell and Linux paths. On Windows use
# flash.ps1, or open build/misko3.hex in the STM32CubeProgrammer GUI.
# ------------------------------------------------------------------

# ------------------------------------------------------------------
# Fallback programmers, for a machine without STM32CubeIDE installed.
# Any one of these does the same job; pick whichever tool is present.
# ------------------------------------------------------------------

# stlink-tools (package "stlink"). Takes the raw binary, so the load address
# has to be given explicitly: a .bin carries no addresses of its own.
.PHONY: flash-stlink
flash-stlink:
	st-flash --reset write $(BUILD)/$(TARGET).bin 0x08000000

# OpenOCD. Reads the ELF, so no address is needed. hla_swd is the transport
# the ST-LINK on this board speaks.
.PHONY: flash-openocd
flash-openocd:
	openocd -f interface/stlink.cfg -c "transport select hla_swd" \
	        -f target/stm32g4x.cfg -c "adapter speed 1000" \
	        -c "program $(BUILD)/$(TARGET).elf verify reset exit"

clean:
	$(call rm_rf,$(BUILD))

-include $(DEP)
