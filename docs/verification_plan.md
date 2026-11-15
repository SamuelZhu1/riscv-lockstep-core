# Verification plan

| Feature | Checked by | Coverage |
|---|---|---|
| Every RV32I instruction | rv32ui in lockstep; random programs | per-instruction retire counts |
| EX/MEM -> EX forwarding (rs1, rs2) | lockstep compares operand values on every retirement | `fwd_exmem_rs1`, `fwd_exmem_rs2` |
| MEM/WB -> EX forwarding (rs1, rs2) | lockstep | `fwd_memwb_rs1`, `fwd_memwb_rs2` |
| WB -> ID register file bypass | lockstep | `wb_id_bypass` |
| Load-use stall | lockstep; generator puts a use right after loads | `load_use_stall` |
| Taken / not-taken branch, jal, jalr flush | lockstep compares next pc | `branch_taken_flush`, `branch_not_taken`, `jal_flush`, `jalr_flush` |
| Stall and redirect in the same cycle | lockstep | `stall_and_flush` |
| Store then load of the same word (RAM old-data bypass) | lockstep compares load data | `store_load_bypass` |
| Writes to x0 dropped | lockstep (rd and later reads of x0) | `write_x0` |
| Exceptions: misaligned fetch, illegal, ebreak, misaligned load/store, ecall | lockstep compares trap flag and handler pc; handler reads mcause/mepc | `exception`, trap cause counts |
| mret | lockstep | `mret` |
| CSRs (mstatus, mtvec, mscratch, mepc, mcause, mtval, misa, mhartid; read-only and unknown CSRs trap) | ISS unit tests; lockstep | csr instruction counts |
| fence.i with self-modifying code | rv32ui fence_i | `fence_i` |
| I2C controller: START, repeated START, byte write/read, ACK/NACK, STOP | driver test against the SFF-8472 model | transfers in the driver test |
| I2C fault handling: NACK retries with backoff, stuck-SDA recovery | driver fault-injection loop | faults injected / recovered |
| Testbench quality | bug injection: every bug must make a check fail | caught / injected |
