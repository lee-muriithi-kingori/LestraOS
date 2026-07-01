; LestraOS - Stage1 Bootloader v4
; NASM 2.16+ compatible
; Load raw kernel at 0x100000, set up paging, switch to 64-bit, jump to kernel
BITS 16
ORG 0x7C00

KERNEL_LBA    equ 1
KERNEL_SECTS  equ 256
KERNEL_PADDR  equ 0x100000
KERNEL_ENTRY  equ 0x100010   ; raw binary: stub(16B) + data starting at 0x1000

; Memory layout:
;  0x7C00: this bootloader (512B)
;  0x7E00: GDT (24B)
;  0x8000: PML4  (4KB)
;  0x9000: PDPT  (4KB)
;  0xA000: PDT   (4KB)
;  0x100000: KERNEL

PML4_ADDR     equ 0x8000
PDPT_ADDR     equ 0x9000
PDT_ADDR      equ 0xA000
GDT_ADDR      equ 0x7E00

; ============================================================
; ENTRY
; ============================================================
entry:
    cld
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    mov [boot_drive], dl

    mov si, msg_loading
    call print16

    ; ---- Load kernel from LBA 1 into KERNEL_PADDR ----
    mov bx, KERNEL_PADDR >> 4
    mov es, bx
    xor bx, bx
    mov ah, 0x42
    mov al, KERNEL_SECTS
    mov si, dap
    int 0x13
    jc disk_error

    mov si, msg_loaded
    call print16

    ; ---- Debug: write '1' to top-left of screen ----
    mov word [0xB8000], 0x0F31  ; light white '1' on black

    ; ---- Enable A20 ----
    mov word [0xB8002], 0x0F32  ; '2'
    in al, 0x92
    or al, 2
    out 0x92, al

    ; ---- Build GDT ----
    ; Use DS:offset = physical address (DS=0 in real mode)
    mov di, GDT_ADDR
    xor eax, eax
    stosd
    stosd                           ; null descriptor

    ; code: base=0, limit=0xFFFFF, P+DPL0+code+R, L=1, D=0, G=1
    ; L=1 (64-bit mode), D=0 (default operand size 64)
    mov eax, 0x0000FFFF
    mov edx, 0x00AF9A00
    stosd
    stosd

    ; data: base=0, limit=0xFFFFF, P+DPL0+data+R/W, D=1, G=1
    mov eax, 0x0000FFFF
    mov edx, 0x00CF9200
    stosd
    stosd

    ; Load GDTR
    mov word [gdtr_val], 0x001F
    mov dword [gdtr_val + 2], GDT_ADDR
    lgdt [gdtr_val]
    mov word [0xB8004], 0x0F33  ; '3'

    ; ---- Build page tables (identity-map bottom 2MB) ----
    ; Clear PML4
    mov di, PML4_ADDR
    mov cx, 512
    xor eax, eax
    rep stosd

    ; PML4[0] -> PDPT
    mov dword [PML4_ADDR], PDPT_ADDR | 3

    ; Clear PDPT
    mov di, PDPT_ADDR
    mov cx, 256
    xor eax, eax
    rep stosd

    ; PDPT[0] -> PDT
    mov dword [PDPT_ADDR], PDT_ADDR | 3

    ; Clear PDT
    mov di, PDT_ADDR
    mov cx, 512
    xor eax, eax
    rep stosd

    ; PDT[0]: 2MB page at phys 0 (present + writable + PS)
    mov dword [PDT_ADDR], 0x00000083

    ; ---- Enable PAE ----
    mov word [0xB8008], 0x0F35  ; '5'
    mov eax, cr4
    or eax, 0x20
    mov cr4, eax

    ; ---- Load CR3 ----
    mov word [0xB800A], 0x0F36  ; '6'
    mov eax, PML4_ADDR
    mov cr3, eax

    ; ---- Enable Long Mode (IA32_EFER.LME) ----
    mov word [0xB800C], 0x0F37  ; '7'
    mov ecx, 0xC0000080
    rdmsr
    or eax, 0x100
    wrmsr

    ; ---- Enable paging (CR0.PG) ----
    mov word [0xB800E], 0x0F38  ; '8'
    mov eax, cr0
    or eax, 0x80000000
    mov cr0, eax
    mov word [0xB87FFE], 0x0F39  ; '9' at bottom-right to confirm paging is on

    ; ---- Far jump to 64-bit code segment ----
    ; 0x08 = code segment selector in our GDT
    jmp 0x0008:pm_switch

; ============================================================
; 32-bit protected mode stub (after far jump, CS=0x08)
; ============================================================
BITS 32
pm_switch:
    ; Reload data segment registers
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; Set stack
    mov esp, 0x80000

    ; ---- Far jump to 64-bit code segment ----
    ; This transitions from 32-bit protected mode to 64-bit long mode
    jmp 0x0008:.enter64

.enter64:
    ; Now in 64-bit mode!
    ; Pass 0 as mb_info pointer (no multiboot2 info from our custom bootloader)
    xor edi, edi           ; RDI = 0

    ; Jump to kernel entry (in 64-bit mode)
    ; The raw kernel has _start at file offset 0x1000, which is at physical KERNEL_PADDR + 0x1000
    jmp 0x101000

; ============================================================
; 64-bit mode halt loop (fallback, should never run)
; ============================================================
BITS 64
halt64:
    hlt
    jmp halt64

; ============================================================
; Subroutines (16-bit real mode)
; ============================================================
print16:
    lodsb
    test al, al
    jz .done
    mov ah, 0x0E
    int 0x10
    jmp print16
.done:
    ret

disk_error:
    mov si, msg_disk_err
    call print16
    jmp $

; ============================================================
; Data
; ============================================================
msg_loading:  db 'Loading kernel...', 13, 10, 0
msg_loaded:   db 'OK. Switching to long mode...', 13, 10, 0
msg_disk_err: db 'DISK ERROR!', 13, 10, 0
boot_drive:   db 0

dap:
    db 16
    db 0
    dw KERNEL_SECTS
    dw 0x0000
    dw KERNEL_PADDR >> 4
    dq KERNEL_LBA

gdtr_val:
    dw 0x001F
    dd GDT_ADDR

; Pad and boot signature
times 510 - ($ - $$) db 0
dw 0xAA55
