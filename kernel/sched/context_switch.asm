;;
;; Lestra OS - Real Context Switch v2 (x86_64)
;; Saves/restores FS_BASE MSR per-thread + wraps in cli/sti
;; Only saves callee-saved regs (caller-saved are in the IRQ frame)
;;

bits 64
default rel

global context_switch

%define OFF_R15   0x00
%define OFF_R14   0x08
%define OFF_R13   0x10
%define OFF_R12   0x18
%define OFF_RBP   0x20
%define OFF_RBX   0x28
%define OFF_RAX   0x30
%define OFF_RIP   0x38
%define OFF_CS    0x40
%define OFF_RFLAGS 0x48
%define OFF_RSP   0x50
%define OFF_SS    0x58
%define OFF_FSBASE 0x60

section .text

context_switch:
    cli

    test    rdi, rdi
    jz      .no_save

    mov     [rdi + OFF_R15], r15
    mov     [rdi + OFF_R14], r14
    mov     [rdi + OFF_R13], r13
    mov     [rdi + OFF_R12], r12
    mov     [rdi + OFF_RBP], rbp
    mov     [rdi + OFF_RBX], rbx
    mov     [rdi + OFF_RAX], rax

    mov     ecx, 0xC0000100
    rdmsr
    mov     [rdi + OFF_FSBASE], eax
    mov     [rdi + OFF_FSBASE + 4], edx

.no_save:
    mov     r12, rsi

    mov     rax, cr3
    cmp     rax, rdx
    je      .skip_cr3
    mov     cr3, rdx
.skip_cr3:

    mov     rsp, rcx

    mov     ecx, 0xC0000100
    mov     eax, [r12 + OFF_FSBASE]
    mov     edx, [r12 + OFF_FSBASE + 4]
    wrmsr

    mov     rax, [r12 + OFF_SS]
    push    rax
    mov     rax, [r12 + OFF_RSP]
    push    rax
    mov     rax, [r12 + OFF_RFLAGS]
    push    rax
    mov     rax, [r12 + OFF_CS]
    push    rax
    mov     rax, [r12 + OFF_RIP]
    push    rax

    mov     r15, [r12 + OFF_R15]
    mov     r14, [r12 + OFF_R14]
    mov     r13, [r12 + OFF_R13]
    mov     rbp, [r12 + OFF_RBP]
    mov     rbx, [r12 + OFF_RBX]
    mov     rax, [r12 + OFF_RAX]
    mov     r12, [r12 + OFF_R12]

    iretq
