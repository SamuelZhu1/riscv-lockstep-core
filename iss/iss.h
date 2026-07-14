// RV32I instruction set simulator used as the golden model.
// One call to step() executes one instruction and returns the same
// retirement record the RTL core puts on its RVFI port, so the two can be
// compared field by field.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace iss {

constexpr uint32_t MEM_SIZE  = 64 * 1024;   // unified RAM at 0x0000_0000
constexpr uint32_t MMIO_BASE = 0x00020000;  // everything at or above is MMIO
constexpr uint32_t MMIO_END  = 0x00030000;

// CSR numbers (machine mode only)
enum : uint16_t {
    CSR_MSTATUS  = 0x300,
    CSR_MISA     = 0x301,
    CSR_MIE      = 0x304,
    CSR_MTVEC    = 0x305,
    CSR_MSCRATCH = 0x340,
    CSR_MEPC     = 0x341,
    CSR_MCAUSE   = 0x342,
    CSR_MTVAL    = 0x343,
    CSR_MIP      = 0x344,
    CSR_MHARTID  = 0xF14,
};

// mcause values for synchronous exceptions
enum : uint32_t {
    CAUSE_MISALIGNED_FETCH = 0,
    CAUSE_ILLEGAL_INSN     = 2,
    CAUSE_BREAKPOINT       = 3,
    CAUSE_MISALIGNED_LOAD  = 4,
    CAUSE_MISALIGNED_STORE = 6,
    CAUSE_ECALL_M          = 11,
};

// Mirrors the RVFI subset the core exposes. Memory fields are word aligned:
// mem_addr has its low 2 bits cleared and the masks / data sit in their
// byte lanes, the same way the RTL drives its data bus.
struct Retire {
    uint64_t order = 0;
    uint32_t insn = 0;
    bool     trap = false;
    uint32_t pc_rdata = 0, pc_wdata = 0;
    uint8_t  rs1_addr = 0, rs2_addr = 0, rd_addr = 0;
    uint32_t rs1_rdata = 0, rs2_rdata = 0, rd_wdata = 0;
    uint32_t mem_addr = 0;
    uint8_t  mem_rmask = 0, mem_wmask = 0;
    uint32_t mem_rdata = 0, mem_wdata = 0;
};

std::string describe(const Retire& r);
std::string disasm(uint32_t insn);

class Iss {
public:
    Iss();
    void reset(uint32_t pc = 0);
    void load(const std::vector<uint8_t>& image, uint32_t base = 0);

    // Executes one instruction. mmio_read is called for loads in the MMIO
    // range; the lockstep harness answers it with the value the RTL saw, so
    // device registers (cycle counter, I2C status) never cause a mismatch.
    Retire step();

    std::function<uint32_t(uint32_t addr)> mmio_read;
    std::function<void(uint32_t addr, uint32_t data, uint8_t mask)> mmio_write;

    uint32_t x[32] = {};
    uint32_t pc = 0;
    uint64_t retired = 0;

    // machine CSRs
    uint32_t mstatus = 0x1800, mtvec = 0, mscratch = 0, mepc = 0, mcause = 0, mtval = 0, mie = 0;

    std::vector<uint8_t> mem;

    uint32_t read32(uint32_t addr) const;
    void write32(uint32_t addr, uint32_t data, uint8_t mask);

private:
    bool csr_read(uint16_t csr, uint32_t& val) const;
    bool csr_write(uint16_t csr, uint32_t val);
    void take_trap(Retire& r, uint32_t cause, uint32_t tval);
};

}  // namespace iss
