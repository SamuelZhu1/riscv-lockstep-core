// Constrained-random RV32I program generator.
//
// Each program: a prologue that sets the trap vector and fills registers
// with random values, a body of random instructions, an epilogue that
// reports pass through TEST_STATUS, and a trap handler that skips the
// faulting instruction. Control flow only goes forward, so every program
// terminates. A random 4 KB data block at 0x8000 gives loads real values.
//
// Bias toward the cases a pipeline gets wrong: with probability --hazard a
// source register is one of the last three destinations, stores are often
// followed by a load of the same word, and loads are often followed by a
// use of their result.
//
// usage: gen --out DIR --count N --len L --seed S [--hazard 0.5]

#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <deque>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#include "encode.h"

using namespace enc;

namespace {

constexpr uint32_t DATA_BASE = 0x8000;
constexpr uint32_t DATA_SIZE = 0x1000;
constexpr uint32_t BASE_REG = 31;   // holds DATA_BASE, never written by the body
constexpr uint32_t TRAP_T0 = 29;    // handler scratch
constexpr uint32_t TRAP_T1 = 28;

// A group is one or more instructions that must run together (auipc+jalr,
// load+use). Branches and jumps always land on a group boundary, so a jump
// never skips the auipc that a later jalr depends on.
struct Group {
    std::vector<uint32_t> w;
    enum Kind { PLAIN, BRANCH, JAL, JALR } kind = PLAIN;
    uint32_t f3 = 0, ra = 0, rb = 0, rd = 0;
    int skip = 0;    // groups to jump over
    int delta = 0;   // added to a jalr offset (1 is cleared by jalr, 2 traps)
};

struct Gen {
    std::mt19937 rng;
    double hazard;
    std::vector<uint32_t> code;
    std::vector<Group> groups;
    std::deque<uint32_t> recent_rd;

    explicit Gen(uint32_t seed, double hz) : rng(seed), hazard(hz) {}

    uint32_t rnd(uint32_t n) { return std::uniform_int_distribution<uint32_t>(0, n - 1)(rng); }
    bool chance(double p) { return std::uniform_real_distribution<double>(0, 1)(rng) < p; }

    uint32_t pick_rd() {
        if (chance(0.04)) return 0;  // writes to x0 must be dropped
        uint32_t r = 1 + rnd(30);     // x1..x30
        recent_rd.push_back(r);
        if (recent_rd.size() > 3) recent_rd.pop_front();
        return r;
    }
    uint32_t pick_rs() {
        if (!recent_rd.empty() && chance(hazard)) return recent_rd[rnd(recent_rd.size())];
        if (chance(0.05)) return 0;
        return 1 + rnd(30);
    }
    int32_t imm12() {
        switch (rnd(4)) {
            case 0: return static_cast<int32_t>(rnd(4096)) - 2048;
            case 1: return static_cast<int32_t>(rnd(16)) - 8;
            case 2: return chance(0.5) ? 2047 : -2048;
            default: return static_cast<int32_t>(rnd(64));
        }
    }
    void emit(uint32_t w) { code.push_back(w); }
    void gemit(uint32_t w) { groups.back().w.push_back(w); }

    void load_imm(uint32_t rd, uint32_t v) {
        uint32_t hi = (v + 0x800) >> 12;
        emit(lui(rd, hi));
        emit(addi(rd, rd, static_cast<int32_t>(v << 20) >> 20));
    }

    // memory offset from BASE_REG; misaligned with small probability
    int32_t mem_off(uint32_t size, bool allow_misaligned) {
        // stays inside the 12-bit immediate and inside the data block
        int32_t off = static_cast<int32_t>(rnd(2040 / size)) * static_cast<int32_t>(size);
        if (allow_misaligned && size > 1 && chance(0.06)) off += 1 + static_cast<int32_t>(rnd(size - 1));
        return off;
    }

