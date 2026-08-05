;;
;; Lestra OS - Jump to userspace (ring 3)
;; Copyright (c) 2026 lestramk.org / Lee Muriihi Kingori
;;
;; This switches CR3 to the user's PML4, sets up an IRETQ frame with
;; RPL=3 (user mode), and jumps to the user program's entry point.
;;
;; Calling convention (System V AMD64):
;;   rdi = entry point (RIP for user)
;;   rsi = stack pointer (RSP for user)
;;   rdx = PML4 physical address
;;
;; We never return from this function. The only way back to the kernel
;; is via a syscall (SYS_EXIT etc.).

bits 64

section .text

global elf_jump_to_user

elf_jump_to_user:
    ; Save arguments
    mov r15, rdi            ; r15 = user entry point
    mov r14, rsi            ; r14 = user stack pointer
    mov r13, rdx            ; r13 = user PML4 physical address

    ; Switch CR3 to the user's address space
    ; IMPORTANT: The user PML4 includes the kernel's identity-mapped
    ; entries (PML4[0..3]) so kernel code can still run when handling
    ; syscalls. Without this, a page fault would triple-fault the CPU.
    mov rax, cr3
    mov [save_kernel_cr3], rax    ; save kernel CR3 for syscall return
    mov rax, r13
    mov cr3, rax

    ; Set up the IRETQ frame on the user stack.
    ; IRETQ expects: [RSP] = RIP, CS, RFLAGS, RSP, SS (pushed in reverse)
    ; We push these onto the USER stack, then IRETQ pops them.
    ;
    ; We need to set RFLAGS with interrupts enabled (IF=1) so the user
    ; process can be interrupted by the timer IRQ (for preemption later).
    ;
    ; CS = 0x1B (USER_CS | RPL3 = 0x18 | 3)
    ; SS = 0x23 (USER_DS | RPL3 = 0x20 | 3)

    ; KE-25: The pushes below are SUPERVISOR writes to the USER stack (a
    ; user page). Under SMAP (CR4.SMAP=1) these would #PF with AC clear.
    ; stac() sets AC so the supervisor may write the iret frame to the
    ; user stack; clac() clears it before iretq (after iretq we are in
    ; ring 3 where SMAP does not apply).
    extern g_smap_enabled
    mov rax, [g_smap_enabled]
    test rax, rax
    jz .no_stac
    stac
.no_stac:

    mov rsp, r14            ; switch to user stack

    ; Push SS (user data segment with RPL=3)
    mov rax, 0x23
    push rax

    ; Push RSP (user stack pointer — same as current RSP)
    push rsp

    ; Push RFLAGS (enable interrupts)
    pushfq
    pop rax
    or rax, 0x200           ; set IF (interrupt flag)
    push rax

    ; Push CS (user code segment with RPL=3)
    mov rax, 0x1B
    push rax

    ; Push RIP (user entry point)
    push r15

    ; Clear AC before leaving supervisor mode (ring 3 ignores SMAP).
    mov rax, [g_smap_enabled]
    test rax, rax
    jz .no_clac
    clac
.no_clac:

    ; Set up segment registers for user mode
    mov ax, 0x23            ; USER_DS | RPL3
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    ; Zero out the general-purpose registers so the user process
    ; starts with a clean state (no kernel data leaks)
    xor rax, rax
    xor rbx, rbx
    xor rcx, rcx
    xor rdx, rdx
    xor rsi, rsi
    xor rdi, rdi
    xor rbp, rbp
    xor r8, r8
    xor r9, r9
    xor r10, r10
    xor r11, r11
    xor r12, r12
    xor r13, r13
    xor r14, r14
    xor r15, r15

    ; Jump to userspace!
    ; IRETQ pops RIP, CS, RFLAGS, RSP, SS from the stack and switches
    ; to the privilege level specified in CS (RPL=3 = user mode).

    iretq

section .data
save_kernel_cr3: dq 0
