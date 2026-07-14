#include "iss.h"

#include <cstdio>
#include <cstring>

namespace iss {

namespace {

inline int32_t sext(uint32_t v, int bits) {
    uint32_t m = 1u << (bits - 1);
    return static_cast<int32_t>((v ^ m) - m);
}
inline uint32_t bits(uint32_t v, int hi, int lo) { return (v >> lo) & ((1u << (hi - lo + 1)) - 1); }

inline int32_t imm_i(uint32_t i) { return sext(bits(i, 31, 20), 12); }
inline int32_t imm_s(uint32_t i) { return sext((bits(i, 31, 25) << 5) | bits(i, 11, 7), 12); }
inline int32_t imm_b(uint32_t i) {
    return sext((bits(i, 31, 31) << 12) | (bits(i, 7, 7) << 11) | (bits(i, 30, 25) << 5) | (bits(i, 11, 8) << 1), 13);
}
inline uint32_t imm_u(uint32_t i) { return i & 0xFFFFF000u; }
inline int32_t imm_j(uint32_t i) {
    return sext((bits(i, 31, 31) << 20) | (bits(i, 19, 12) << 12) | (bits(i, 20, 20) << 11) | (bits(i, 30, 21) << 1), 21);
}

const char* REG[32] = {"zero", "ra", "sp", "gp", "tp", "t0", "t1", "t2", "s0", "s1", "a0",
                       "a1",   "a2", "a3", "a4", "a5", "a6", "a7", "s2", "s3", "s4", "s5",
                       "s6",   "s7", "s8", "s9", "s10", "s11", "t3", "t4", "t5", "t6"};

}  // namespace

Iss::Iss() : mem(MEM_SIZE, 0) {}

void Iss::reset(uint32_t start_pc) {
    std::memset(x, 0, sizeof(x));
    pc = start_pc;
    retired = 0;
    mstatus = 0x1800;  // MPP = machine, MIE = MPIE = 0
    mtvec = mscratch = mepc = mcause = mtval = mie = 0;
}

void Iss::load(const std::vector<uint8_t>& image, uint32_t base) {
    std::fill(mem.begin(), mem.end(), 0);
    for (size_t i = 0; i < image.size() && base + i < MEM_SIZE; ++i) mem[base + i] = image[i];
}

uint32_t Iss::read32(uint32_t addr) const {
    uint32_t a = addr & (MEM_SIZE - 1) & ~3u;
    return mem[a] | (mem[a + 1] << 8) | (mem[a + 2] << 16) | (static_cast<uint32_t>(mem[a + 3]) << 24);
}

void Iss::write32(uint32_t addr, uint32_t data, uint8_t mask) {
    uint32_t a = addr & (MEM_SIZE - 1) & ~3u;
    for (int b = 0; b < 4; ++b)
        if (mask & (1 << b)) mem[a + b] = static_cast<uint8_t>(data >> (8 * b));
}

bool Iss::csr_read(uint16_t csr, uint32_t& v) const {
    switch (csr) {
        case CSR_MSTATUS:  v = mstatus; return true;
        case CSR_MISA:     v = 0x40000100; return true;  // RV32I
        case CSR_MIE:      v = mie; return true;
        case CSR_MTVEC:    v = mtvec; return true;
        case CSR_MSCRATCH: v = mscratch; return true;
        case CSR_MEPC:     v = mepc; return true;
        case CSR_MCAUSE:   v = mcause; return true;
        case CSR_MTVAL:    v = mtval; return true;
        case CSR_MIP:      v = 0; return true;
        case CSR_MHARTID:  v = 0; return true;
        default: return false;
    }
}

bool Iss::csr_write(uint16_t csr, uint32_t v) {
    switch (csr) {
        case CSR_MSTATUS:  mstatus = (v & 0x88) | 0x1800; return true;  // MIE, MPIE writable; MPP fixed
        case CSR_MISA:     return true;                                  // WARL, writes ignored
        case CSR_MIE:      mie = v & 0x888; return true;
        case CSR_MTVEC:    mtvec = v & ~3u; return true;                 // direct mode only
        case CSR_MSCRATCH: mscratch = v; return true;
        case CSR_MEPC:     mepc = v & ~3u; return true;
        case CSR_MCAUSE:   mcause = v; return true;
        case CSR_MTVAL:    mtval = v; return true;
        case CSR_MIP:      return true;
        default: return false;  // read-only (mhartid) or unknown
    }
}

void Iss::take_trap(Retire& r, uint32_t cause, uint32_t tval) {
    r.trap = true;
    r.rd_addr = 0;
    r.rd_wdata = 0;
    r.mem_addr = 0;
    r.mem_rmask = r.mem_wmask = 0;
    r.mem_rdata = r.mem_wdata = 0;
    mepc = pc;
    mcause = cause;
    mtval = tval;
    // MPIE <= MIE, MIE <= 0
    uint32_t mie_bit = (mstatus >> 3) & 1;
    mstatus = (mstatus & ~0x88u) | (mie_bit << 7) | 0x1800;
    r.pc_wdata = mtvec;
}

Retire Iss::step() {
    Retire r;
    r.order = retired;
    r.pc_rdata = pc;
    uint32_t insn = read32(pc);
    r.insn = insn;

    const uint32_t opc = bits(insn, 6, 0);
    const uint32_t rd = bits(insn, 11, 7);
    const uint32_t f3 = bits(insn, 14, 12);
    const uint32_t rs1 = bits(insn, 19, 15);
    const uint32_t rs2 = bits(insn, 24, 20);
    const uint32_t f7 = bits(insn, 31, 25);
    const uint32_t a = x[rs1], b = x[rs2];

    uint32_t next = pc + 4;
    bool wr = false;       // writes rd
    uint32_t res = 0;
    bool use1 = false, use2 = false;
    bool illegal = false;

    auto branch_to = [&](uint32_t target) -> bool {
        if (target & 3) {
            take_trap(r, CAUSE_MISALIGNED_FETCH, target);
            return false;
        }
        next = target;
        return true;
    };

    switch (opc) {
        case 0x37:  // LUI
            wr = true; res = imm_u(insn); break;
        case 0x17:  // AUIPC
            wr = true; res = pc + imm_u(insn); break;
        case 0x6F: {  // JAL
            if (!branch_to(pc + imm_j(insn))) break;
            wr = true; res = pc + 4; break;
        }
        case 0x67: {  // JALR
            if (f3 != 0) { illegal = true; break; }
            use1 = true;
            uint32_t t = (a + imm_i(insn)) & ~1u;
            if (!branch_to(t)) break;
            wr = true; res = pc + 4; break;
        }
        case 0x63: {  // branches
            use1 = use2 = true;
            bool take;
            switch (f3) {
                case 0: take = a == b; break;
                case 1: take = a != b; break;
                case 4: take = static_cast<int32_t>(a) < static_cast<int32_t>(b); break;
                case 5: take = static_cast<int32_t>(a) >= static_cast<int32_t>(b); break;
                case 6: take = a < b; break;
                case 7: take = a >= b; break;
                default: illegal = true; take = false;
            }
            if (!illegal && take) branch_to(pc + imm_b(insn));
            break;
        }
        case 0x03: {  // loads
            use1 = true;
            uint32_t addr = a + imm_i(insn);
            uint32_t size;
            switch (f3) {
                case 0: case 4: size = 1; break;
                case 1: case 5: size = 2; break;
                case 2: size = 4; break;
                default: size = 0; illegal = true;
            }
            if (illegal) break;
            if (addr & (size - 1)) { take_trap(r, CAUSE_MISALIGNED_LOAD, addr); break; }
            uint32_t word;
            if (addr >= MMIO_BASE) word = mmio_read ? mmio_read(addr & ~3u) : 0;
            else word = read32(addr);
            uint32_t sh = (addr & 3) * 8;
            uint8_t mask = static_cast<uint8_t>(((1u << size) - 1) << (addr & 3));
            r.mem_addr = addr & ~3u;
            r.mem_rmask = mask;
            r.mem_rdata = word;
            uint32_t v = word >> sh;
            switch (f3) {
                case 0: res = static_cast<uint32_t>(sext(v & 0xFF, 8)); break;
                case 1: res = static_cast<uint32_t>(sext(v & 0xFFFF, 16)); break;
                case 2: res = v; break;
                case 4: res = v & 0xFF; break;
                case 5: res = v & 0xFFFF; break;
            }
            wr = true;
            break;
        }
        case 0x23: {  // stores
            use1 = use2 = true;
            uint32_t addr = a + imm_s(insn);
            uint32_t size = f3 == 0 ? 1 : f3 == 1 ? 2 : f3 == 2 ? 4 : 0;
            if (!size) { illegal = true; break; }
            if (addr & (size - 1)) { take_trap(r, CAUSE_MISALIGNED_STORE, addr); break; }
            uint32_t sh = (addr & 3) * 8;
            uint8_t mask = static_cast<uint8_t>(((1u << size) - 1) << (addr & 3));
            uint32_t data = size == 4 ? b : size == 2 ? (b & 0xFFFF) * 0x10001u : (b & 0xFF) * 0x01010101u;
            (void)sh;
            r.mem_addr = addr & ~3u;
            r.mem_wmask = mask;
            r.mem_wdata = data;
            if (addr >= MMIO_BASE) {
                if (mmio_write) mmio_write(addr & ~3u, data, mask);
            } else {
                write32(addr, data, mask);
            }
            break;
        }
        case 0x13: {  // OP-IMM
            use1 = true;
            int32_t imm = imm_i(insn);
            uint32_t sh = rs2;
            switch (f3) {
                case 0: res = a + imm; break;
                case 2: res = static_cast<int32_t>(a) < imm; break;
                case 3: res = a < static_cast<uint32_t>(imm); break;
                case 4: res = a ^ imm; break;
                case 6: res = a | imm; break;
                case 7: res = a & imm; break;
                case 1:
                    if (f7 != 0) illegal = true;
                    res = a << sh; break;
                case 5:
                    if (f7 == 0x00) res = a >> sh;
                    else if (f7 == 0x20) res = static_cast<uint32_t>(static_cast<int32_t>(a) >> sh);
                    else illegal = true;
                    break;
            }
            wr = !illegal;
            break;
        }
        case 0x33: {  // OP
            use1 = use2 = true;
            if (f7 != 0x00 && !(f7 == 0x20 && (f3 == 0 || f3 == 5))) { illegal = true; break; }
            switch (f3) {
                case 0: res = f7 ? a - b : a + b; break;
                case 1: res = a << (b & 31); break;
                case 2: res = static_cast<int32_t>(a) < static_cast<int32_t>(b); break;
                case 3: res = a < b; break;
                case 4: res = a ^ b; break;
                case 5: res = f7 ? static_cast<uint32_t>(static_cast<int32_t>(a) >> (b & 31)) : a >> (b & 31); break;
                case 6: res = a | b; break;
                case 7: res = a & b; break;
            }
            wr = true;
            break;
        }
        case 0x0F:  // FENCE / FENCE.I: no caches, so nothing to do
            if (f3 > 1) illegal = true;
            break;
        case 0x73: {  // SYSTEM
            if (f3 == 0) {
                if (insn == 0x00000073) { take_trap(r, CAUSE_ECALL_M, 0); break; }
                if (insn == 0x00100073) { take_trap(r, CAUSE_BREAKPOINT, 0); break; }
                if (insn == 0x30200073) {  // MRET
                    next = mepc;
                    uint32_t mpie = (mstatus >> 7) & 1;
                    mstatus = (mstatus & ~0x88u) | (mpie << 3) | 0x80 | 0x1800;
                    break;
                }
                if (insn == 0x10500073) break;  // WFI: no interrupts, so a nop
                illegal = true;
                break;
            }
            if (f3 == 4) { illegal = true; break; }
            uint16_t csr = static_cast<uint16_t>(bits(insn, 31, 20));
            bool imm_form = f3 & 4;
            uint32_t src = imm_form ? rs1 : a;
            if (!imm_form) use1 = true;
            bool writes = (f3 & 3) == 1 || rs1 != 0;  // CSRRW always writes; S/C only if source != 0
            uint32_t old;
            if (!csr_read(csr, old)) { illegal = true; use1 = false; break; }
            if (writes && (csr >> 10) == 3) { illegal = true; use1 = false; break; }  // read-only space
            uint32_t nv = old;
            switch (f3 & 3) {
                case 1: nv = src; break;
                case 2: nv = old | src; break;
                case 3: nv = old & ~src; break;
            }
            if (writes) csr_write(csr, nv);
            wr = true;
            res = old;
            break;
        }
        default:
            illegal = true;
    }

    if (illegal && !r.trap) {
        use1 = use2 = false;
        take_trap(r, CAUSE_ILLEGAL_INSN, insn);
    }

    if (r.trap) {
        pc = r.pc_wdata;
        // a trapping instruction reports no register reads either
        r.rs1_addr = r.rs2_addr = 0;
        r.rs1_rdata = r.rs2_rdata = 0;
    } else {
        if (use1) { r.rs1_addr = rs1; r.rs1_rdata = a; }
        if (use2) { r.rs2_addr = rs2; r.rs2_rdata = b; }
        if (wr && rd != 0) {
            x[rd] = res;
            r.rd_addr = rd;
            r.rd_wdata = res;
        }
        r.pc_wdata = next;
        pc = next;
    }
    ++retired;
    return r;
}

std::string disasm(uint32_t i) {
    char buf[96];
    const uint32_t opc = bits(i, 6, 0), rd = bits(i, 11, 7), f3 = bits(i, 14, 12), rs1 = bits(i, 19, 15),
                   rs2 = bits(i, 24, 20), f7 = bits(i, 31, 25);
    static const char* BR[8] = {"beq", "bne", "?", "?", "blt", "bge", "bltu", "bgeu"};
    static const char* LD[8] = {"lb", "lh", "lw", "?", "lbu", "lhu", "?", "?"};
    static const char* ST[8] = {"sb", "sh", "sw", "?", "?", "?", "?", "?"};
    static const char* AI[8] = {"addi", "slli", "slti", "sltiu", "xori", "srli", "ori", "andi"};
    static const char* AR[8] = {"add", "sll", "slt", "sltu", "xor", "srl", "or", "and"};
    switch (opc) {
        case 0x37: snprintf(buf, sizeof buf, "lui %s,0x%x", REG[rd], i >> 12); break;
        case 0x17: snprintf(buf, sizeof buf, "auipc %s,0x%x", REG[rd], i >> 12); break;
        case 0x6F: snprintf(buf, sizeof buf, "jal %s,%d", REG[rd], imm_j(i)); break;
        case 0x67: snprintf(buf, sizeof buf, "jalr %s,%d(%s)", REG[rd], imm_i(i), REG[rs1]); break;
        case 0x63: snprintf(buf, sizeof buf, "%s %s,%s,%d", BR[f3], REG[rs1], REG[rs2], imm_b(i)); break;
        case 0x03: snprintf(buf, sizeof buf, "%s %s,%d(%s)", LD[f3], REG[rd], imm_i(i), REG[rs1]); break;
        case 0x23: snprintf(buf, sizeof buf, "%s %s,%d(%s)", ST[f3], REG[rs2], imm_s(i), REG[rs1]); break;
        case 0x13:
            if (f3 == 5 && f7 == 0x20) snprintf(buf, sizeof buf, "srai %s,%s,%u", REG[rd], REG[rs1], rs2);
            else if (f3 == 1 || f3 == 5) snprintf(buf, sizeof buf, "%s %s,%s,%u", AI[f3], REG[rd], REG[rs1], rs2);
            else snprintf(buf, sizeof buf, "%s %s,%s,%d", AI[f3], REG[rd], REG[rs1], imm_i(i));
            break;
        case 0x33:
            snprintf(buf, sizeof buf, "%s %s,%s,%s",
                     f7 == 0x20 ? (f3 == 0 ? "sub" : "sra") : AR[f3], REG[rd], REG[rs1], REG[rs2]);
            break;
        case 0x0F: snprintf(buf, sizeof buf, "%s", f3 == 1 ? "fence.i" : "fence"); break;
        case 0x73:
            if (i == 0x00000073) snprintf(buf, sizeof buf, "ecall");
            else if (i == 0x00100073) snprintf(buf, sizeof buf, "ebreak");
            else if (i == 0x30200073) snprintf(buf, sizeof buf, "mret");
            else if (f3) snprintf(buf, sizeof buf, "csr%s%s %s,0x%x,%s", (f3 & 3) == 1 ? "rw" : (f3 & 3) == 2 ? "rs" : "rc",
                                  f3 & 4 ? "i" : "", REG[rd], i >> 20, f3 & 4 ? std::to_string(rs1).c_str() : REG[rs1]);
            else snprintf(buf, sizeof buf, "system 0x%08x", i);
            break;
        default: snprintf(buf, sizeof buf, ".word 0x%08x", i);
    }
    return buf;
}

std::string describe(const Retire& r) {
    char buf[256];
    snprintf(buf, sizeof buf,
             "#%llu pc=%08x insn=%08x %-24s trap=%d rs1=x%u:%08x rs2=x%u:%08x rd=x%u:%08x next=%08x "
             "mem=%08x r%x:%08x w%x:%08x",
             static_cast<unsigned long long>(r.order), r.pc_rdata, r.insn, disasm(r.insn).c_str(), r.trap, r.rs1_addr,
             r.rs1_rdata, r.rs2_addr, r.rs2_rdata, r.rd_addr, r.rd_wdata, r.pc_wdata, r.mem_addr, r.mem_rmask,
             r.mem_rdata, r.mem_wmask, r.mem_wdata);
    return buf;
}

}  // namespace iss