    void body_insn(size_t remaining) {
        groups.emplace_back();
        uint32_t c = rnd(1000);
        if (c < 250) {  // register-register ALU
            static uint32_t (*ops[])(uint32_t, uint32_t, uint32_t) = {add, sub, sll, slt, sltu, xor_, srl, sra, or_, and_};
            uint32_t a = pick_rs(), b = pick_rs();
            gemit(ops[rnd(10)](pick_rd(), a, b));
        } else if (c < 450) {  // immediate ALU
            uint32_t a = pick_rs();
            uint32_t rd = pick_rd();
            switch (rnd(9)) {
                case 0: gemit(addi(rd, a, imm12())); break;
                case 1: gemit(slti(rd, a, imm12())); break;
                case 2: gemit(sltiu(rd, a, imm12())); break;
                case 3: gemit(xori(rd, a, imm12())); break;
                case 4: gemit(ori(rd, a, imm12())); break;
                case 5: gemit(andi(rd, a, imm12())); break;
                case 6: gemit(slli(rd, a, rnd(32))); break;
                case 7: gemit(srli(rd, a, rnd(32))); break;
                default: gemit(srai(rd, a, rnd(32))); break;
            }
        } else if (c < 500) {  // upper immediates
            gemit(chance(0.5) ? lui(pick_rd(), rnd(1 << 20)) : auipc(pick_rd(), rnd(1 << 20)));
        } else if (c < 620) {  // loads, often followed by a use
            static uint32_t (*ops[])(uint32_t, uint32_t, int32_t) = {lb, lh, lw, lbu, lhu};
            static const uint32_t sz[] = {1, 2, 4, 1, 2};
            uint32_t k = rnd(5);
            uint32_t rd = pick_rd();
            gemit(ops[k](rd, BASE_REG, mem_off(sz[k], true)));
            if (rd != 0 && chance(0.4) && remaining > 1) gemit(add(pick_rd(), rd, pick_rs()));
        } else if (c < 720) {  // stores, often followed by a load of the same word
            static uint32_t (*ops[])(uint32_t, uint32_t, int32_t) = {sb, sh, sw};
            static const uint32_t sz[] = {1, 2, 4};
            uint32_t k = rnd(3);
            int32_t off = mem_off(sz[k], true);
            gemit(ops[k](pick_rs(), BASE_REG, off));
            if (chance(0.35) && remaining > 1) {
                static uint32_t (*lops[])(uint32_t, uint32_t, int32_t) = {lb, lh, lw, lbu, lhu};
                static const uint32_t lsz[] = {1, 2, 4, 1, 2};
                uint32_t j = rnd(5);
                int32_t loff = (off & ~3) + static_cast<int32_t>(rnd(4 / lsz[j]) * lsz[j]);
                gemit(lops[j](pick_rd(), BASE_REG, loff));
            }
        } else if (c < 830 && remaining > 10) {  // forward branch
            static uint32_t (*ops[])(uint32_t, uint32_t, int32_t) = {beq, bne, blt, bge, bltu, bgeu};
            static const uint32_t f3s[] = {0, 1, 4, 5, 6, 7};
            Group& g = groups.back();
            g.kind = Group::BRANCH;
            g.f3 = f3s[rnd(6)];
            g.ra = pick_rs();
            g.rb = pick_rs();
            g.skip = 1 + static_cast<int>(rnd(5));
            g.w.push_back(0);
            (void)ops;
        } else if (c < 860 && remaining > 10) {  // jal forward
            Group& g = groups.back();
            g.kind = Group::JAL;
            g.rd = pick_rd();
            g.skip = 1 + static_cast<int>(rnd(4));
            g.w.push_back(0);
        } else if (c < 890 && remaining > 10) {  // jalr forward through a register
            Group& g = groups.back();
            g.kind = Group::JALR;
            g.ra = 1 + rnd(30);
            g.rd = pick_rd();
            g.skip = 1 + static_cast<int>(rnd(4));
            g.delta = chance(0.08) ? 2 : chance(0.3) ? 1 : 0;
            g.w.push_back(auipc(g.ra, 0));
            g.w.push_back(0);
        } else if (c < 930) {  // CSRs
            uint32_t rd = pick_rd();
            switch (rnd(6)) {
                case 0: gemit(csrrw(rd, 0x340, pick_rs())); break;
                case 1: gemit(csrrs(rd, 0x340, pick_rs())); break;
                case 2: gemit(csrrc(rd, 0x340, pick_rs())); break;
                case 3: gemit(csrrwi(rd, 0x340, rnd(32))); break;
                case 4: gemit(csrrs(rd, chance(0.5) ? 0x300 : 0x301, 0)); break;
                default: gemit(csrrs(rd, chance(0.5) ? 0x342 : 0xF14, 0)); break;
            }
        } else if (c < 945) {  // exceptions
            switch (rnd(5)) {
                case 0: gemit(ECALL); break;
                case 1: gemit(EBREAK); break;
                case 2: gemit(0x00000000); break;                        // illegal
                case 3: gemit(csrrw(pick_rd(), 0xF14, pick_rs())); break;  // write read-only CSR
                default: gemit(csrrs(pick_rd(), 0x7C0, 0)); break;       // unknown CSR
            }
        } else if (c < 950) {
            gemit(chance(0.5) ? FENCE : FENCE_I);
        } else {
            gemit(add(pick_rd(), pick_rs(), pick_rs()));
        }
    }

