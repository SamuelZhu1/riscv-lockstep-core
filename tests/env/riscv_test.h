// Minimal test environment for riscv-tests on this core: no virtual memory,
// no host interface. Pass and fail are reported by a store to TEST_STATUS
// (0x0002_0000), which the testbench and the FPGA top both watch.
#ifndef _ENV_LOCKSTEP_H
#define _ENV_LOCKSTEP_H

#define RVTEST_RV32U .macro init; .endm
#define RVTEST_RV64U RVTEST_RV32U

#define TESTNUM gp

#define TEST_STATUS_HI 0x20

#define RVTEST_CODE_BEGIN                                               \
        .section .text.init;                                            \
        .align  6;                                                      \
        .globl _start;                                                  \
_start:                                                                 \
        la      t0, trap_vector;                                        \
        csrw    mtvec, t0;                                              \
        li      TESTNUM, 0;                                             \
        j       reset_vector;                                           \
        .align  2;                                                      \
trap_vector:                                                            \
        /* no rv32ui test expects a trap: report test 0x7ff */          \
        li      t0, (0x7ff << 1) | 1;                                   \
        lui     t1, TEST_STATUS_HI;                                     \
        sw      t0, 0(t1);                                              \
1:      j       1b;                                                     \
reset_vector:                                                           \
        init;

#define RVTEST_CODE_END                                                 \
        unimp

#define RVTEST_PASS                                                     \
        fence;                                                          \
        li      t0, 1;                                                  \
        lui     t1, TEST_STATUS_HI;                                     \
        sw      t0, 0(t1);                                              \
1:      j       1b;

#define RVTEST_FAIL                                                     \
        fence;                                                          \
        slli    t0, TESTNUM, 1;                                         \
        ori     t0, t0, 1;                                              \
        lui     t1, TEST_STATUS_HI;                                     \
        sw      t0, 0(t1);                                              \
1:      j       1b;

#define EXTRA_DATA

#define RVTEST_DATA_BEGIN                                               \
        EXTRA_DATA                                                      \
        .align 4;

#define RVTEST_DATA_END

#endif
