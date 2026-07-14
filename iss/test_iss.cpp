// Unit tests for the ISS: every RV32I instruction against values worked out
// by hand, plus the trap and CSR behaviour the core has to match.
#include <cstdio>
#include <vector>

#include "../gen/encode.h"
#include "iss.h"

using namespace enc;

static int failures = 0, checks = 0;
#define CHECK_EQ(a, b)                                                                                    \
    do {                                                                                                  \
        ++checks;                                                                                         \
        auto _a = (a);                                                                                    \
        auto _b = (b);                                                                                    \
        if (_a != _b) {                                                                                   \
            ++failures;                                                                                   \
            std::printf("FAIL %s:%d  %s = 0x%llx, expected 0x%llx\n", __FILE__, __LINE__, #a,             \
                        static_cast<unsigned long long>(_a), static_cast<unsigned long long>(_b));        \
        }                                                                                                 \
    } while (0)

// Loads a program, sets registers, runs n instructions.
struct Run {
    iss::Iss cpu;
    std::vector<iss::Retire> log;
    Run(std::initializer_list<uint32_t> prog, std::initializer_list<std::pair<int, uint32_t>> regs = {}) {
        std::vector<uint8_t> img;
        for (uint32_t w : prog)
            for (int k = 0; k < 4; ++k) img.push_back(static_cast<uint8_t>(w >> (8 * k)));
        cpu.load(img);
        cpu.reset(0);
        for (auto& [r, v] : regs) cpu.x[r] = v;
        for (size_t k = 0; k < prog.size(); ++k) log.push_back(cpu.step());
    }
};

static void test_alu() {
    Run t({add(3, 1, 2), sub(4, 1, 2), xor_(5, 1, 2), or_(6, 1, 2), and_(7, 1, 2), slt(8, 1, 2), sltu(9, 1, 2),
           sll(10, 2, 11), srl(12, 1, 11), sra(13, 1, 11)},
          {{1, 0x80000000}, {2, 0x00000001}, {11, 4}});
    CHECK_EQ(t.cpu.x[3], 0x80000001u);
    CHECK_EQ(t.cpu.x[4], 0x7FFFFFFFu);
    CHECK_EQ(t.cpu.x[5], 0x80000001u);
    CHECK_EQ(t.cpu.x[6], 0x80000001u);
    CHECK_EQ(t.cpu.x[7], 0u);
    CHECK_EQ(t.cpu.x[8], 1u);   // -2^31 < 1 signed
    CHECK_EQ(t.cpu.x[9], 0u);   // 0x80000000 > 1 unsigned
    CHECK_EQ(t.cpu.x[10], 16u);
    CHECK_EQ(t.cpu.x[12], 0x08000000u);
    CHECK_EQ(t.cpu.x[13], 0xF8000000u);
}

static void test_imm() {
    Run t({addi(1, 0, -1), slti(2, 1, 0), sltiu(3, 1, 0), xori(4, 1, 0x0F0), ori(5, 0, 0x7FF), andi(6, 1, 0x0F0),
           slli(7, 5, 21), srli(8, 1, 28), srai(9, 1, 28), lui(10, 0x12345), auipc(11, 1)});
    CHECK_EQ(t.cpu.x[1], 0xFFFFFFFFu);
    CHECK_EQ(t.cpu.x[2], 1u);
    CHECK_EQ(t.cpu.x[3], 0u);  // sltiu against 0: nothing is below 0
    CHECK_EQ(t.cpu.x[4], 0xFFFFFF0Fu);
    CHECK_EQ(t.cpu.x[5], 0x7FFu);
    CHECK_EQ(t.cpu.x[6], 0xF0u);
    CHECK_EQ(t.cpu.x[7], 0xFFE00000u);
    CHECK_EQ(t.cpu.x[8], 0xFu);
    CHECK_EQ(t.cpu.x[9], 0xFFFFFFFFu);
    CHECK_EQ(t.cpu.x[10], 0x12345000u);
    CHECK_EQ(t.cpu.x[11], 0x1000u + 40u);  // auipc at pc=40
}

static void test_x0() {
    Run t({addi(0, 0, 5), add(1, 0, 0)});
    CHECK_EQ(t.cpu.x[0], 0u);
    CHECK_EQ(t.log[0].rd_addr, 0);  // write to x0 reports no destination
}

static void test_branches() {
    // beq taken skips one instruction
    Run t({beq(1, 2, 8), addi(3, 0, 1), addi(4, 0, 1)}, {{1, 7}, {2, 7}});
    CHECK_EQ(t.log[0].pc_wdata, 8u);
    // each branch condition, signed and unsigned
    struct Case { uint32_t insn; uint32_t a, b; bool taken; };
    Case cases[] = {{bne(1, 2, 8), 1, 2, true},       {bne(1, 2, 8), 2, 2, false},
                    {blt(1, 2, 8), 0xFFFFFFFF, 0, true}, {bltu(1, 2, 8), 0xFFFFFFFF, 0, false},
                    {bge(1, 2, 8), 0, 0xFFFFFFFF, true}, {bgeu(1, 2, 8), 0, 0xFFFFFFFF, false},
                    {bge(1, 2, 8), 5, 5, true},          {bgeu(1, 2, 8), 5, 5, true}};
    for (auto& c : cases) {
        Run r({c.insn}, {{1, c.a}, {2, c.b}});
        CHECK_EQ(r.log[0].pc_wdata, c.taken ? 8u : 4u);
    }
}

