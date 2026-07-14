// Tiny RV32I encoder shared by the random program generator and the ISS
// unit tests, so neither needs a cross toolchain.
#pragma once
#include <cstdint>

namespace enc {

inline uint32_t r(uint32_t f7, uint32_t rs2, uint32_t rs1, uint32_t f3, uint32_t rd, uint32_t op) {
    return (f7 << 25) | (rs2 << 20) | (rs1 << 15) | (f3 << 12) | (rd << 7) | op;
}
inline uint32_t i(int32_t imm, uint32_t rs1, uint32_t f3, uint32_t rd, uint32_t op) {
    return (static_cast<uint32_t>(imm & 0xFFF) << 20) | (rs1 << 15) | (f3 << 12) | (rd << 7) | op;
}
inline uint32_t s(int32_t imm, uint32_t rs2, uint32_t rs1, uint32_t f3, uint32_t op) {
    uint32_t u = static_cast<uint32_t>(imm) & 0xFFF;
    return ((u >> 5) << 25) | (rs2 << 20) | (rs1 << 15) | (f3 << 12) | ((u & 0x1F) << 7) | op;
}
inline uint32_t b(int32_t imm, uint32_t rs2, uint32_t rs1, uint32_t f3) {
    uint32_t u = static_cast<uint32_t>(imm) & 0x1FFF;
    return (((u >> 12) & 1) << 31) | (((u >> 5) & 0x3F) << 25) | (rs2 << 20) | (rs1 << 15) | (f3 << 12) |
           (((u >> 1) & 0xF) << 8) | (((u >> 11) & 1) << 7) | 0x63;
}
inline uint32_t u(uint32_t imm20, uint32_t rd, uint32_t op) { return (imm20 << 12) | (rd << 7) | op; }
inline uint32_t j(int32_t imm, uint32_t rd) {
    uint32_t v = static_cast<uint32_t>(imm) & 0x1FFFFF;
    return (((v >> 20) & 1) << 31) | (((v >> 1) & 0x3FF) << 21) | (((v >> 11) & 1) << 20) | (((v >> 12) & 0xFF) << 12) |
           (rd << 7) | 0x6F;
}

// mnemonics
inline uint32_t addi(uint32_t rd, uint32_t rs1, int32_t imm) { return i(imm, rs1, 0, rd, 0x13); }
inline uint32_t slti(uint32_t rd, uint32_t rs1, int32_t imm) { return i(imm, rs1, 2, rd, 0x13); }
inline uint32_t sltiu(uint32_t rd, uint32_t rs1, int32_t imm) { return i(imm, rs1, 3, rd, 0x13); }
inline uint32_t xori(uint32_t rd, uint32_t rs1, int32_t imm) { return i(imm, rs1, 4, rd, 0x13); }
inline uint32_t ori(uint32_t rd, uint32_t rs1, int32_t imm) { return i(imm, rs1, 6, rd, 0x13); }
inline uint32_t andi(uint32_t rd, uint32_t rs1, int32_t imm) { return i(imm, rs1, 7, rd, 0x13); }
inline uint32_t slli(uint32_t rd, uint32_t rs1, uint32_t sh) { return i(sh & 31, rs1, 1, rd, 0x13); }
inline uint32_t srli(uint32_t rd, uint32_t rs1, uint32_t sh) { return i(sh & 31, rs1, 5, rd, 0x13); }
inline uint32_t srai(uint32_t rd, uint32_t rs1, uint32_t sh) { return i(0x400 | (sh & 31), rs1, 5, rd, 0x13); }
inline uint32_t add(uint32_t rd, uint32_t a, uint32_t b_) { return r(0, b_, a, 0, rd, 0x33); }
inline uint32_t sub(uint32_t rd, uint32_t a, uint32_t b_) { return r(0x20, b_, a, 0, rd, 0x33); }
inline uint32_t sll(uint32_t rd, uint32_t a, uint32_t b_) { return r(0, b_, a, 1, rd, 0x33); }
inline uint32_t slt(uint32_t rd, uint32_t a, uint32_t b_) { return r(0, b_, a, 2, rd, 0x33); }
inline uint32_t sltu(uint32_t rd, uint32_t a, uint32_t b_) { return r(0, b_, a, 3, rd, 0x33); }
inline uint32_t xor_(uint32_t rd, uint32_t a, uint32_t b_) { return r(0, b_, a, 4, rd, 0x33); }
inline uint32_t srl(uint32_t rd, uint32_t a, uint32_t b_) { return r(0, b_, a, 5, rd, 0x33); }
inline uint32_t sra(uint32_t rd, uint32_t a, uint32_t b_) { return r(0x20, b_, a, 5, rd, 0x33); }
inline uint32_t or_(uint32_t rd, uint32_t a, uint32_t b_) { return r(0, b_, a, 6, rd, 0x33); }
inline uint32_t and_(uint32_t rd, uint32_t a, uint32_t b_) { return r(0, b_, a, 7, rd, 0x33); }
inline uint32_t lui(uint32_t rd, uint32_t imm20) { return u(imm20 & 0xFFFFF, rd, 0x37); }
inline uint32_t auipc(uint32_t rd, uint32_t imm20) { return u(imm20 & 0xFFFFF, rd, 0x17); }
inline uint32_t jal(uint32_t rd, int32_t off) { return j(off, rd); }
inline uint32_t jalr(uint32_t rd, uint32_t rs1, int32_t off) { return i(off, rs1, 0, rd, 0x67); }
inline uint32_t beq(uint32_t a, uint32_t b_, int32_t off) { return b(off, b_, a, 0); }
inline uint32_t bne(uint32_t a, uint32_t b_, int32_t off) { return b(off, b_, a, 1); }
inline uint32_t blt(uint32_t a, uint32_t b_, int32_t off) { return b(off, b_, a, 4); }
inline uint32_t bge(uint32_t a, uint32_t b_, int32_t off) { return b(off, b_, a, 5); }
inline uint32_t bltu(uint32_t a, uint32_t b_, int32_t off) { return b(off, b_, a, 6); }
inline uint32_t bgeu(uint32_t a, uint32_t b_, int32_t off) { return b(off, b_, a, 7); }
inline uint32_t lb(uint32_t rd, uint32_t rs1, int32_t off) { return i(off, rs1, 0, rd, 0x03); }
inline uint32_t lh(uint32_t rd, uint32_t rs1, int32_t off) { return i(off, rs1, 1, rd, 0x03); }
inline uint32_t lw(uint32_t rd, uint32_t rs1, int32_t off) { return i(off, rs1, 2, rd, 0x03); }
inline uint32_t lbu(uint32_t rd, uint32_t rs1, int32_t off) { return i(off, rs1, 4, rd, 0x03); }
inline uint32_t lhu(uint32_t rd, uint32_t rs1, int32_t off) { return i(off, rs1, 5, rd, 0x03); }
inline uint32_t sb(uint32_t rs2, uint32_t rs1, int32_t off) { return s(off, rs2, rs1, 0, 0x23); }
inline uint32_t sh(uint32_t rs2, uint32_t rs1, int32_t off) { return s(off, rs2, rs1, 1, 0x23); }
inline uint32_t sw(uint32_t rs2, uint32_t rs1, int32_t off) { return s(off, rs2, rs1, 2, 0x23); }
inline uint32_t csrrw(uint32_t rd, uint32_t csr, uint32_t rs1) { return i(static_cast<int32_t>(csr), rs1, 1, rd, 0x73); }
inline uint32_t csrrs(uint32_t rd, uint32_t csr, uint32_t rs1) { return i(static_cast<int32_t>(csr), rs1, 2, rd, 0x73); }
inline uint32_t csrrc(uint32_t rd, uint32_t csr, uint32_t rs1) { return i(static_cast<int32_t>(csr), rs1, 3, rd, 0x73); }
inline uint32_t csrrwi(uint32_t rd, uint32_t csr, uint32_t z) { return i(static_cast<int32_t>(csr), z, 5, rd, 0x73); }
inline uint32_t csrrsi(uint32_t rd, uint32_t csr, uint32_t z) { return i(static_cast<int32_t>(csr), z, 6, rd, 0x73); }
inline uint32_t csrrci(uint32_t rd, uint32_t csr, uint32_t z) { return i(static_cast<int32_t>(csr), z, 7, rd, 0x73); }
constexpr uint32_t ECALL = 0x00000073, EBREAK = 0x00100073, MRET = 0x30200073, FENCE = 0x0FF0000F,
                   FENCE_I = 0x0000100F, NOP = 0x00000013;

}  // namespace enc
