# build.mk — settings shared by every controller program's Makefile.
#
# A program's Makefile sets, before including this file:
#   NAME        output name, e.g. bootloader
#   BUILD       output directory
#   SRCS        our C files (held to -Werror)
#   SRCS_CXX    our C++ files (held to -Werror); only the main firmware has any. A program
#               with C++ files is linked by g++, one without by gcc, exactly as before.
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
CXX     = $(TOOLCHAIN)/arm-none-eabi-g++
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
# C warnings: the same strict set as the C++ code below (owner, 13 Oct 2026; code rule D1):
#   -Wpedantic       ISO C11 only, no compiler extensions used by accident
#   -Wconversion     an implicit conversion that can change a value (e.g. uint32_t to uint8_t)
#   -Wsign-conversion  an implicit change of sign (int to unsigned and back)
#   -Wshadow         a local name that hides another
# ST's device headers and Arm's CMSIS headers come in through -isystem, as for C++: their own
# warnings are theirs, not ours (measured on 13 Oct 2026: the flags above give 18 warnings in
# CMSIS core_cm4.h and none in our C code). Our code is still held to every warning by -Werror.
CWARNINGS = -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Wswitch-enum
CFLAGS = $(CPU) $(OPT) -g3 -std=c11 -DSTM32G431xx $(DEFS) \
         -isystem $(DEV)/Include -isystem $(CUBE)/Drivers/CMSIS/Include -I$(COMMON) $(INCLUDES) \
         -ffunction-sections -fdata-sections -fstack-usage $(CWARNINGS)
# C++17 for the main firmware, with what a small controller cannot afford switched off:
#   -fno-exceptions         no throw, no unwinding code; failures are return values, as in C
#   -fno-rtti               no type information tables (no dynamic_cast, no typeid)
#   -fno-threadsafe-statics no lock around the first use of a function-local static: one core,
#                           no threads (and there are no such statics today)
#   -fno-use-cxa-atexit     no registering of static destructors to run at exit: main never
#                           returns, and the objects have none
# ST's and Arm's headers come in through -isystem: their C-style casts and macros are theirs,
# so the stricter C++ warnings below apply to our code only.
#
# -Wswitch-enum (C and C++): a switch over an enum must name every enumerator as its own case,
# even when it has a default. -Wswitch alone is silent once a default exists, so a forgotten
# case would fall into the default unnoticed. With -Werror a missing case fails the build. The
# default stays only for a value outside the enum (MISRA C:2012 Rule 16.4).
CXXWARNINGS = -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow \
              -Wold-style-cast -Wcast-align -Wnull-dereference -Wformat=2 -Wswitch-enum
CXXFLAGS = $(CPU) $(OPT) -g3 -std=c++17 -DSTM32G431xx $(DEFS) \
           -isystem $(DEV)/Include -isystem $(CUBE)/Drivers/CMSIS/Include -I$(COMMON) $(INCLUDES) \
           -ffunction-sections -fdata-sections -fstack-usage \
           -fno-exceptions -fno-rtti -fno-threadsafe-statics -fno-use-cxa-atexit $(CXXWARNINGS)
# g++ links a program with C++ files: it offers the C++ support library, and the linker takes
# from it only what the program calls (nothing today: nm shows no __cxa_, _Znw or malloc).
LD = $(if $(SRCS_CXX),$(CXX),$(CC))
# Linking as STM32CubeIDE projects do (nano C library, no OS). Expect "_close/_lseek/_read/
# _write is not implemented" messages: they come from the C library's unused start file, and
# --gc-sections drops that code (no malloc or stdio in the image: check with nm).
# --no-warn-rwx-segments: the toolchain marks .init_array writable, yet it sits in flash with
# the code; nothing writes it.
LDFLAGS = $(CPU) --specs=nano.specs --specs=nosys.specs -Wl,--gc-sections \
          -Wl,--no-warn-rwx-segments -Wl,-Map=$(BUILD)/$(NAME).map -T$(BUILD)/program.ld

OBJS = $(addprefix $(BUILD)/,$(notdir $(SRCS_CXX:.cpp=.o) $(SRCS:.c=.o) $(SRCS_EXT:.c=.o) $(SRCS_ST:.c=.o) $(ASM_ST:.s=.o)))
vpath %.c $(sort $(dir $(SRCS) $(SRCS_EXT) $(SRCS_ST)))
vpath %.cpp $(sort $(dir $(SRCS_CXX)))
vpath %.s $(dir $(ASM_ST))

all: $(BUILD)/$(NAME).bin

$(BUILD):
	mkdir -p $@

# The linker script goes through the C preprocessor so it can use layout.h.
$(BUILD)/program.ld: $(COMMON)/program.ld $(COMMON)/layout.h | $(BUILD)
	$(CC) -E -P -undef -x c -I$(COMMON) -DCODE_START=$(CODE_START) -DCODE_BYTES=$(CODE_BYTES) $< -o $@

$(addprefix $(BUILD)/,$(notdir $(SRCS:.c=.o))): $(BUILD)/%.o: %.c $(wildcard $(COMMON)/*.h) $(HEADERS) $(MAKEFILE_LIST) | $(BUILD)
	$(CC) $(CFLAGS) -Werror -c $< -o $@

ifneq ($(SRCS_CXX),)
$(addprefix $(BUILD)/,$(notdir $(SRCS_CXX:.cpp=.o))): $(BUILD)/%.o: %.cpp $(wildcard $(COMMON)/*.h) $(HEADERS) $(MAKEFILE_LIST) | $(BUILD)
	$(CXX) $(CXXFLAGS) -Werror -c $< -o $@
endif

$(addprefix $(BUILD)/,$(notdir $(SRCS_EXT:.c=.o) $(SRCS_ST:.c=.o))): $(BUILD)/%.o: %.c $(MAKEFILE_LIST) | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: %.s | $(BUILD)
	$(CC) $(CPU) -c $< -o $@

$(BUILD)/$(NAME).elf: $(OBJS) $(BUILD)/program.ld
	$(LD) $(OBJS) $(LDFLAGS) -o $@
	$(SIZE) $@

$(BUILD)/$(NAME).bin: $(BUILD)/$(NAME).elf
	$(OBJCOPY) -O binary $< $@

clean:
	rm -rf $(BUILD)

.PHONY: all clean
