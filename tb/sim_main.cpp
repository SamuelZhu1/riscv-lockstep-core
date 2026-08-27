// Verilator harness: runs programs on the RTL core and, in lockstep, on the
// C++ ISS. Every instruction the core retires is compared field by field
// with the ISS; the first mismatch stops the run and prints the last 32
// retirements from both sides.
//
// usage: Vtb_top [options] prog1.bin [prog2.bin ...]
//   --no-lockstep        run the RTL alone
//   --max-cycles N       per program (default 2,000,000)
//   --cov FILE           write coverage counts as JSON
//   --summary FILE       write totals as JSON
//   --quiet              only print failures and the summary
//   --trace FILE.vcd     dump waves for the first program (needs a --trace build)

#include <verilated.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "../iss/iss.h"
#include "Vtb_top.h"
#if VM_TRACE
#include <verilated_vcd_c.h>
#endif

// ---------------------------------------------------------------------
// memories reached from the RTL through DPI
// ---------------------------------------------------------------------
static std::vector<uint32_t> g_ram(16384, 0);
static std::array<std::array<uint8_t, 256>, 2> g_eeprom;

extern "C" uint32_t ram_read(uint32_t word_addr) { return g_ram[word_addr & 16383]; }
extern "C" void ram_write(uint32_t word_addr, uint32_t data, uint32_t mask) {
    uint32_t& w = g_ram[word_addr & 16383];
    for (int b = 0; b < 4; ++b)
        if (mask & (1u << b)) w = (w & ~(0xFFu << (8 * b))) | (data & (0xFFu << (8 * b)));
}
extern "C" uint32_t eeprom_read(uint32_t dev, uint32_t off) { return g_eeprom[dev & 1][off & 255]; }

// SFF-8472 contents for a made-up 10GBASE-LR module. The driver test in
// sw/i2c_driver checks the values it decodes against these.
static void build_eeprom() {
    auto& a0 = g_eeprom[0];
    auto& a2 = g_eeprom[1];
    a0.fill(0);
    a2.fill(0);
    a0[0] = 0x03;  // SFP
    a0[1] = 0x04;
    a0[2] = 0x07;  // LC
    a0[3] = 0x20;  // 10GBASE-LR
    a0[11] = 0x06;
    a0[12] = 0x67;  // 10.3 Gb/s
    auto put = [&](int off, const char* s, int len) {
        for (int i = 0; i < len; ++i) a0[off + i] = (i < (int)std::strlen(s)) ? s[i] : ' ';
    };
    put(20, "LAB OPTICS", 16);
    put(40, "SFP-10G-LR", 16);
    put(68, "LO2611A0042", 16);
    a0[60] = 0x05;  // 1310 nm
    a0[61] = 0x1E;
    a0[92] = 0x68;  // DDM implemented, internally calibrated
    uint8_t cc = 0;
    for (int i = 0; i < 63; ++i) cc += a0[i];
    a0[63] = cc;
    uint8_t ce = 0;
    for (int i = 64; i < 95; ++i) ce += a0[i];
    a0[95] = ce;
    // live diagnostics, big endian
    auto put16 = [&](int off, uint16_t v) { a2[off] = v >> 8; a2[off + 1] = v & 0xFF; };
    put16(96, 0x2380);   // 35.5 C
    put16(98, 33000);    // 3.300 V in 100 uV
    put16(100, 3000);    // 6.000 mA in 2 uA
    put16(102, 5000);    // 0.5000 mW in 0.1 uW  -> -3.01 dBm
    put16(104, 2000);    // 0.2000 mW            -> -6.99 dBm
}

// ---------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------
static std::vector<uint8_t> read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

