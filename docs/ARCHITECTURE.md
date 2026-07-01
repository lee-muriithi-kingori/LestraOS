# Lestra OS Architecture

## Overview

Lestra OS is a monolithic kernel operating system for x86_64 architecture, designed to be lightweight and efficient on systems with 4GB of RAM.

## Kernel Architecture

```
+──────────────────────────────────────────────────────+
|                   User Space                          |
|  +─────────+ +──────────+ +───────+ +──────────+   |
|  |  Shell  | |   Apps   | |  Desktop │ | Package │   |
|  +────┬────+ +────┬─────+ └───┬───┘ +────┬─────┘   |
+───────┼───────────┼──────────┼──────────┼──────────+
|       │           │          │          │           |
|  +────┴───────────┴──────────┴──────────┴─────┐     |
|  |           System Call Interface             │     |
|  +────┬───────────────────────────────────┬────┘     |
+───────┼───────────────────────────────────┼──────────+
|  +────┴────+ +─────────+ +───────+ +────┴────┐     |
|  |   VFS   │ │ Scheduler│ │  MM   │ │  Drivers│     |
|  +────┬────+ +────┬────+ └───┬───┘ +────┬────┘     |
|  +────┴────+ +────┴────+ +───┴───┐ +────┴────┐     |
|  │  initrd │ │ Process │ │Paging │ │ Keyboard│     |
|  │  ext4   │ │  Table  │ │ Heap  │ │  VGA    │     |
|  │         │ │  Timer  │ │Bitmap │ │  Serial │     |
|  +─────────+ +─────────+ +───────┘ +─────────┘     |
+──────────────────────────────────────────────────────+
|         Architecture Layer (x86_64)                  |
|  GDT │ IDT │ ISR │ IRQ │ Paging │ Boot              |
+──────────────────────────────────────────────────────+
|              Hardware (x86_64 PC)                     |
+──────────────────────────────────────────────────────+
```

## Memory Layout

```
0xFFFFFFFFFFFFFFFF  ─┬─  Top of virtual address space
                     │
0xFFFFFFFFC0000000   │   Recursive page table mapping
                     │
0xFFFFFFFFA0000000   │   Kernel heap end
                     │
0xFFFFFFFF90000000   │   Kernel heap start
                     │
0xFFFFFFFF80000000  ─┼─  Kernel base (higher half)
                     │   .text, .rodata, .data, .bss
                     │
                     │   (Unmapped gap)
                     │
0x00007FFFFFFFFFFF  ─┼─   User space top
                     │    Stack (grows down)
                     │    Heap (grows up)
                     │    .bss, .data, .text
                     │
0x0000000000400000  ─┼─   User program base
                     │
0x0000000000100000  ─┼─   Kernel physical load address (1MB)
                     │
0x0000000000000000  ─┴─   NULL (protected)
```

## Boot Process

1. **BIOS/UEFI** → Loads GRUB2 bootloader
2. **GRUB2** → Loads kernel.bin (multiboot2) and initrd.img
3. **boot.asm** → Sets up page tables, enters long mode (x86_64)
4. **kernel_main()** → Initializes all subsystems:
   - GDT (Global Descriptor Table)
   - IDT (Interrupt Descriptor Table)
   - PIC (Programmable Interrupt Controller)
   - PMM (Physical Memory Manager)
   - VMM (Virtual Memory Manager)
   - Heap allocator
   - Timer (PIT 1000Hz)
   - Keyboard driver
5. **shell_run()** → Starts interactive shell

## File System

Lestra OS uses a Virtual File System (VFS) with pluggable backends:
- **initrd** - Initial ramdisk for boot files
- **ext4** - Standard Linux filesystem (planned)
- **procfs** - Process information (planned)
- **devfs** - Device files (planned)

## Process Management

Round-robin scheduler with:
- Preemptive multitasking (timer-based)
- 256 max concurrent tasks
- Priority levels (0-31)
- Sleep/block support
- Context switching via assembly

## System Calls

| Number | Name | Description |
|--------|------|-------------|
| 0 | exit | Terminate process |
| 2 | read | Read from file descriptor |
| 3 | write | Write to file descriptor |
| 4 | open | Open file |
| 5 | close | Close file |
| 8 | getpid | Get process ID |
| 9 | brk | Manage data segment |
| 13 | sleep | Sleep for milliseconds |
| 21 | reboot | Reboot or shutdown |
| 22 | uname | Get system information |

## License

MIT License - See LICENSE file for details.
