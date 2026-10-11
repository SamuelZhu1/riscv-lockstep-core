# Bare-metal programs for the core, built with the RISC-V GCC cross compiler.
RVPREFIX ?= riscv64-unknown-elf-
CC       := $(RVPREFIX)gcc
OBJCOPY  := $(RVPREFIX)objcopy
PRESCALE ?= 8
CFLAGS   := -march=rv32i_zicsr -mabi=ilp32 -O2 -ffreestanding -fno-builtin -nostdlib -nostartfiles \
            -Wall -Wextra -Icommon -DPRESCALE=$(PRESCALE)
LDFLAGS  := -T common/link.ld -Wl,--gc-sections -Wl,--no-warn-rwx-segments -lgcc

COMMON := common/crt0.S common/print.c
DRIVER := i2c_driver/sfp_i2c.c i2c_driver/sfp.c i2c_driver/main.c

all: build/i2c_test.bin build/bench.bin

build/i2c_test.elf: $(COMMON) $(DRIVER) i2c_driver/*.h common/*.h common/link.ld
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ $(COMMON) $(DRIVER) $(LDFLAGS)

build/bench.elf: $(COMMON) bench/bench.c common/*.h common/link.ld
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ $(COMMON) bench/bench.c $(LDFLAGS)

build/%.bin: build/%.elf
	$(OBJCOPY) -O binary $< $@
	@$(RVPREFIX)size $<

clean:
	rm -rf build
