# DE10-Lite bring-up

Not run on the board yet. Steps:

1. Build the program: `make -C sw PRESCALE=125` (100 kHz I2C at 50 MHz), then
   `python3 scripts/bin2hex.py sw/build/i2c_test.bin fpga/de10_lite/program.hex`.
   For a quick smoke test without an SFP+ cage use `sw/build/bench.bin` instead.
2. New Quartus Prime Lite project, device 10M50DAF484C7G, top-level entity `top`.
   Add `fpga/de10_lite/top.sv`, `rtl/core.sv`, `rtl/soc.sv`, `rtl/ram_dp.sv`, `rtl/i2c_master.sv`,
   and `fpga/de10_lite/program.hex`. Source `fpga/de10_lite/pins.tcl`.
3. Compile, then read the slow 85C model Fmax in TimeQuest. Record it in `docs/results.md`.
   If it is under 50 MHz, add an ALTPLL at 25 MHz (see the comment in `top.sv`).
4. Program over USB-Blaster. Press KEY0. LEDR9 = pass; all of LEDR[8:0] lit = fail.

Yosys (no timing) maps the core alone to about 5,400 logic cells and 2,150 flip-flops on
MAX 10, around 11% of the 10M50. Quartus usually lands lower; record its number too.
