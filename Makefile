# Top-level build. Targets:
#   make iss-test      ISS unit tests
#   make sim           Verilator lockstep simulator (build/obj/Vtb_top)
#   make compliance    rv32ui compliance suite in lockstep
#   make random N=200  random lockstep regression
#   make sw            bare-metal programs (I2C driver test, benchmark)
#   make driver        run the I2C driver test on the core
#   make bench         CPI benchmark
#   make ci            what GitHub Actions runs

VERILATOR ?= verilator
CXX       ?= g++
BUG       ?=
OBJ       := build/obj$(if $(BUG),_$(BUG),)
RTL       := rtl/core.sv rtl/i2c_master.sv rtl/ram_dp.sv rtl/soc.sv tb/i2c_eeprom.sv tb/tb_top.sv
N         ?= 200
LEN       ?= 3000
SEED      ?= 1

VFLAGS := --cc --exe --build -j 0 -O3 --top-module tb_top -DSIM_DPI_RAM \
          -Wall -Wno-fatal -Wno-DECLFILENAME -Wno-UNUSEDSIGNAL -Wno-UNUSEDPARAM -Wno-PINCONNECTEMPTY \
          -CFLAGS "-std=c++17 -O2" $(if $(BUG),-D$(BUG),) $(if $(TRACE),--trace,)

.PHONY: all iss-test sim compliance random sw driver bench ci clean

all: sim

build/test_iss: iss/test_iss.cpp iss/iss.cpp iss/iss.h gen/encode.h
	@mkdir -p build
	$(CXX) -std=c++17 -O2 -Wall -Wextra -o $@ iss/test_iss.cpp iss/iss.cpp

iss-test: build/test_iss
	./build/test_iss

$(OBJ)/Vtb_top: $(RTL) tb/sim_main.cpp iss/iss.cpp iss/iss.h
	@mkdir -p $(OBJ)
	$(VERILATOR) $(VFLAGS) --Mdir $(OBJ) $(RTL) $(abspath tb/sim_main.cpp) $(abspath iss/iss.cpp) > $(OBJ)/build.log 2>&1 || (tail -40 $(OBJ)/build.log; false)

sim: $(OBJ)/Vtb_top

build/gen: gen/gen.cpp gen/encode.h
	@mkdir -p build
	$(CXX) -std=c++17 -O2 -Wall -o $@ gen/gen.cpp

compliance: sim
	$(MAKE) -C tests
	./$(OBJ)/Vtb_top --quiet tests/build/*.bin

random: sim build/gen
	@rm -rf build/rand && mkdir -p build/rand
	./build/gen --out build/rand --count $(N) --len $(LEN) --seed $(SEED)
	./$(OBJ)/Vtb_top --quiet --cov build/coverage.json --summary build/random_summary.json build/rand/*.bin

sw:
	$(MAKE) -C sw

driver: sim sw
	./$(OBJ)/Vtb_top sw/build/i2c_test.bin

bench: sim sw
	./$(OBJ)/Vtb_top sw/build/bench.bin

ci: iss-test compliance random driver bench

clean:
	rm -rf build tests/build sw/build
