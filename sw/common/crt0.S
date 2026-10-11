# Startup for bare-metal programs on the core: stack, .bss, trap vector,
# then main(). main's return value is reported through TEST_STATUS:
# 0 -> 1 (pass), n -> (n << 1) | 1 (fail with code n).
    .section .text.init
    .globl _start
_start:
    la      sp, __stack_top
    la      t0, trap_entry
    csrw    mtvec, t0
    la      t0, __bss_start
    la      t1, __bss_end
1:  bgeu    t0, t1, 2f
    sw      zero, 0(t0)
    addi    t0, t0, 4
    j       1b
2:  call    main
    li      t1, 0x20000
    beqz    a0, 3f
    slli    a0, a0, 1
    ori     a0, a0, 1
    sw      a0, 0(t1)
    j       4f
3:  li      t0, 1
    sw      t0, 0(t1)
4:  j       4b

# Unexpected trap: report mcause as a failure code (0x100 + cause).
    .align 2
trap_entry:
    csrr    a0, mcause
    addi    a0, a0, 0x100
    slli    a0, a0, 1
    ori     a0, a0, 1
    li      t1, 0x20000
    sw      a0, 0(t1)
5:  j       5b
