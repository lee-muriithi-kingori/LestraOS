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

syscall_entry:
    swapgs                          ; kernel GS base now active

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

    swapgs                      ; restore user GS base

    ; sysretq: loads RIP from RCX, RFLAGS from R11
    sysretq