    std::vector<uint8_t> build(size_t len) {
        code.clear();
        recent_rd.clear();
        // prologue: mtvec, base register, random registers
        size_t mtvec_fixup = code.size();
        emit(0); emit(0);                 // placeholder: load handler address into x5
        emit(csrrw(0, 0x305, 5));
        load_imm(BASE_REG, DATA_BASE);
        for (uint32_t r = 1; r < 31; ++r) load_imm(r, rng());

        size_t body_start = code.size();
        groups.clear();
        size_t n = 0;
        while (n < len) {
            body_insn(len - n);
            n += groups.back().w.size();
        }
        // lay out the groups, then patch every branch and jump to land on a
        // later group start (or the epilogue)
        std::vector<size_t> start(groups.size() + 1);
        size_t pos = body_start;
        for (size_t gi = 0; gi < groups.size(); ++gi) {
            start[gi] = pos;
            pos += groups[gi].w.size();
        }
        start[groups.size()] = pos;  // epilogue
        for (size_t gi = 0; gi < groups.size(); ++gi) {
            Group& g = groups[gi];
            size_t tgt_group = std::min(groups.size(), gi + 1 + static_cast<size_t>(g.skip));
            int32_t tgt = static_cast<int32_t>(start[tgt_group] * 4);
            if (g.kind == Group::BRANCH) {
                int32_t off = tgt - static_cast<int32_t>(start[gi] * 4);
                g.w[0] = b(off, g.rb, g.ra, g.f3);
            } else if (g.kind == Group::JAL) {
                g.w[0] = jal(g.rd, tgt - static_cast<int32_t>(start[gi] * 4));
            } else if (g.kind == Group::JALR) {
                // auipc gives the auipc's own pc
                g.w[1] = jalr(g.rd, g.ra, tgt - static_cast<int32_t>(start[gi] * 4) + g.delta);
            }
            for (uint32_t w : g.w) code.push_back(w);
        }

        // epilogue: pass
        emit(addi(5, 0, 1));
        emit(lui(6, 0x20));
        emit(sw(5, 6, 0));
        emit(jal(0, 0));

        // trap handler: skip the faulting instruction
        uint32_t handler = static_cast<uint32_t>(code.size() * 4);
        emit(csrrs(TRAP_T0, 0x342, 0));
        emit(csrrs(TRAP_T1, 0x341, 0));
        emit(addi(TRAP_T1, TRAP_T1, 4));
        emit(csrrw(0, 0x341, TRAP_T1));
        emit(MRET);

        uint32_t hi = (handler + 0x800) >> 12;
        code[mtvec_fixup] = lui(5, hi);
        code[mtvec_fixup + 1] = addi(5, 5, static_cast<int32_t>(handler << 20) >> 20);

        std::vector<uint8_t> img(DATA_BASE + DATA_SIZE, 0);
        for (size_t i = 0; i < code.size(); ++i)
            for (int b = 0; b < 4; ++b) img[i * 4 + b] = static_cast<uint8_t>(code[i] >> (8 * b));
        for (uint32_t a = DATA_BASE; a < DATA_BASE + DATA_SIZE; ++a) img[a] = static_cast<uint8_t>(rng());
        return img;
    }
};

}  // namespace

int main(int argc, char** argv) {
    std::string out = ".";
    int count = 1;
    size_t len = 2000;
    uint32_t seed = 1;
    double hazard = 0.5;
    for (int i = 1; i + 1 < argc; i += 2) {
        std::string a = argv[i];
        if (a == "--out") out = argv[i + 1];
        else if (a == "--count") count = std::atoi(argv[i + 1]);
        else if (a == "--len") len = std::strtoul(argv[i + 1], nullptr, 10);
        else if (a == "--seed") seed = std::strtoul(argv[i + 1], nullptr, 10);
        else if (a == "--hazard") hazard = std::atof(argv[i + 1]);
    }
    if (len * 4 + 0x400 > DATA_BASE) {
        std::fprintf(stderr, "--len too large: code would overlap the data block\n");
        return 2;
    }
    for (int k = 0; k < count; ++k) {
        Gen g(seed * 1000003u + static_cast<uint32_t>(k), hazard);
        auto img = g.build(len);
        char name[64];
        std::snprintf(name, sizeof name, "/rand_s%u_%05d.bin", seed, k);
        std::ofstream f(out + name, std::ios::binary);
        f.write(reinterpret_cast<const char*>(img.data()), static_cast<std::streamsize>(img.size()));
    }
    return 0;
}
