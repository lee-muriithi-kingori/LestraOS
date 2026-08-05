;;
;; Lestra OS - Syscall Entry Point (x86_64)
;; Copyright (c) 2026 lestramk.org
;;
;; Handles syscall instruction from userspace.
;; Arguments: RAX=num, RDI=a1, RSI=a2, RDX=a3, R10=a4, R8=a5, R9=a6
;; C calling convention: RDI=num, RSI=a1, RDX=a2, RCX=a3, R8=a4, R9=a5, [rsp]=a6
;;
;; The kernel does NOT use GS-relative per-CPU data yet, but swapgs
;; is still needed for correctness — without it a user-set GS base
;; would leak into any future kernel code that touches the GS segment.
;;

bits 64

global syscall_entry
extern syscall_dispatch

; KE-25: Kernel stack for syscall entry.
; The `syscall` instruction does NOT load a new RSP (unlike interrupt
; delivery, which loads RSP from TSS.RSP0 on a privilege change). Without
; an explicit stack switch here, the kernel would push registers onto the
; USER stack. Under SMAP (CR4.SMAP=1) that is a supervisor write to a
; user page with AC clear → #PF; the #PF handler runs at CPL=0 with the
; SAME user RSP, tries to push its frame to the user stack → another SMAP
; #PF → recursive fault → #DF → triple fault. This was the SMAP-on
; triple-fault root cause.
;
; g_syscall_kstack is set by sched_start_first() (and context_switch) to
; the current process's kernel_stack_top. We save the user RSP and switch
; to it before any push, then restore the user RSP before sysretq (which
; does not touch RSP).
global g_syscall_kstack
global g_saved_user_rsp
section .data
g_syscall_kstack:    dq 0
g_saved_user_rsp:    dq 0

section .text
syscall_entry:
    swapgs                          ; kernel GS base now active

    ; KE-25: switch to the kernel stack BEFORE pushing anything.
    mov [g_saved_user_rsp], rsp     ; save user RSP
    mov rsp, [g_syscall_kstack]     ; load kernel stack top

    ; Save user RIP (RCX) and RFLAGS (R11)
    push rcx
    push r11

    ; Save callee-saved registers
    push rbx
    push r12
    push r13
    push r14
    push r15
    push rbp

    ; Save original R9 (a6) — it becomes the 7th C argument on the stack
    push r9

    ; Shuffle registers from syscall convention to C calling convention:
    ; syscall: RAX=num, RDI=a1, RSI=a2, RDX=a3, R10=a4, R8=a5, R9=a6
    ; C call:  RDI=num, RSI=a1, RDX=a2, RCX=a3, R8=a4,  R9=a5, [rsp]=a6
    ; Use R11 and RAX as temporaries
    mov r11, rax                ; r11 = num
    mov rax, rdi                ; rax = a1
    mov rdi, r11                ; rdi = num
    mov r11, rsi                ; r11 = a2
    mov rsi, rax                ; rsi = a1
    mov rax, rdx                ; rax = a3
    mov rdx, r11                ; rdx = a2
    mov r11, r8                 ; r11 = a5
    mov rcx, rax                ; rcx = a3
    mov rax, r10                ; rax = a4
    mov r8, rax                 ; r8  = a4
    mov r9, r11                 ; r9  = a5

    call syscall_dispatch

    add rsp, 8                  ; pop saved a6

    ; Restore registers
    pop rbp
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop r11
    pop rcx

    ; KE-25: restore the user RSP before sysretq (sysretq loads RIP from
    ; RCX and RFLAGS from R11 but does NOT change RSP).
    mov rsp, [g_saved_user_rsp]

    swapgs                      ; restore user GS base

    ; sysretq: loads RIP from RCX, RFLAGS from R11
    sysretq
