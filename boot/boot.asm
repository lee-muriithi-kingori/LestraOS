;;
;; Lestra OS - Boot Entry Point
;; Copyright (c) 2026 lestramk.org
;;
;; Multiboot2 compliant boot header and entry point.
;; The bootloader (GRUB2) loads this code at 0x100000 (1MB).
;; We set up the higher half kernel mapping and transition to long mode.
;;

bits 32

section .multiboot2
align 8

; Multiboot2 header
MB2_MAGIC    equ 0xE85250D6
MB2_ARCH     equ 0            ; i386 protected mode

mb2_header:
    dd MB2_MAGIC
    dd MB2_ARCH
    dd mb2_header_end - mb2_header   ; Header length
    dd -(MB2_MAGIC + MB2_ARCH + (mb2_header_end - mb2_header)) ; Checksum

    ; Information request tag
    align 8
    dw 1                      ; Type: information request
    dw 0                      ; Flags
    dd 24                     ; Size
    dd 4                      ; Basic mem info
    dd 5                      ; BIOS boot device
    dd 6                      ; Memory map
    dd 9                      ; ELF sections
    dd 14                     ; ACPI old RSDP

    ; Address tag
    align 8
    dw 2                      ; Type: address
    dw 0                      ; Flags
    dd 24                     ; Size
    dd mb2_header             ; Header load address
    dd 0x100000               ; Load address (1MB)
    dd 0                      ; Load end address (0 = entire file)
    dd 0                      ; BSS end address (0 = no BSS)

    ; Entry address tag
    align 8
    dw 3                      ; Type: entry address
    dw 0                      ; Flags
    dd 12                     ; Size
    dd _start                 ; Entry point

    ; Flags tag (none required)
    align 8
    dw 4                      ; Type: flags
    dw 0                      ; Flags
    dd 8                      ; Size

    ; Module alignment tag
    align 8
    dw 6                      ; Type: module alignment
    dw 0                      ; Flags
    dd 8                      ; Size

    ; EFI boot services tag
    align 8
    dw 7                      ; Type: EFI boot services
    dw 0                      ; Flags
    dd 8                      ; Size

    ; End tag
    align 8
    dw 0                      ; Type: end
    dw 0                      ; Flags
    dd 8                      ; Size

mb2_header_end:

section .bss
align 4096

; Page tables for higher half kernel (0xFFFFFFFF80000000)
global boot_pml4
global boot_pdpt
global boot_pd
global boot_stack_bottom
global boot_stack_top

boot_pml4:
    resb 4096
boot_pdpt:
    resb 4096
boot_pd:
    resb 4096
boot_pt:
    resb 4096

; Boot stack (16KB)
boot_stack_bottom:
    resb 16384
boot_stack_top:

; Multiboot info pointer
mb_info_ptr:
    resd 1

section .text

global _start
extern kernel_main
extern __kernel_start
extern __kernel_end
extern __bss_start
extern __bss_end

_start:
    ; Save multiboot info pointer
    mov [mb_info_ptr], ebx

    ; Set up initial stack
    mov esp, boot_stack_top

    ; Zero BSS section
    mov edi, __bss_start
    mov ecx, __bss_end
    sub ecx, __bss_start
    shr ecx, 2              ; Divide by 4 for dwords
    xor eax, eax
    cld
    rep stosd

    ; Set up identity mapping for first 2MB and higher half
    call setup_page_tables

    ; Enable PAE and paging
    call enable_paging

    ; Load GDT and jump to long mode
    call enter_long_mode

    ; We should never reach here
    cli
    hlt
    jmp $

;-----------------------------------------------
; Set up page tables
; Maps first 2MB (identity) and higher half (0xFFFFFFFF80000000)
;-----------------------------------------------
setup_page_tables:
    pusha

    ; Clear page tables
    mov edi, boot_pml4
    xor eax, eax
    mov ecx, 4096 * 4 / 4   ; 4 pages, dwords
    cld
    rep stosd

    ; PML4[0] -> PDPT (identity mapping lower half)
    mov eax, boot_pdpt
    or eax, 0x03            ; Present + Writable
    mov [boot_pml4], eax

    ; PML4[256] -> PDPT (higher half mapping at 0xFFFFFFFF80000000)
    ; Index 256 = 0x800 in PML4 (bits 39-47)
    mov eax, boot_pdpt
    or eax, 0x03
    mov [boot_pml4 + 256 * 8], eax

    ; PDPT[0] -> PD
    mov eax, boot_pd
    or eax, 0x03
    mov [boot_pdpt], eax

    ; PD[0] -> 2MB page at 0x000000 (identity map first 2MB)
    mov eax, 0x00000000
    or eax, 0x83            ; Present + Writable + Huge page (2MB)
    mov [boot_pd], eax

    ; Also set up a page table for finer-grained mapping if needed
    mov eax, boot_pt
    or eax, 0x03
    mov [boot_pd], eax

    ; Map first 512 4KB pages (2MB) in PT
    mov edi, boot_pt
    xor eax, eax
    or eax, 0x03            ; Present + Writable
    mov ecx, 512
.pt_loop:
    mov [edi], eax
    add eax, 4096           ; Next physical page
    add edi, 8              ; Next PTE
    loop .pt_loop

    popa
    ret

;-----------------------------------------------
; Enable PAE and paging, then long mode
;-----------------------------------------------
enable_paging:
    ; Enable PAE
    mov eax, cr4
    or eax, 1 << 5          ; PAE bit
    mov cr4, eax

    ; Set CR3 to point to PML4
    mov eax, boot_pml4
    mov cr3, eax

    ; Enable long mode via EFER MSR
    mov ecx, 0xC0000080     ; EFER MSR
    rdmsr
    or eax, 1 << 8          ; LME (Long Mode Enable)
    wrmsr

    ; Enable paging
    mov eax, cr0
    or eax, 1 << 31         ; PG bit
    mov cr0, eax

    ret

;-----------------------------------------------
; GDT for long mode
;-----------------------------------------------
align 8

gdt64:
    dq 0                    ; Null descriptor

    ; Code segment (64-bit)
    dq (1 << 43) | (1 << 44) | (1 << 47) | (1 << 53)

    ; Data segment
    dq (1 << 41) | (1 << 44) | (1 << 47)

gdt64_end:

gdt64_ptr:
    dw gdt64_end - gdt64 - 1
    dq gdt64

CODE_SEG equ gdt64.code - gdt64
DATA_SEG equ gdt64.data - gdt64

;-----------------------------------------------
; Enter long mode
;-----------------------------------------------
enter_long_mode:
    ; Load GDT
    lgdt [gdt64_ptr]

    ; Far jump to 64-bit code
    jmp CODE_SEG:long_mode_start

bits 64

long_mode_start:
    ; Set up data segments
    mov ax, DATA_SEG
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; Set up stack pointer (higher half)
    mov rsp, (boot_stack_top - 0xFFFFFFFF80000000)
    add rsp, 0xFFFFFFFF80000000

    ; Jump to higher half
    mov rax, higher_half
    jmp rax

higher_half:
    ; Remove identity mapping (lower half)
    mov qword [boot_pml4], 0

    ; Reload CR3 to flush TLB
    mov rax, cr3
    mov cr3, rax

    ; Push multiboot info pointer as argument
    mov rdi, [mb_info_ptr]

    ; Call kernel main
    call kernel_main

    ; If kernel_main returns, halt
.halt:
    cli
    hlt
    jmp .halt
