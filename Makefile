# Builds the recompiled-side mod code (build/mod.elf).
# Requires clang with a MIPS target (LLVM 18.x recommended; the 19.1.0 release
# binaries have broken MIPS support) and ld.lld.
#
# After this, run RecompModTool to produce the .nrm:
#   RecompModTool mod.toml build

BUILD_DIR := build

CC ?= clang
LD ?= ld.lld

TARGET := $(BUILD_DIR)/mod.elf

LDSCRIPT := mod.ld
ARCHFLAGS := -target mips -mips2 -mabi=32 -O2 -G0 -mno-abicalls -mno-odd-spreg -mno-check-zero-division \
             -fomit-frame-pointer -ffast-math -fno-unsafe-math-optimizations -fno-builtin-memset
WARNFLAGS := -Wall -Wextra -Wno-incompatible-library-redeclaration -Wno-unused-parameter -Wno-unknown-pragmas \
             -Wno-unused-variable -Wno-missing-braces -Wno-unsupported-floating-point-opt -Werror=section
CFLAGS   := $(ARCHFLAGS) $(WARNFLAGS) -D_LANGUAGE_C -nostdinc -ffunction-sections
CPPFLAGS := -DMIPS -I include
LDFLAGS  := -nostdlib -T $(LDSCRIPT) -Map $(BUILD_DIR)/mod.map --unresolved-symbols=ignore-all --emit-relocs -e 0 --no-nmagic -gc-sections

C_SRCS := $(wildcard src/*.c)
C_OBJS := $(addprefix $(BUILD_DIR)/, $(C_SRCS:.c=.o))

all: $(TARGET)

$(TARGET): $(C_OBJS) $(LDSCRIPT) | $(BUILD_DIR)
	$(LD) $(C_OBJS) $(LDFLAGS) -o $@

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)/src

$(C_OBJS): $(BUILD_DIR)/%.o : %.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(CPPFLAGS) $< -c -o $@

clean:
	rm -rf $(BUILD_DIR)

.PHONY: all clean
