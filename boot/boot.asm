;;
;; Lestra OS - Boot Entry Point (FIXED)
;; Copyright (c) 2026 lestramk.org
;;
;; Multiboot2 compliant boot header and entry point.
;; Loaded at physical 0x100000 (1MB) by GRUB/multiboot2.
;; Sets up identity mapping for first 4GB, enters long mode, calls kernel_main.
;;
;; FIXES:
;;  - Removed broken higher-half alias (PDPT[510] was never set, so
;;    0xFFFFFFFF80000000 would page fault). Kernel runs identity-mapped
;;    at 0x100000+ for now.
;;  - Use 2MB huge pages to identity-map first 4GB (8 PD entries -> 4GB? no,
;;    1 PD = 512 entries * 2MB = 1GB per PD. We map the first 1GB which
;;    is plenty for QEMU 4GB RAM regions the kernel needs).
;;  - Save multiboot info pointer in a 64-bit-safe location.
;;

bits 32
SERIAL_PORT equ 0x3F8

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

    ; Information request tag.
    ; FIX: original code requested 5 mbi tags (basic mem, boot device,
    ; mmap, framebuffer, ACPI RSDP) but declared size=24 which only
    ; covers 4 tags (size = 8 header + 4*4 data = 24). GRUB read past
    ; the declared size, hit the next tag's bytes, and choked with
    ; "unsupported tag: 0xe" because it tried to interpret the
    ; tag-14 dword as a new tag header.
    ;
    ; We also drop the ACPI RSDP request entirely - the kernel doesn't
    ; parse ACPI yet, so requesting it just gives GRUB more chances to
    ; complain. 4 tags * 4 bytes + 8-byte header = 24 bytes, matches.
    align 8
    dw 1                      ; Type: information request
    dw 0                      ; Flags
    dd 24                     ; Size = 8 (header) + 4*4 (4 mbi tag ids)
    dd 4                      ; Basic mem info
    dd 5                      ; BIOS boot device
    dd 6                      ; Memory map
    dd 8                      ; Framebuffer info

    ; Entry address tag - tells bootloader to jump to _start
    align 8
    dw 3                      ; Type: entry address
    dw 0                      ; Flags
    dd 12                     ; Size
    dd _start                 ; Entry point

    ; Module alignment tag
    align 8
    dw 6                      ; Type: module alignment
    dw 0                      ; Flags
    dd 8                      ; Size

    ; Framebuffer tag - request a specific graphics mode from GRUB.
    ; Type 5 = framebuffer. Width/height/depth = requested mode.
    ; GRUB will try to set 1024×768×32 and provide the linear framebuffer
    ; address in the multiboot2 info struct (tag type 8).
    align 8
    dw 5                      ; Type: framebuffer
    dw 0                      ; Flags
    dd 20                     ; Size = 8 (header) + 3*4 (width, height, depth)
    dd 1024                   ; Width  (0 = any)
    dd 768                    ; Height (0 = any)
    dd 32                     ; Depth  (0 = any)

    ; End tag
    align 8
    dw 0                      ; Type: end
    dw 0                      ; Flags
    dd 8                      ; Size

mb2_header_end:

section .bss
align 4096

; Page tables for identity mapping 4 GB with 2 MB huge pages.
; Layout: 1 PML4 (4 KB) + 1 PDPT (4 KB) + 4 PDs (16 KB) = 24 KB total.
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
    resb 4096 * 4      ; 4 PDs to cover 4 GB

; Boot stack (16KB)
boot_stack_bottom:
    resb 16384
boot_stack_top:

; Multiboot info pointer (8 bytes for 64-bit safety)
mb_info_ptr:
    resq 1

section .text

global _start
extern kernel_main
extern __kernel_start
extern __kernel_end
extern __bss_start
extern __bss_end

