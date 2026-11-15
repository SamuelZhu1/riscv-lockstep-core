# riscv-lockstep-core

A 5-stage pipelined RV32I CPU in SystemVerilog, checked instruction by instruction against a C++
reference model, plus a bare-metal I2C driver for SFP+ optical modules running on it.

I wanted to see if I could verify a CPU the way a real team does: a golden model in C++, every
retired instruction compared in lockstep, random programs aimed at the pipeline's weak spots, and
deliberately broken versions of the design to prove the tests would notice. Then I put real
firmware on it: the I2C driver from my SFP+ module firmware, talking to a simulated module.

## What's in it

```
                 +------------------ Verilator binary -------------------+
 program ------> |  RTL core (SV)  --- RVFI retire port --->  lockstep    |
 (.bin)          |  IF ID EX MEM WB                           comparator  |
                 |     |   |                                      ^       |
                 |  64 KB RAM, MMIO, I2C master                   |       |
                 |     |  open-drain bus                    C++ ISS       |
                 |  SFF-8472 module model (0x50, 0x51)      (golden)      |
                 +--------------------------------------------------------+
  gen (C++ random programs), rv32ui compliance, bare-metal C (RISC-V GCC)
```

- `rtl/core.sv`: IF/ID/EX/MEM/WB, full forwarding (EX/MEM and MEM/WB to EX, WB to ID), one-cycle
  load-use stall, branches and jumps resolved in EX with a 2-instruction flush, precise
  machine-mode traps, CSRs. Memories are synchronous (MAX 10 M9K style), so the core also has a
  store-to-load bypass for a load that reads the word a store is writing in the same cycle.
- `rtl/i2c_master.sv`: memory-mapped I2C master with a bit-bang mode for bus recovery.
- `iss/`: RV32I instruction set simulator; `step()` returns the same record the RTL retires.
- `tb/`: Verilator harness (lockstep compare, coverage, last-32-retirement dump on a mismatch) and
  an SFP+ EEPROM model with NACK and stuck-SDA fault injection.
- `gen/`: constrained-random program generator with a hazard bias.
- `sw/`: crt0, linker script, the SFP+ I2C driver and SDK subset, a CPI benchmark.
- `scripts/bug_campaign.py`: rebuilds the core with each of 18 deliberate bugs and requires a failure.
- `fpga/de10_lite/`: board top, pins, constraints, bring-up steps.

## Results

| | |
|---|---|
| rv32ui compliance | 41 of 41 pass in lockstep |
| Random lockstep regression | 12.1M instructions over 2,000 programs, 0 mismatches |
| Bug injection | 17 of 18 caught (the 18th can't change behaviour, see `docs/bug_log.md`) |
| Bugs found bringing it up | 3: a reset fetch bug, an I2C status race, a driver checksum hole |
| I2C fault injection | 200 of 200 faults recovered without a reset |
| CPI (benchmark) | 1.39 |
| Fmax on the DE10-Lite | not measured yet |

Details: `docs/results.md`, `docs/bug_log.md`, `docs/verification_plan.md`.

## Running it

Needs Verilator 5, a C++17 compiler, and `riscv64-unknown-elf-gcc` (Ubuntu:
`apt install verilator gcc-riscv64-unknown-elf`).

```
make iss-test            # ISS unit tests
make compliance          # rv32ui in lockstep (fetches riscv-tests the first time)
make random N=200        # random programs in lockstep, coverage in build/coverage.json
make driver              # SFP+ I2C driver on the core, with fault injection
make bench               # CPI benchmark
python3 scripts/bug_campaign.py
```

A mismatch prints the field that differs and the last 32 retirements from both the RTL and the ISS.
`make sim TRACE=1` builds a waveform-capable simulator (`--trace out.vcd`).
