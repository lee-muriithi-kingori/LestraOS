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
global g_syscall_user_rip
global g_syscall_user_rflags
section .data
g_syscall_kstack:    dq 0
g_saved_user_rsp:    dq 0
g_syscall_user_rip:    dq 0
g_syscall_user_rflags: dq 0

section .text
syscall_entry:
    swapgs                          ; kernel GS base now active

    ; KE-25: switch to the kernel stack BEFORE pushing anything.
    mov [g_saved_user_rsp], rsp     ; save user RSP
    mov rsp, [g_syscall_kstack]     ; load kernel stack top

    ; KE-31: Save user RIP (RCX) and RFLAGS (R11) to globals so
    ; proc_fork() can build the child's return state. Without these,
    ; fork's child would resume from stale saved_state (set by
    ; proc_create and never updated by context_switch if the parent
    ; was never preempted).
    mov [g_syscall_user_rip], rcx
    mov [g_syscall_user_rflags], r11

    ; Save user RIP (RCX) and RFLAGS (R11) on stack (for syscall return)
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
    pop r11                     ; r11 = user RFLAGS
    pop rcx                     ; rcx = user RIP

    ; KE-26: Return via IRETQ (not SYSRETQ).
    ;
    ; SYSRETQ computes CS/SS from the STAR MSR, but QEMU's qemu64 +smep
    ; was loading CS=0x08 (KERNEL_CS) instead of the computed 0x23,
    ; causing the CPU to run /init's .text in ring 0 → SMEP #PF. Rather
    ; than chase the QEMU sysret quirk, we use IRETQ which takes an
    ; EXPLICIT frame (RIP, CS, RFLAGS, RSP, SS) from the kernel stack.
    ; CS is exactly what we push (0x23 = USER_CS|RPL3), not what STAR
    ; computes. This is the same return mechanism used by interrupt
    ; handlers and context_switch — unambiguous and SMEP/SMAP-safe.
    ;
    ; RAX holds the syscall return value from syscall_dispatch. We must
    ; NOT clobber it while building the iretq frame. R10 is caller-saved
    ; and free after the dispatch returns (it held a4, the 4th syscall
    ; arg, which we don't need anymore). Use R10 as the temp.
    ;
    ; We're still on the kernel stack (g_syscall_kstack). The iretq frame
    ; is pushed onto kernel pages (no SMAP issue). iretq pops it, loads
    ; RSP from the frame (user RSP), and switches us to ring 3.
    ;
    ; swapgs MUST happen before iretq (after iretq we're in ring 3 and
    ; swapgs would #GP).
    mov r10, rax                ; r10 = syscall return value (preserve!)

    swapgs                      ; restore user GS base

    ; Push the IRETQ frame onto the kernel stack:
    ;   SS, RSP, RFLAGS, CS, RIP (pushed in reverse order for iretq)
    mov rax, 0x1B               ; USER_DS | RPL3 (KE-26: 0x18 | 3)
    push rax                    ; SS
    push qword [g_saved_user_rsp] ; RSP (user stack pointer)
    push r11                    ; RFLAGS (user RFLAGS, saved by `syscall`)
    mov rax, 0x23               ; USER_CS | RPL3 (KE-26: 0x20 | 3)
    push rax                    ; CS
    push rcx                    ; RIP (user return address, saved by `syscall`)

    ; Restore the syscall return value to RAX (the libc wrapper reads
    ; the result from RAX).
    mov rax, r10

    ; IRETQ pops RIP, CS, RFLAGS, RSP, SS from the stack and switches
    ; to the privilege level in CS (RPL=3 = ring 3).
    iretq