static const char* insn_class(uint32_t i) {
    uint32_t op = i & 0x7F, f3 = (i >> 12) & 7, f7 = i >> 25;
    switch (op) {
        case 0x37: return "lui";
        case 0x17: return "auipc";
        case 0x6F: return "jal";
        case 0x67: return "jalr";
        case 0x63: { static const char* n[8] = {"beq", "bne", "?", "?", "blt", "bge", "bltu", "bgeu"}; return n[f3]; }
        case 0x03: { static const char* n[8] = {"lb", "lh", "lw", "?", "lbu", "lhu", "?", "?"}; return n[f3]; }
        case 0x23: { static const char* n[8] = {"sb", "sh", "sw", "?", "?", "?", "?", "?"}; return n[f3]; }
        case 0x13: {
            static const char* n[8] = {"addi", "slli", "slti", "sltiu", "xori", "srli", "ori", "andi"};
            if (f3 == 5 && f7 == 0x20) return "srai";
            return n[f3];
        }
        case 0x33: {
            static const char* n[8] = {"add", "sll", "slt", "sltu", "xor", "srl", "or", "and"};
            if (f7 == 0x20) return f3 == 0 ? "sub" : "sra";
            return n[f3];
        }
        case 0x0F: return f3 == 1 ? "fence.i" : "fence";
        case 0x73: {
            if (i == 0x00000073) return "ecall";
            if (i == 0x00100073) return "ebreak";
            if (i == 0x30200073) return "mret";
            static const char* n[8] = {"?", "csrrw", "csrrs", "csrrc", "?", "csrrwi", "csrrsi", "csrrci"};
            return n[f3];
        }
    }
    return "?";
}

static const char* COV_NAMES[16] = {"fwd_exmem_rs1", "fwd_exmem_rs2", "fwd_memwb_rs1", "fwd_memwb_rs2",
                                    "load_use_stall", "branch_taken_flush", "branch_not_taken", "jal_flush",
                                    "jalr_flush", "store_load_bypass", "exception", "mret",
                                    "wb_id_bypass", "write_x0", "stall_and_flush", "fence_i"};

struct Totals {
    uint64_t programs = 0, passed = 0, retired = 0, cycles = 0, mismatches = 0, timeouts = 0;
    uint64_t nack_faults = 0, stuck_faults = 0;
    std::array<uint64_t, 16> cov{};
    std::map<std::string, uint64_t> insns;
    std::map<uint32_t, uint64_t> causes;
};

static iss::Retire from_rtl(const Vtb_top& t) {
    iss::Retire r;
    r.order = t.rvfi_order;
    r.insn = t.rvfi_insn;
    r.trap = t.rvfi_trap;
    r.pc_rdata = t.rvfi_pc_rdata;
    r.pc_wdata = t.rvfi_pc_wdata;
    r.rs1_addr = t.rvfi_rs1_addr;
    r.rs2_addr = t.rvfi_rs2_addr;
    r.rs1_rdata = t.rvfi_rs1_rdata;
    r.rs2_rdata = t.rvfi_rs2_rdata;
    r.rd_addr = t.rvfi_rd_addr;
    r.rd_wdata = t.rvfi_rd_wdata;
    r.mem_addr = t.rvfi_mem_addr;
    r.mem_rmask = t.rvfi_mem_rmask;
    r.mem_wmask = t.rvfi_mem_wmask;
    r.mem_rdata = t.rvfi_mem_rdata;
    r.mem_wdata = t.rvfi_mem_wdata;
    return r;
}

static uint32_t lanes(uint32_t v, uint8_t mask) {
    uint32_t m = 0;
    for (int b = 0; b < 4; ++b)
        if (mask & (1 << b)) m |= 0xFFu << (8 * b);
    return v & m;
}

