# Results

Measured in simulation (Verilator 5.020) unless marked. Board numbers are still to do.

| What | Result | How |
|---|---|---|
| rv32ui compliance | 41 of 41 tests pass in lockstep | `make compliance` (ma_data left out: the core traps on misaligned accesses) |
| Random lockstep | 12,072,931 instructions in 2,000 programs, 0 mismatches | `gen` seeds 101-104, 7,000 instructions each |
| Bug injection | 17 of 18 RTL bugs caught; the 18th cannot change behaviour (see bug_log.md) | `scripts/bug_campaign.py` |
| Real bugs found in bring-up | 3 (1 core, 1 I2C controller, 1 driver) | `docs/bug_log.md` |
| I2C fault recovery | 200 of 200 injected faults recovered without a reset (100 NACK bursts, 100 stuck SDA) | `make driver` |
| dBm conversion (fixed point) | worst error 0.01 dB on 10 reference powers | `make driver` |
| CPI | 1.39 on the benchmark (sort, CRC-32, list walk) | `make bench` |
| ISS unit tests | 73 checks pass | `make iss-test` |
| Logic (Yosys, core only, no timing) | about 5,400 LUTs and 2,150 flip-flops on MAX 10 | `yosys synth_intel -family max10` |
| Fmax on the DE10-Lite | not measured yet | Quartus TimeQuest, slow 85C model |

## Coverage from the random regression

| Event | Count |
|---|---|
| fwd_exmem_rs1 | 1,570,730 |
| fwd_exmem_rs2 | 580,404 |
| fwd_memwb_rs1 | 1,142,980 |
| fwd_memwb_rs2 | 659,868 |
| load_use_stall | 646,296 |
| branch_taken_flush | 529,281 |
| branch_not_taken | 529,642 |
| jal_flush | 289,513 |
| jalr_flush | 264,605 |
| store_load_bypass | 323,288 |
| exception | 247,011 |
| mret | 247,011 |
| wb_id_bypass | 1,350,939 |
| write_x0 | 575,822 |
| stall_and_flush | 20,746 |
| fence_i | 23,926 |

All 37 RV32I instructions, the 6 CSR instructions, ecall, ebreak, mret, fence and fence.i retired.
Trap causes hit: misaligned fetch (0), illegal instruction (2), breakpoint (3), misaligned load (4), misaligned store (6), ecall (11).
