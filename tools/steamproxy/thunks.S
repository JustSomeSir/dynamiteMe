# Forwarding thunks for wrapped Steam interfaces (x64, Windows calling convention).
#
# A wrapper object is { void **vtbl; void *real; }. Thunk N replaces `this` (rcx) with
# wrapper->real and jumps to real->vtbl[N]. Nothing else is touched, so register, stack, float
# and hidden-return-pointer arguments all reach the real method exactly as the caller passed them.

    .intel_syntax noprefix
    .text

    .macro THUNK idx
    .globl sp_fwd_\idx
    .p2align 4
sp_fwd_\idx:
    mov rcx, qword ptr [rcx + 8]
    mov rax, qword ptr [rcx]
    jmp qword ptr [rax + \idx * 8]
    .endm

    .irp i, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
    THUNK \i
    .endr

    .section .rdata,"dr"
    .globl sp_fwd_table
    # 32-byte aligned: C++ sees `void *const[32]` and the x64 ABI lets the compiler assume 16-byte
    # alignment for arrays this big, so it copies the table with movaps (crashed at 8-byte alignment).
    .p2align 5
sp_fwd_table:
    .irp i, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
    .quad sp_fwd_\i
    .endr