// returns an empty string when the records agree
static std::string compare(const iss::Retire& rtl, const iss::Retire& ref) {
    char buf[160];
#define FIELD(f)                                                                                           \
    if (rtl.f != ref.f) {                                                                                  \
        snprintf(buf, sizeof buf, #f ": rtl=0x%llx iss=0x%llx", (unsigned long long)rtl.f,                 \
                 (unsigned long long)ref.f);                                                               \
        return buf;                                                                                        \
    }
    FIELD(pc_rdata);
    FIELD(insn);
    FIELD(trap);
    FIELD(pc_wdata);
    if (ref.trap) return "";
    FIELD(rs1_addr);
    FIELD(rs2_addr);
    FIELD(rs1_rdata);
    FIELD(rs2_rdata);
    FIELD(rd_addr);
    FIELD(rd_wdata);
    FIELD(mem_rmask);
    FIELD(mem_wmask);
    if (ref.mem_rmask || ref.mem_wmask) FIELD(mem_addr);
    if (lanes(rtl.mem_rdata, ref.mem_rmask) != lanes(ref.mem_rdata, ref.mem_rmask)) return "mem_rdata";
    if (lanes(rtl.mem_wdata, ref.mem_wmask) != lanes(ref.mem_wdata, ref.mem_wmask)) return "mem_wdata";
#undef FIELD
    return "";
}

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    bool lockstep = true, quiet = false;
    uint64_t max_cycles = 2000000;
    std::string cov_file, summary_file, trace_file;
    std::vector<std::string> progs;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--no-lockstep") lockstep = false;
        else if (a == "--quiet") quiet = true;
        else if (a == "--max-cycles" && i + 1 < argc) max_cycles = std::stoull(argv[++i]);
        else if (a == "--cov" && i + 1 < argc) cov_file = argv[++i];
        else if (a == "--summary" && i + 1 < argc) summary_file = argv[++i];
        else if (a == "--trace" && i + 1 < argc) trace_file = argv[++i];
        else if (a == "--list" && i + 1 < argc) {
            std::ifstream f(argv[++i]);
            std::string line;
            while (std::getline(f, line))
                if (!line.empty()) progs.push_back(line);
        } else if (a.rfind("+", 0) == 0) continue;
        else progs.push_back(a);
    }
    if (progs.empty()) {
        std::fprintf(stderr, "no programs given\n");
        return 2;
    }

    build_eeprom();
    auto ctx = std::make_unique<VerilatedContext>();
    auto top = std::make_unique<Vtb_top>(ctx.get());
#if VM_TRACE
    std::unique_ptr<VerilatedVcdC> tfp;
    if (!trace_file.empty()) {
        ctx->traceEverOn(true);
        tfp = std::make_unique<VerilatedVcdC>();
        top->trace(tfp.get(), 99);
        tfp->open(trace_file.c_str());
    }