static void test_jumps() {
    Run t({jal(1, 12), 0, 0, jalr(2, 1, 7)});
    CHECK_EQ(t.log[0].rd_wdata, 4u);
    CHECK_EQ(t.log[0].pc_wdata, 12u);
    // jalr target = (4 + 7) & ~1 = 10, misaligned: traps, no link written
    CHECK_EQ(t.log[1].trap, true);
    CHECK_EQ(t.cpu.mcause, 0u);
    CHECK_EQ(t.cpu.mtval, 10u);
    CHECK_EQ(t.cpu.x[2], 0u);
}

static void test_loads_stores() {
    Run t({lui(1, 0x1), addi(2, 0, -2), sw(2, 1, 0), sb(0, 1, 1), lw(3, 1, 0), lb(4, 1, 0), lbu(5, 1, 0), lh(6, 1, 2),
           lhu(7, 1, 2), sh(2, 1, 6), lw(8, 1, 4)});
    // memory at 0x1000 after sw: FE FF FF FF, after sb 0 at +1: FE 00 FF FF
    CHECK_EQ(t.cpu.x[3], 0xFFFF00FEu);
    CHECK_EQ(t.cpu.x[4], 0xFFFFFFFEu);
    CHECK_EQ(t.cpu.x[5], 0xFEu);
    CHECK_EQ(t.cpu.x[6], 0xFFFFFFFFu);
    CHECK_EQ(t.cpu.x[7], 0xFFFFu);
    CHECK_EQ(t.cpu.x[8], 0xFFFE0000u);
    CHECK_EQ(t.log[3].mem_wmask, 0x2);
    CHECK_EQ(t.log[3].mem_addr, 0x1000u);
    CHECK_EQ(t.log[7].mem_rmask, 0xC);
}

static void test_misaligned() {
    Run t({lui(1, 0x1), lw(2, 1, 2)});
    CHECK_EQ(t.log[1].trap, true);
    CHECK_EQ(t.cpu.mcause, 4u);
    CHECK_EQ(t.cpu.mtval, 0x1002u);
    CHECK_EQ(t.cpu.mepc, 4u);
    Run s({lui(1, 0x1), sh(0, 1, 1)});
    CHECK_EQ(s.cpu.mcause, 6u);
    CHECK_EQ(s.log[1].mem_wmask, 0);  // a trapping store writes nothing
}

static void test_csr_and_traps() {
    // mtvec = 0x100, ecall traps there, handler reads mepc
    Run t({addi(1, 0, 0x100), csrrw(0, iss::CSR_MTVEC, 1), csrrsi(2, iss::CSR_MSTATUS, 8), ECALL});
    CHECK_EQ(t.cpu.mtvec, 0x100u);
    CHECK_EQ(t.log[3].trap, true);
    CHECK_EQ(t.log[3].pc_wdata, 0x100u);
    CHECK_EQ(t.cpu.mepc, 12u);
    CHECK_EQ(t.cpu.mcause, 11u);
    CHECK_EQ(t.cpu.mstatus & 0x88, 0x80u);  // MIE moved to MPIE
    CHECK_EQ(t.log[2].rd_wdata, 0x1800u);  // old mstatus

    Run m({addi(1, 0, 0x40), csrrw(0, iss::CSR_MEPC, 1), MRET});
    CHECK_EQ(m.log[2].pc_wdata, 0x40u);

    Run il({0x00000000});
    CHECK_EQ(il.log[0].trap, true);
    CHECK_EQ(il.cpu.mcause, 2u);

    Run ro({csrrw(1, iss::CSR_MHARTID, 0)});  // writing a read-only CSR is illegal
    CHECK_EQ(ro.cpu.mcause, 2u);
    Run rd_ok({csrrs(1, iss::CSR_MHARTID, 0)});  // reading it is fine
    CHECK_EQ(rd_ok.log[0].trap, false);

    Run bad_shift({0x02009093});  // slli with shamt[5] set is illegal on RV32
    CHECK_EQ(bad_shift.cpu.mcause, 2u);

    Run eb({EBREAK});
    CHECK_EQ(eb.cpu.mcause, 3u);
}

static void test_rvfi_fields() {
    Run t({add(3, 1, 2)}, {{1, 5}, {2, 6}});
    const auto& r = t.log[0];
    CHECK_EQ(r.rs1_addr, 1);
    CHECK_EQ(r.rs2_addr, 2);
    CHECK_EQ(r.rs1_rdata, 5u);
    CHECK_EQ(r.rs2_rdata, 6u);
    CHECK_EQ(r.rd_wdata, 11u);
    Run l({lui(5, 1)});
    CHECK_EQ(l.log[0].rs1_addr, 0);  // LUI reads no registers
}

int main() {
    test_alu();
    test_imm();
    test_x0();
    test_branches();
    test_jumps();
    test_loads_stores();
    test_misaligned();
    test_csr_and_traps();
    test_rvfi_fields();
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
