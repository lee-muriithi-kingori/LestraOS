;;
;; Lestra OS - Context Switch v3 (x86_64) - KE-30
;;
;; Two modes:
;;
;; 1. ISR-swap (g_isr_frame != NULL):
;;    From timer IRQ -> sched_tick -> schedule.
;;    Writes new state into ISR frame on stack, ret so
;;    isr_common pops new regs and iretqs.
;;
;; 2. Direct (g_isr_frame == NULL):
;;    From C code (task_sleep, proc_exit).
;;    Builds iretq frame, iretqs to user space.
;;
;; Args: rdi=old_state rsi=new_state rdx=new_pml4 rcx=new_kstack_top
;;
;; struct cpu_state offsets:
;;   rax:0 rbx:8 rcx:0x10 rdx:0x18 rsi:0x20 rdi:0x28 rbp:0x30
;;   r8:0x38 r9:0x40 r10:0x48 r11:0x50 r12:0x58 r13:0x60
;;   r14:0x68 r15:0x70 (ret_addr:0x78)
;;   int_no:0x80 err_code:0x88
;;   rip:0x90 cs:0x98 rflags:0xA0 rsp:0xA8 ss:0xB0
;;
;; ISR frame offsets (from g_isr_frame):
;;   rax:0 rbx:8 rcx:0x10 rdx:0x18 rsi:0x20 rdi:0x28
;;   rbp:0x30 r8:0x38 r9:0x40 r10:0x48 r11:0x50
;;   r12:0x58 r13:0x60 r14:0x68 r15:0x70
;;   int_no:0x80 err_code:0x88
;;   rip:0x90 cs:0x98 rflags:0xA0 rsp:0xA8 ss:0xB0
;;

bits 64
default rel

global context_switch

; struct cpu_state offsets
%define CS_RAX    0x00
%define CS_RBX    0x08
%define CS_RCX    0x10
%define CS_RDX    0x18
%define CS_RSI    0x20
%define CS_RDI    0x28
%define CS_RBP    0x30
%define CS_R8     0x38
%define CS_R9     0x40
%define CS_R10    0x48
%define CS_R11    0x50
%define CS_R12    0x58
%define CS_R13    0x60
%define CS_R14    0x68
%define CS_R15    0x70
%define CS_INTNO  0x78
%define CS_ERR    0x80
%define CS_RIP    0x88
%define CS_CS     0x90
%define CS_RFLAGS 0x98
%define CS_RSP    0xA0
%define CS_SS     0xA8
%define CS_FSBASE 0xB0
%define CS_GSBASE 0xB8
%define CS_KGSBASE 0xC0
%define CS_KRSP   0xC8

; ISR frame offsets (from g_isr_frame)
;; ISR frame offsets (from g_isr_frame = RSP after all 15 GPRs pushed).
;; isr_common pushes rax FIRST (highest address) and r15 LAST (lowest,
;; i.e. at g_isr_frame itself). So from g_isr_frame:
;;   [0x00] r15 .. [0x70] rax  (15 GPRs, reversed from push order)
;;   [0x78] vector_number  (pushed 2nd by ISR stub)
;;   [0x80] error_code     (pushed 1st by ISR stub)
;;   [0x88] rip .. [0xA8] ss  (CPU interrupt frame)
;;
;; struct cpu_state layout (sched.h):
;;   [0x00] rax .. [0x70] r15  (push order, NOT stack order)
;;   [0x78] int_no  [0x80] err_code
;;   [0x88] rip .. [0xA8] ss  [0xB0] fs_base [0xB8] gs_base [0xC0] kgs_base [0xC8] krsp
;;
;; KE-32 BUGFIX: The original KE-30/31 offsets assumed g_isr_frame
;; pointed to rax (first push), but it actually points to r15 (last
;; push). All 15 GPR offsets were reversed. This was never caught
;; before because with only one process, the ISR-swap path never
;; executed (schedule() was a no-op). With fork() creating a
;; second process, the swap finally ran and the reversed GPRs
;; caused an immediate triple fault. */
%define ISR_R15    0x00
%define ISR_R14    0x08
%define ISR_R13    0x10
%define ISR_R12    0x18
%define ISR_R11    0x20
%define ISR_R10    0x28
%define ISR_R9     0x30
%define ISR_R8     0x38
%define ISR_RBP    0x40
%define ISR_RDI    0x48
%define ISR_RSI    0x50
%define ISR_RDX    0x58
%define ISR_RCX    0x60
%define ISR_RBX    0x68
%define ISR_RAX    0x70
%define ISR_VECTOR 0x78
%define ISR_ERR    0x80
%define ISR_RIP    0x88
%define ISR_CS     0x90
%define ISR_RFLAGS 0x98
%define ISR_RSP    0xA0
%define ISR_SS     0xA8