#endif
    uint64_t sim_time = 0;
    auto tick = [&]() {
        top->clk = 0;
        top->eval();
#if VM_TRACE
        if (tfp) tfp->dump(sim_time);
#endif
        ++sim_time;
        top->clk = 1;
        top->eval();
#if VM_TRACE
        if (tfp) tfp->dump(sim_time);
#endif
        ++sim_time;
    };

    Totals tot;
    iss::Iss ref;
    const iss::Retire* cur_rtl = nullptr;
    ref.mmio_read = [&](uint32_t) -> uint32_t { return cur_rtl ? cur_rtl->mem_rdata : 0; };
    auto t0 = std::chrono::steady_clock::now();

    for (size_t pi = 0; pi < progs.size(); ++pi) {
        const std::string& path = progs[pi];
        auto img = read_file(path);
        if (img.empty()) {
            std::printf("FAIL %s: cannot read program\n", path.c_str());
            ++tot.programs;
            continue;
        }
        std::fill(g_ram.begin(), g_ram.end(), 0);
        for (size_t i = 0; i < img.size() && i < 65536; ++i)
            g_ram[i / 4] |= static_cast<uint32_t>(img[i]) << (8 * (i % 4));
        ref.load(img);
        ref.reset(0);

        top->rst_n = 0;
        for (int i = 0; i < 4; ++i) tick();
        top->rst_n = 1;

        std::deque<std::string> hist_rtl, hist_iss;
        std::string console;
        bool done = false, pass = false, mismatch = false;
        uint32_t status = 0;
        uint64_t cycles = 0, retired = 0;
        while (!done && cycles < max_cycles) {
            tick();
            ++cycles;
            for (int b = 0; b < 16; ++b)
                if (top->cov & (1u << b)) ++tot.cov[b];
            if (top->fault_nack_used) ++tot.nack_faults;
            if (top->rvfi_valid) {
                iss::Retire r = from_rtl(*top);
                ++retired;
                ++tot.insns[insn_class(r.insn)];
                if (lockstep) {
                    cur_rtl = &r;
                    iss::Retire e = ref.step();
                    cur_rtl = nullptr;
                    if (r.trap) ++tot.causes[ref.mcause];
                    hist_rtl.push_back(iss::describe(r));
                    hist_iss.push_back(iss::describe(e));
                    if (hist_rtl.size() > 32) { hist_rtl.pop_front(); hist_iss.pop_front(); }
                    std::string diff = compare(r, e);
                    if (!diff.empty()) {
                        std::printf("FAIL %s: lockstep mismatch at retirement %llu: %s\n", path.c_str(),
                                    (unsigned long long)r.order, diff.c_str());
                        std::printf("  last %zu retirements, RTL:\n", hist_rtl.size());
                        for (auto& s : hist_rtl) std::printf("    %s\n", s.c_str());
                        std::printf("  ISS:\n");
                        for (auto& s : hist_iss) std::printf("    %s\n", s.c_str());
                        mismatch = true;
                        done = true;
                    }
                }
            }
            if (top->console_valid) {
                char c = static_cast<char>(top->console_char);
                if (c == '\n') {
                    if (!quiet) std::printf("  [%s] %s\n", path.c_str(), console.c_str());
                    console.clear();
                } else console += c;
            }
            if (top->status_valid && !done) {
                status = top->status_data;
                pass = (status == 1);
                done = true;
                // drain the pipeline so the status store itself is compared
                for (int k = 0; k < 4 && lockstep && !mismatch; ++k) {
                    tick();
                    ++cycles;
                    if (top->rvfi_valid) {
                        iss::Retire r = from_rtl(*top);
                        ++retired;
                        cur_rtl = &r;
                        iss::Retire e = ref.step();
                        cur_rtl = nullptr;
                        std::string diff = compare(r, e);
                        if (!diff.empty()) {
                            std::printf("FAIL %s: lockstep mismatch at retirement %llu: %s\n", path.c_str(),
                                        (unsigned long long)r.order, diff.c_str());
                            mismatch = true;
                        }
                    }
                }
            }
        }
#if VM_TRACE
        if (tfp) { tfp->close(); tfp.reset(); }
#endif
        ++tot.programs;
        tot.retired += retired;
        tot.cycles += cycles;
        if (mismatch) {
            ++tot.mismatches;
        } else if (!done) {
            ++tot.timeouts;
            std::printf("FAIL %s: timeout after %llu cycles\n", path.c_str(), (unsigned long long)cycles);
        } else if (!pass) {
            std::printf("FAIL %s: program reported failure, status=0x%x (test %u)\n", path.c_str(), status,
                        status >> 1);
        } else {
            ++tot.passed;
            if (!quiet)
                std::printf("PASS %s  (%llu instructions, %llu cycles, CPI %.3f)\n", path.c_str(),
                            (unsigned long long)retired, (unsigned long long)cycles,
                            retired ? double(cycles) / retired : 0.0);
        }
    }

    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("SUMMARY programs=%llu passed=%llu mismatches=%llu timeouts=%llu retired=%llu cycles=%llu "
                "lockstep=%s %.1fs\n",
                (unsigned long long)tot.programs, (unsigned long long)tot.passed,
                (unsigned long long)tot.mismatches, (unsigned long long)tot.timeouts,
                (unsigned long long)tot.retired, (unsigned long long)tot.cycles, lockstep ? "on" : "off", secs);

    if (!cov_file.empty()) {
        std::ofstream f(cov_file);
        f << "{\n  \"events\": {";
        for (int b = 0; b < 16; ++b) f << (b ? ", " : "") << "\"" << COV_NAMES[b] << "\": " << tot.cov[b];
        f << "},\n  \"instructions\": {";
        bool first = true;
        for (auto& [k, v] : tot.insns) { f << (first ? "" : ", ") << "\"" << k << "\": " << v; first = false; }
        f << "},\n  \"trap_causes\": {";
        first = true;
        for (auto& [k, v] : tot.causes) { f << (first ? "" : ", ") << "\"" << k << "\": " << v; first = false; }
        f << "},\n  \"i2c_nack_faults\": " << tot.nack_faults << "\n}\n";
    }
    if (!summary_file.empty()) {
        std::ofstream f(summary_file);
        f << "{\"programs\": " << tot.programs << ", \"passed\": " << tot.passed << ", \"mismatches\": "
          << tot.mismatches << ", \"timeouts\": " << tot.timeouts << ", \"retired\": " << tot.retired
          << ", \"cycles\": " << tot.cycles << "}\n";
    }
    top->final();
    return tot.passed == tot.programs ? 0 : 1;
}
