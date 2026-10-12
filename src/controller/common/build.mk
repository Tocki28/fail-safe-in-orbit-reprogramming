# build.mk — settings shared by every controller program's Makefile.
#
# A program's Makefile sets, before including this file:
#   NAME        output name, e.g. bootloader
#   BUILD       output directory
#   SRCS        our C files (held to -Werror)
#   SRCS_EXT    third-party C files (compiled as shipped, warnings shown but not fatal)
#   CODE_START  CODE_BYTES   names from layout.h: where the program is linked
#   OPT         optimisation flag
#   DEFS        extra -D flags
#   INCLUDES    extra -I flags
#   HEADERS     the program's own headers (rebuild when they change)

COMMON := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))

# Toolchain bundled with STM32CubeIDE (not on PATH).
TOOLCHAIN ?= /Applications/STM32CubeIDE.app/Contents/Eclipse/plugins/com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32.13.3.rel1.macos64_1.0.0.202411102158/tools/bin
CC      = $(TOOLCHAIN)/arm-none-eabi-gcc
OBJCOPY = $(TOOLCHAIN)/arm-none-eabi-objcopy
SIZE    = $(TOOLCHAIN)/arm-none-eabi-size

# ST's G4 package: device header, CMSIS core header, startup file, SystemInit.
CUBE ?= $(HOME)/STM32Cube/Repository/STM32Cube_FW_G4_V1.6.3
DEV   = $(CUBE)/Drivers/CMSIS/Device/ST/STM32G4xx
SRCS_ST = $(DEV)/Source/Templates/system_stm32g4xx.c
ASM_ST  = $(DEV)/Source/Templates/gcc/startup_stm32g431xx.s

# Cortex-M4 with single-precision FPU, as on the STM32G431.
CPU = -mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard

# -ffunction-sections/-fdata-sections + --gc-sections: unused functions are dropped.
# -fstack-usage: writes each function's stack use to a .su file next to its object.
CFLAGS = $(CPU) $(OPT) -g3 -std=c11 -DSTM32G431xx $(DEFS) \
         -I$(DEV)/Include -I$(CUBE)/Drivers/CMSIS/Include -I$(COMMON) $(INCLUDES) \
         -ffunction-sections -fdata-sections -fstack-usage -Wall -Wextra
# Linking as STM32CubeIDE projects do (nano C library, no OS). Expect "_close/_lseek/_read/
# _write is not implemented" messages: they come from the C library's unused start file, and
# --gc-sections drops that code (no malloc or stdio in the image: check with nm).
# --no-warn-rwx-segments: the toolchain marks .init_array writable, yet it sits in flash with
# the code; nothing writes it.
LDFLAGS = $(CPU) --specs=nano.specs --specs=nosys.specs -Wl,--gc-sections \
          -Wl,--no-warn-rwx-segments -Wl,-Map=$(BUILD)/$(NAME).map -T$(BUILD)/program.ld

OBJS = $(addprefix $(BUILD)/,$(notdir $(SRCS:.c=.o) $(SRCS_EXT:.c=.o) $(SRCS_ST:.c=.o) $(ASM_ST:.s=.o)))
vpath %.c $(sort $(dir $(SRCS) $(SRCS_EXT) $(SRCS_ST)))
vpath %.s $(dir $(ASM_ST))

all: $(BUILD)/$(NAME).bin

$(BUILD):
	mkdir -p $@

# The linker script goes through the C preprocessor so it can use layout.h.
$(BUILD)/program.ld: $(COMMON)/program.ld $(COMMON)/layout.h | $(BUILD)
	$(CC) -E -P -undef -x c -I$(COMMON) -DCODE_START=$(CODE_START) -DCODE_BYTES=$(CODE_BYTES) $< -o $@

$(addprefix $(BUILD)/,$(notdir $(SRCS:.c=.o))): $(BUILD)/%.o: %.c $(wildcard $(COMMON)/*.h) $(HEADERS) $(MAKEFILE_LIST) | $(BUILD)
	$(CC) $(CFLAGS) -Werror -c $< -o $@

$(addprefix $(BUILD)/,$(notdir $(SRCS_EXT:.c=.o) $(SRCS_ST:.c=.o))): $(BUILD)/%.o: %.c $(MAKEFILE_LIST) | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: %.s | $(BUILD)
	$(CC) $(CPU) -c $< -o $@

$(BUILD)/$(NAME).elf: $(OBJS) $(BUILD)/program.ld
	$(CC) $(OBJS) $(LDFLAGS) -o $@
	$(SIZE) $@

$(BUILD)/$(NAME).bin: $(BUILD)/$(NAME).elf
	$(OBJCOPY) -O binary $< $@

clean:
	rm -rf $(BUILD)

.PHONY: all clean
