; LestraOS - Stage1 (final, fixed DL=0x80, two-half EDD read)
BITS 16
ORG 0x7C00

jmp short entry

print16:
    push ax
    push si
.next:
    lodsb
    test al, al
    jz .done
    mov ah, 0x0E
    int 0x10
    jmp .next
.done:
    pop si
    pop ax
    ret

entry:
    cld
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00

    mov si, msg_loading
    call print16

    ; === Read 1: 127 sectors @ LBA 1 -> 0x10000 ===
    xor bx, bx
    mov ax, 0x1000
    mov es, ax
    mov ah, 0x42
    mov dl, 0x80                ; <-- explicit drive: first HDD
    mov si, dap
    push ax
    int 0x13
    jc disk_error
    pop ax

    mov si, msg_r1
    call print16

    ; visual marker 'O' = ok
    mov word [0xB8000], 0x0F4F

    ; === Read 2: 19 sectors @ LBA 128 -> 0x1FC00 ===
    xor bx, bx
    mov ax, 0x1FC0
    mov es, ax
    mov ah, 0x42
    mov dl, 0x80
    mov si, dap2
    push ax
    int 0x13
    jc disk_error
    pop ax

    mov si, msg_r2
    call print16

    ; visual marker 'K' = read2 ok
    mov word [0xB8002], 0x0F4B

    ; === Switch to 32-bit protected mode ===
    cli
    lgdt [gdt_ptr]
    mov eax, cr0
    or eax, 1
    mov cr0, eax
    mov word [0xB8004], 0x0F50    ; 'P' = pm on
    jmp 0x0008:pm32

disk_error:
    ; print ERR AH=XX (using AH we saved on stack)
    mov si, msg_e
    call print16
    mov al, ah
    call print_hex_byte
    cli
    hlt

print_hex_byte:
    push ax
    push cx
    mov cl, 4
    shr al, cl
    and al, 0x0F
    add al, 0x30
    cmp al, 0x3A
    jb .p1
    add al, 7
.p1:
    mov ah, 0x0E
    int 0x10
    pop ax
    and al, 0x0F
    add al, 0x30
    cmp al, 0x3A
    jb .p2
    add al, 7
.p2:
    mov ah, 0x0E
    int 0x10
    pop cx
    ret

BITS 32
pm32:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, 0x80000
    mov word [0xB8006], 0x0F4B    ; 'K' = entered kernel

    ; Jump to kernel entry _start.
    ; Stage1 loads kernel from disk LBA 1 (file offset 0x200) into memory
    ; 0x10000. The linker script puts .multiboot2 first (padded to a 4 KB
    ; boundary by ALIGN(4096) on .boot), so .text starts at file offset
    ; 0x1000. That means _start lands in memory at
    ;   0x10000 + (0x1000 - 0x200) = 0x10E00.
    ;
    ; FIX: previous code computed 0x11E00 by assuming .text is at file
    ; offset 0x2000, but the multiboot2 header itself is only ~80 bytes
    ; and gets padded to 0x1000 — not 0x2000. Jumping to 0x11E00 would
    ; land 4 KB too high, in the middle of .rodata or .data, and crash.
    ;
    ; NOTE: this raw-disk boot path is NOT used when booting from the
    ; GRUB ISO (make iso + make run). GRUB loads kernel.bin as a
    ; multiboot2 kernel and jumps directly to _start, ignoring stage1.
    ; stage1 is only needed if you dd the image directly to a USB/HDD.
    mov eax, 0x10E00
    jmp eax

gdt:
    dq 0
    dq 0x00CF9A000000FFFF
    dq 0x00CF92000000FFFF
gdt_ptr:
    dw 23
    dd gdt

dap:
    db 16
    db 0
    dw 127                   ; 127 sectors
    dw 0x0000
    dw 0x1000                ; linear 0x10000
    dq 1                     ; LBA 1

dap2:
    db 16
    db 0
    dw 19                    ; 19 sectors
    dw 0x0000
    dw 0x1FC0                ; linear 0x1FC00
    dq 128                   ; LBA 128

msg_loading:  db 'Loading kernel...', 13, 10, 0
msg_r1:       db 'R1 ok ', 0
msg_r2:       db 'R2 ok ', 0
msg_e:        db 'ERR AH=', 0

times 510 - ($ - $$) db 0
dw 0xAA55