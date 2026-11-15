# Bug log

Real bugs found while bringing the design up, in the order they showed up.

## 1. First instruction after reset never retired (RTL, found by rv32ui in lockstep)

Every compliance test failed at retirement 0: the RTL's first retired pc was 0x4, the ISS's 0x0.
The fetch-valid flag was a register that went high one cycle after reset, but the synchronous RAM
had already latched address 0 during reset, so the instruction at 0 was on the bus in the first
cycle and got marked invalid. Fix: fetch is valid whenever reset is released. The same run showed
the register file was not reset (values carried over between programs), which the ISS treats as
zero; the register file now resets.

## 2. STATUS read right after a CMD store saw "idle" (RTL, found by the driver test)

The bare-metal driver NACKed every first transaction. A waveform showed the address byte going out
as 0x00: `wait_idle()` returned immediately, so the next `TXDATA` store (the register offset)
overwrote the address before the controller loaded it. The STATUS load sat in EX while the CMD
store was in MEM, so the load's address was registered at the same edge the store landed and the
read mux still saw the old idle state. Fix in `i2c_master.sv`: a CMD write in flight counts as busy.
Retries had been hiding it: the second attempt always worked.

## 3. All-zero reads from a stuck bus passed the checksum (driver, found by fault injection)

With the stuck-SDA fault, 100 of 200 injected faults came back "recovered" with the wrong module
data. A slave holding SDA low returns 0x00 for every byte, and SFF-8472's checksums are plain sums,
so 96 zero bytes pass both (0 == 0). Fix in `sfp.c`: also require the identifier byte to be 0x03
(SFP/SFP+) before accepting a read. After the fix all 200 faults recover: 100 NACK bursts through
retries with backoff, 100 stuck buses through the 9-clock recovery.

## Bug injection finding: forwarding from x0 can't break anything

18 deliberate RTL bugs, 17 caught (see `bug_injection_results.md`). The miss is `BUG_FWD_X0`, which
lets an instruction that writes x0 forward its result. No test caught it because the operand mux
forces any operand read from x0 to zero after forwarding, so the forwarded value never reaches the
ALU. The x0 check in the forwarding condition is redundant; it stays in because it keeps the
coverage numbers honest (a "forward" from x0 is not counted as one).

Two of the 17 were caught only by the random programs, not by rv32ui: `jalr` keeping bit 0 of the
target, and `mepc` getting pc+4. The compliance suite never jumps to an odd `jalr` target and never
traps, so random testing is what covers them.
