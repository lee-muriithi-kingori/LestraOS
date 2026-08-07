;;
;; Lestra OS - Signal trampoline
;; Copyright (c) 2026 lestramk.org / Lee Muriithi Kingori
;;
;; This code is copied into each user process's address space at a
;; fixed virtual address (signal_trampoline_addr) by the kernel when
;; the first sigaction() is installed. After the kernel rewrites the
;; saved RIP to point at a user's signal handler, it arranges for the
;; trampoline's address to be pushed on the user stack so that when
;; the handler returns (`ret`), control lands here.
;;
;; The trampoline's job is to issue rt_sigreturn (LestraOS syscall 15
;; in the Linux personality, or the native sigreturn syscall) so the
;; kernel can restore the pre-signal CPU state from
;; task->pre_signal_state.
;;
;; Layout:
;;   sig_trampoline_start:
;;       mov rdi, [rsp+8]      ; pass signal number (pushed by kernel)
;;       call rax               ; rax = signal handler address (set by kernel)
;;       mov rax, 15            ; rt_sigreturn syscall number
;;       syscall                ; never returns
;;       ud2                    ; trap if we ever fall through
;;   sig_trampoline_end:
;;
;; The kernel reads (sig_trampoline_end - sig_trampoline_start) bytes
;; starting at sig_trampoline_start to know how much to copy.

bits 64

global sig_trampoline_start
global sig_trampoline_end

section .text

sig_trampoline_start:
    mov     rdi, [rsp+8]        ; signal number (kernel pushed it)
    call    rax                 ; rax holds handler address (set by kernel)
    mov     rax, 15             ; SYS_rt_sigreturn
    syscall
    ud2                         ; should never reach here
sig_trampoline_end:
