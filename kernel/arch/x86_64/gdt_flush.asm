;;
;; Lestra OS - GDT Flush (x86_64)
;; Copyright (c) 2026 lestramk.org
;;

bits 64

global gdt_flush

; void gdt_flush(uint64_t gdt_ptr, uint16_t cs, uint16_t ds)
gdt_flush:
    ; Load GDT pointer
    mov rax, rdi
    lgdt [rax]
    
    ; Reload CS via far return
    push rsi            ; CS
    lea rax, [rel .reload]
    push rax
    o64 retf
    
.reload:
    ; Reload data segments
    mov ax, dx
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    ret