_start:
    ; Serial mark 'B' for "we got here"
    mov dx, SERIAL_PORT
    mov al, 'B'
    out dx, al

    ; FIX: do NOT save EBX (the multiboot2 info pointer) to mb_info_ptr
    ; yet. mb_info_ptr lives in .bss, and the BSS-zeroing loop below
    ; would wipe it. EBX is preserved across `rep stosd` (which only
    ; touches eax/edi/ecx), so we save it AFTER the zeroing.
    mov dx, SERIAL_PORT
    mov al, '1'
    out dx, al

    ; Write 'B' to top-left of screen (VGA memory 0xB8000)
    mov word [0xB8000], 0x0F42   ; 'B' for "Boot reached"
    mov dx, SERIAL_PORT
    mov al, '2'
    out dx, al

    ; Set up initial stack
    mov esp, boot_stack_top
    mov ebp, esp
    mov dx, SERIAL_PORT
    mov al, '3'
    out dx, al

    ; Zero BSS section. rep stosd uses only eax/edi/ecx, so ebx (which
    ; holds the multiboot2 info pointer passed by GRUB) is preserved.
    mov edi, __bss_start
    mov ecx, __bss_end
    sub ecx, __bss_start
    shr ecx, 2              ; Divide by 4 for dwords
    xor eax, eax
    cld
    rep stosd

    ; NOW safe to save EBX to mb_info_ptr (.bss has just been zeroed).
    mov [mb_info_ptr], ebx
    mov dx, SERIAL_PORT
    mov al, '4'
    out dx, al

    ; Set up identity mapping for first 1GB using 2MB huge pages
    call setup_page_tables
    mov dx, SERIAL_PORT
    mov al, '5'
    out dx, al

    ; Enable PAE and paging
    call enable_paging
    mov dx, SERIAL_PORT
    mov al, '6'
    out dx, al

    ; Load GDT and jump to long mode
    call enter_long_mode
    mov dx, SERIAL_PORT
    mov al, 'L'
    out dx, al

    ; We should never reach here
    cli
    hlt
    jmp $

;-----------------------------------------------
; Set up page tables
; Identity-map the first 4 GB of physical memory using 2 MB huge pages.
; 4 GB covers: kernel image (low 1 MB), VGA (0xB8000), kernel heap
; (0x10000000-0x30000000), AND PCI device MMIO regions (typically
; 0xFEBxxxxx for QEMU's E1000). Without the 4 GB map, the E1000 driver
; page-faults when it tries to read its MMIO registers.
;
; Layout:
;   PML4[0]    -> PDPT (covers low 512 GB)
;   PDPT[0]    -> PD   (covers low 1 GB)      [PDPT[1] covers 1-2 GB,
;                                              PDPT[2] covers 2-3 GB,
;                                              PDPT[3] covers 3-4 GB]
;   We need 4 PDs (one per GB), each with 512 entries.
;-----------------------------------------------
setup_page_tables:
    pusha

    ; Clear all page-table pages: 1 PML4 + 1 PDPT + 4 PDs = 6 pages = 24 KB
    mov edi, boot_pml4
    xor eax, eax
    mov ecx, (4096 * 6) / 4
    cld
    rep stosd

    ; PML4[0] -> PDPT
    mov eax, boot_pdpt
    or eax, 0x03            ; Present + Writable
    mov [boot_pml4], eax

    ; PDPT[0..3] -> 4 PDs (one per GB, covering 0..4 GB)
    mov eax, boot_pd
    or eax, 0x03
    mov [boot_pdpt + 0], eax     ; GB 0
    mov eax, boot_pd + 0x1000
    or eax, 0x03
    mov [boot_pdpt + 8], eax     ; GB 1
    mov eax, boot_pd + 0x2000
    or eax, 0x03
    mov [boot_pdpt + 16], eax    ; GB 2
    mov eax, boot_pd + 0x3000
    or eax, 0x03
    mov [boot_pdpt + 24], eax    ; GB 3

    ; Fill 4 PDs (4 * 512 = 2048 entries) with huge 2 MB pages,
    ; starting at physical 0 and incrementing by 2 MB each entry.
    mov edi, boot_pd
    xor eax, eax            ; start at physical 0
    mov ecx, 2048           ; 4 GB / 2 MB = 2048 entries
.lp:
    or eax, 0x83            ; Present + Writable + Huge (2 MB)
    mov [edi], eax
    add edi, 8
    add eax, 0x200000       ; next 2 MB chunk
    loop .lp

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

gdt64.code:
    ; 64-bit code segment descriptor.
    ; Access byte = 0x9B: P=1, DPL=00, S=1, Type=0xB (Execute/Read/Accessed).
    ; Flags byte = 0xAF: G=1, D=0, L=1 (long mode), AVL=0; limit 19:16 = 0xF.
    dq 0x00AF9B000000FFFF

gdt64.data:
    ; 64-bit data segment: P=1, DPL=00, S=1, Type=0x3 (Read/Write/Accessed).
    ; Flags = 0xCF: G=1, D=1 (32-bit operand size), AVL=0.
    dq 0x00CF93000000FFFF

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

    ; Set up stack pointer (physical address since boot stack is in .bss)
    mov rsp, boot_stack_top
    mov rbp, rsp

    ; Push multiboot info pointer as argument (now 64-bit safe)
    mov rdi, [mb_info_ptr]

    ; Call kernel main
    call kernel_main

    ; If kernel_main returns, halt
.halt:
    cli
    hlt
    jmp .halt
