# Builds the official rv32ui compliance tests for this core's environment.
# ma_data is left out: it checks misaligned load/store support, and this core
# raises the misaligned exceptions instead (which is allowed by the spec and
# tested by the random generator).

RVPREFIX ?= riscv64-unknown-elf-
TESTS_DIR := riscv-tests
ifeq ($(wildcard $(TESTS_DIR)/isa/rv32ui/add.S),)
$(shell git clone -q --depth 1 https://github.com/riscv-software-src/riscv-tests.git $(TESTS_DIR))
endif

TESTS := simple add addi and andi auipc beq bge bgeu blt bltu bne fence_i jal jalr \
         lb lbu lh lhu lw ld_st lui or ori sb sh sw st_ld sll slli slt slti sltiu sltu \
         sra srai srl srli sub xor xori
CFLAGS := -march=rv32i_zicsr_zifencei -mabi=ilp32 -nostdlib -nostartfiles -static \
          -Ienv -I$(TESTS_DIR)/isa/macros/scalar -Tenv/link.ld -Wl,--no-warn-rwx-segments

all: $(addprefix build/rv32ui-,$(addsuffix .bin,$(TESTS)))

build/rv32ui-%.elf: $(TESTS_DIR)/isa/rv32ui/%.S env/riscv_test.h env/link.ld
	@mkdir -p build
	$(RVPREFIX)gcc $(CFLAGS) -o $@ $<

build/rv32ui-%.bin: build/rv32ui-%.elf
	$(RVPREFIX)objcopy -O binary $< $@

clean:
	rm -rf build
