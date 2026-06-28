;;
;; Lestra OS - ISR Stubs (x86_64)
;; Copyright (c) 2026 lestramk.org
;;
;; Assembly stubs for all 256 interrupt vectors.
;; Each stub saves CPU state and calls the C interrupt dispatcher.
;;

bits 64

global isr_stubs
extern interrupt_dispatch

; Macro for ISR without error code
%macro ISR_NOERR 1
global isr_%1
isr_%1:
    push 0              ; Dummy error code
    push %1             ; Interrupt vector
    jmp isr_common
%endmacro

; Macro for ISR with error code
%macro ISR_ERR 1
global isr_%1
isr_%1:
    push %1             ; Interrupt vector
    jmp isr_common
%endmacro

; Generate ISR stubs for all 256 vectors
; Exceptions 0-31
ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_NOERR 21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_NOERR 29
ISR_ERR   30
ISR_NOERR 31

; IRQs 32-47
%assign i 32
%rep 16
    ISR_NOERR i
%assign i i+1
%endrep

; Remaining vectors 48-255
%assign i 48
%rep 208
    ISR_NOERR i
%assign i i+1
%endrep

; Common ISR handler
isr_common:
    ; Save all general purpose registers
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    
    ; Call C handler with pointer to saved state
    mov rdi, rsp
    call interrupt_dispatch
    
    ; Restore all registers
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
    
    ; Clean up error code and vector number
    add rsp, 16
    iretq

; Array of ISR stub addresses
section .data
align 8

global isr_stubs
isr_stubs:
%assign i 0
%rep 256
    dq isr_%+i
%assign i i+1
%endrep