extern g_isr_frame
extern g_syscall_kstack
extern tss
section .text

context_switch:
    cli

    ;; FIX 1a: save original r15 before clobbering it.
    ;; `mov r15,rdi` would otherwise destroy the user r15 value that
    ;; .save_no_isr later needs to store into CS_R15. By pushing r15
    ;; first, the original value lives at [rsp+16] after the two
    ;; further pushes and can be reloaded for the CS_R15 store.
    push    r15
    ;; Save new_pml4 (rdx) and new_kstack_top (rcx) on the stack.
    ;; We will pop them after reading new_state (rbp).
    push    rdx
    push    rcx

    ;; Save old_state in r15 (callee-saved, won't be clobbered by calls)
    mov     r15, rdi
    ;; Save new_state in r11 (caller-saved, safe to clobber —
    ;; in ISR-swap mode, isr_common will restore r11 from the frame;
    ;; in direct mode, we restore it from new_state before iretq)
    mov     r11, rsi

    ;; =============================================
    ;; SAVE old process
    ;; =============================================
    test    r15, r15
    jz      .no_save

    ;; Save FS base MSR
    mov     ecx, 0xC0000100
    rdmsr
    mov     [r15 + CS_FSBASE], eax
    mov     [r15 + CS_FSBASE + 4], edx

    ;; Save GS base MSRs (0xC0000101 = GS.base, 0xC0000102 = KernelGS.base)
    ;; Mirror FS handling — both must be preserved per-process so that
    ;; user thread-local storage and per-CPU kernel GS don't leak across
    ;; context switches.
    mov     ecx, 0xC0000101
    rdmsr
    mov     [r15 + CS_GSBASE], eax
    mov     [r15 + CS_GSBASE + 4], edx
    mov     ecx, 0xC0000102
    rdmsr
    mov     [r15 + CS_KGSBASE], eax
    mov     [r15 + CS_KGSBASE + 4], edx

    ;; Check for ISR frame
    mov     r8, [g_isr_frame]
    test    r8, r8
    jz      .save_no_isr

    ;; Save all 15 GPRs from ISR frame to old_state
    mov     rax, [r8 + ISR_RAX];  mov [r15 + CS_RAX], rax
    mov     rax, [r8 + ISR_RBX];  mov [r15 + CS_RBX], rax
    mov     rax, [r8 + ISR_RCX];  mov [r15 + CS_RCX], rax
    mov     rax, [r8 + ISR_RDX];  mov [r15 + CS_RDX], rax
    mov     rax, [r8 + ISR_RSI];  mov [r15 + CS_RSI], rax
    mov     rax, [r8 + ISR_RDI];  mov [r15 + CS_RDI], rax
    mov     rax, [r8 + ISR_RBP];  mov [r15 + CS_RBP], rax
    mov     rax, [r8 + ISR_R8];   mov [r15 + CS_R8], rax
    mov     rax, [r8 + ISR_R9];   mov [r15 + CS_R9], rax
    mov     rax, [r8 + ISR_R10];  mov [r15 + CS_R10], rax
    mov     rax, [r8 + ISR_R11];  mov [r15 + CS_R11], rax
    mov     rax, [r8 + ISR_R12];  mov [r15 + CS_R12], rax
    mov     rax, [r8 + ISR_R13];  mov [r15 + CS_R13], rax
    mov     rax, [r8 + ISR_R14];  mov [r15 + CS_R14], rax
    mov     rax, [r8 + ISR_R15];  mov [r15 + CS_R15], rax

    ;; Save user return state
    mov     rax, [r8 + ISR_RIP];   mov [r15 + CS_RIP], rax
    mov     rax, [r8 + ISR_CS];    mov [r15 + CS_CS], rax
    mov     rax, [r8 + ISR_RFLAGS];mov [r15 + CS_RFLAGS], rax
    mov     rax, [r8 + ISR_RSP];  mov [r15 + CS_RSP], rax
    mov     rax, [r8 + ISR_SS];   mov [r15 + CS_SS], rax
    jmp     .save_done

.save_no_isr:
    ;; No ISR frame — save callee-saved from registers.
    ;; FIX 1a: original r15 was pushed at entry; it lives at [rsp+16].
    ;; Using `mov [r15+CS_R15],r15` would store the old_state pointer
    ;; (r15 == rdi) rather than the user r15. Reload the saved value.
    mov     [r15 + CS_RBX], rbx
    mov     [r15 + CS_RBP], rbp
    mov     [r15 + CS_R12], r12
    mov     [r15 + CS_R13], r13
    mov     [r15 + CS_R14], r14
    mov     rax, [rsp + 16]
    mov     [r15 + CS_R15], rax

    ;; FIX 1b: save voluntary kernel RSP so the blocked task's call
    ;; stack is not lost. Direct-mode currently discards old RSP by
    ;; `mov rsp,r9` to the new kstack. Persist entry RSP (return address
    ;; slot) into CS_KRSP for a future kernel-restore path. The
    ;; scheduler's voluntary-block helpers (task_block/task_sleep) must
    ;; ensure kernel state is saved; see scheduler.c comment.
    mov     rax, rsp
    add     rax, 24
    mov     [r15 + CS_KRSP], rax

.save_done:
.no_save:
    ;; =============================================
    ;; Mode decision: ISR-swap or direct?
    ;; =============================================
    mov     r8, [g_isr_frame]
    test    r8, r8
    jnz     .isr_swap

    ;; =============================================
    ;; DIRECT MODE
    ;; =============================================
    ;; FIX 1a stack: we pushed r15,r dx,rcx (3 slots) at entry.
    ;; Pop new_kstack_top (rcx) and new_pml4 (rdx), then discard saved r15.
    ;; FIX 1b: direct mode builds iret frame on new_kstack_top. This
    ;; correctly builds the user return frame without touching the old
    ;; kernel call stack (which remains at saved CS_KRSP). Callers in
    ;; voluntary-block path (task_block/task_sleep) must save kernel RSP
    ;; into prev->saved_state->krsp before calling schedule(); see
    ;; scheduler.c voluntary-block comment.
    pop     r9                        ; r9  = new_kstack_top
    pop     r8                        ; r8  = new_pml4
    add     rsp, 8                    ; discard saved original r15 (FIX 1a)

    ;; Switch CR3
    mov     rax, cr3
    cmp     rax, r8
    je      .dir_cr3_ok
    mov     cr3, r8
.dir_cr3_ok:

    ;; Update TSS.RSP0 and g_syscall_kstack
    mov     [tss + 4], r9
    mov     [g_syscall_kstack], r9

    ;; Switch to new kernel stack, build iretq frame
    mov     rsp, r9
    sub     rsp, 8                    ; alignment

    ;; Read all values from new_state (r11) into temporaries FIRST
    mov     rax, [r11 + CS_RIP]
    mov     rcx, [r11 + CS_CS]
    mov     rdx, [r11 + CS_RFLAGS]
    mov     rsi, [r11 + CS_RSP]
    mov     rdi, [r11 + CS_SS]
    mov     rbx, [r11 + CS_RBX]
    mov     rbp, [r11 + CS_RBP]
    mov     r12, [r11 + CS_R12]
    mov     r13, [r11 + CS_R13]
    mov     r14, [r11 + CS_R14]
    mov     r15, [r11 + CS_R15]
    mov     r8,  [r11 + CS_R8]
    mov     r9,  [r11 + CS_R9]
    mov     r10, [r11 + CS_R10]

    ;; Restore FS base
    mov     ecx, 0xC0000100
    mov     eax, [r11 + CS_FSBASE]
    mov     edx, [r11 + CS_FSBASE + 4]
    wrmsr
    ;; Restore GS bases (FIX 1c)
    mov     ecx, 0xC0000101
    mov     eax, [r11 + CS_GSBASE]
    mov     edx, [r11 + CS_GSBASE + 4]
    wrmsr
    mov     ecx, 0xC0000102
    mov     eax, [r11 + CS_KGSBASE]
    mov     edx, [r11 + CS_KGSBASE + 4]
    wrmsr

    ;; Push iretq frame: SS, RSP, RFLAGS, CS, RIP
    push    rdi                      ; SS
    push    rsi                      ; RSP
    push    rdx                      ; RFLAGS
    push    rcx                      ; CS
    push    rax                      ; RIP

    ;; Restore remaining GPRs
    mov     rax, [r11 + CS_RAX]
    mov     rcx, [r11 + CS_RCX]
    mov     rdx, [r11 + CS_RDX]
    mov     rsi, [r11 + CS_RSI]
    mov     rdi, [r11 + CS_RDI]
    mov     r11, [r11 + CS_R11]

    iretq

    ;; =============================================
    ;; ISR-SWAP MODE
    ;; =============================================
.isr_swap:
    ;; FIX 1a stack: we pushed r15,rdx,rcx (3 slots). Layout:
    ;; [rsp]=rcx (new_kstack_top), [rsp+8]=rdx (new_pml4),
    ;; [rsp+16]=saved r15, [rsp+24]=return address.
    ;; KE-32 BUGFIX: rdx (new_pml4) was pushed first → [rsp+8],
    ;; rcx (new_kstack_top) pushed second → [rsp].
    ;; Original KE-31 had these swapped, causing CR3 to be loaded
    ;; with the kernel stack address (instant triple-fault).
    mov     r9,  [rsp]                 ; r9  = new_kstack_top
    mov     r10, [rsp + 8]             ; r10 = new_pml4

    ;; Write new process's GPRs into ISR frame
    mov     rax, [r11 + CS_RAX];   mov [r8 + ISR_RAX], rax
    mov     rax, [r11 + CS_RBX];   mov [r8 + ISR_RBX], rax
    mov     rax, [r11 + CS_RCX];   mov [r8 + ISR_RCX], rax
    mov     rax, [r11 + CS_RDX];   mov [r8 + ISR_RDX], rax
    mov     rax, [r11 + CS_RSI];   mov [r8 + ISR_RSI], rax
    mov     rax, [r11 + CS_RDI];   mov [r8 + ISR_RDI], rax
    mov     rax, [r11 + CS_RBP];   mov [r8 + ISR_RBP], rax
    mov     rax, [r11 + CS_R8];    mov [r8 + ISR_R8], rax
    mov     rax, [r11 + CS_R9];    mov [r8 + ISR_R9], rax
    mov     rax, [r11 + CS_R10];   mov [r8 + ISR_R10], rax
    mov     rax, [r11 + CS_R11];   mov [r8 + ISR_R11], rax
    mov     rax, [r11 + CS_R12];   mov [r8 + ISR_R12], rax
    mov     rax, [r11 + CS_R13];   mov [r8 + ISR_R13], rax
    mov     rax, [r11 + CS_R14];   mov [r8 + ISR_R14], rax
    mov     rax, [r11 + CS_R15];   mov [r8 + ISR_R15], rax

    ;; Write new process's user return state into ISR frame
    mov     rax, [r11 + CS_RIP];   mov [r8 + ISR_RIP], rax
    mov     rax, [r11 + CS_CS];    mov [r8 + ISR_CS], rax
    mov     rax, [r11 + CS_RFLAGS];mov [r8 + ISR_RFLAGS], rax
    mov     rax, [r11 + CS_RSP];  mov [r8 + ISR_RSP], rax
    mov     rax, [r11 + CS_SS];   mov [r8 + ISR_SS], rax

    ;; Switch CR3 to new process's page table
    ;; KE-36 DIAG: capture return address ([rsp+24]) before CR3 switch (FIX 1a offset)
    mov     rax, [rsp + 24]
    mov     [rel g_ke36_ret_before_cr3], rax
    mov     rax, cr3
    cmp     rax, r10
    je      .isr_cr3_ok
    mov     cr3, r10                   ; r10 = new_pml4
.isr_cr3_ok:
    ;; KE-36 DIAG: capture return address ([rsp+24]) after CR3 switch
    mov     rax, [rsp + 24]
    mov     [rel g_ke36_ret_after_cr3], rax

    ;; Update TSS.RSP0 and g_syscall_kstack for new process
    mov     [tss + 4], r9              ; r9 = new_kstack_top
    mov     [g_syscall_kstack], r9

    ;; Restore FS base
    mov     ecx, 0xC0000100
    mov     eax, [r11 + CS_FSBASE]
    mov     edx, [r11 + CS_FSBASE + 4]
    wrmsr
    ;; Restore GS bases (FIX 1c)
    mov     ecx, 0xC0000101
    mov     eax, [r11 + CS_GSBASE]
    mov     edx, [r11 + CS_GSBASE + 4]
    wrmsr
    mov     ecx, 0xC0000102
    mov     eax, [r11 + CS_KGSBASE]
    mov     edx, [r11 + CS_KGSBASE + 4]
    wrmsr

    ;; KE-35 FIX: Do NOT restore callee-saved regs (rbx/rbp/r12-r15) or
    ;; rax from new_state here. (See comment block below for rationale.)
    ;;
    ;; The callee-saved regs must stay as the KERNEL's values until
    ;; isr_common's pop-all-GPRs sequence replaces them with the child's
    ;; values just before iretq.

    ;; Return to interrupt_dispatch -> isr_common.
    ;; isr_common will pop all 15 GPRs (now the new process's values)
    ;; and iretq using the updated interrupt frame.
    add     rsp, 24                   ; clean up 3 saved args (FIX 1a was 16)
    ;; KE-36 DIAG: capture [rsp] right before ret (after add rsp, 24)
    mov     rax, [rsp]
    mov     [rel g_ke36_ret_at_ret], rax
    ret

;; KE-36 DIAG globals: capture the return address before/after CR3 switch
;; and right before ret, to determine where the return address gets corrupted.
section .data
global g_ke36_ret_before_cr3
global g_ke36_ret_after_cr3
global g_ke36_ret_at_ret
g_ke36_ret_before_cr3: dq 0
g_ke36_ret_after_cr3:  dq 0
g_ke36_ret_at_ret:     dq 0
