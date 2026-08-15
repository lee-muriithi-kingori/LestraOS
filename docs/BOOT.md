# Lestra OS — Boot Fixes & What Was Wrong

This document explains the bugs that prevented LestraOS from booting and how
each one was fixed in this version.

## Quick summary

| # | Bug | Severity | Fix |
|---|-----|----------|-----|
| 1 | Kernel linked at 0x10000 (64KB) instead of 0x100000 (1MB) | Critical | linker.ld: changed base to 0x100000 |
| 2 | VGA driver used 0xFFFFFFFF800B8000 (unmapped higher-half alias) | Critical | vga.c: use identity-mapped 0xB8000 |
| 3 | boot.asm higher-half alias was broken (PDPT[510] never set) | Critical | boot.asm: removed broken alias, identity-map first 1GB instead |
| 4 | kernel_main.c had a shadow `initrd_init` that masked vfs.c's real one | Critical | kernel_main.c: removed stub, declare extern |
| 5 | syscall.c USER_CS was 0x23 (USER_DS\|RPL3) instead of 0x1B | High | syscall.c: USER_CS = 0x1B |
| 6 | vfs_open returned fd 0,1,2 — collided with stdin/stdout/stderr | High | vfs.c: real fds start at 3 |
| 7 | pmm.c `mark_region` used ALIGN_DOWN(end) — missed last partial page | Medium | pmm.c: use ALIGN_UP |
| 8 | Makefile cross-compiler fallback to non-existent `x86_64-lestra-` | Medium | Makefile: fall back to plain gcc |
| 9 | Makefile `ld` didn't specify `-m elf_x86_64` | Low | Makefile: added `-m elf_x86_64` |
| 10 | `build/mkinitrd.py` referenced by Makefile but missing | High | Added build/mkinitrd.py |
| 11 | `build/cross-compiler.sh` referenced by README but missing | Medium | Added build/cross-compiler.sh |
| 12 | Multiboot2 header tag alignment was wrong in some places | Medium | boot.asm: explicit `align 8` per tag |
| 13 | GDT entries missing S bit (0x10) — CPU treated code/data as system segments | Critical | gdt.c: added GDT_ACCESS_S to all code/data descriptors |
| 14 | Kernel heap was at 0xFFFFFFFF90000000 (higher half) — unmapped after fix #3 | Critical | mm.h: moved heap to 0x40000000-0x50000000 (identity-mapped) |
| 15 | vmm.c `get_pte` didn't handle huge pages — would read garbage when walking into a 2MB huge page | Critical | vmm.c: detect huge pages and return the entry itself |
| 16 | vmm.c `vmm_map_page` would dereference huge PD entries as PT pointers, corrupting memory | Critical | vmm.c: refuse to map if a huge page is in the way |
| 17 | vmm.c `vmm_get_phys` didn't compute physical address for huge pages | High | vmm.c: handle 1GB and 2MB huge page address calculation |
| 18 | heap.c didn't reserve its own region in PMM, causing double-allocation | High | heap.c: call `pmm_reserve_region()` at init |
| 19 | vmm.c `vmm_alloc_page` used KERNEL_HEAP_START, conflicting with bump heap | Medium | vmm.c: use a separate 2GB-3GB region |
| 20 | printk format specifiers used %lx/%lu but printk only supports %x/%u | Medium | Fixed all callers to use %x/%u with casts |

## Detailed explanations

### Bug 1: Kernel load address

**Before:** `linker.ld` had `. = 0x10000;` — placing the kernel at physical
64KB. That region overlaps with BIOS data, the real-mode IVT, and boot
loader scratch space. GRUB/multiboot2 might refuse to load or the kernel
might be overwritten during BIOS calls.

**After:** `. = 0x100000;` — the standard 1MB physical address. This is the
conventional load address for multiboot2 kernels and is well above any
BIOS-occupied memory.

### Bug 2: VGA memory address

**Before:** `vga.c` declared `vga_buffer = (volatile uint16_t*)0xFFFFFFFF800B8000;`
This is the higher-half alias of physical 0xB8000. But `boot.asm` only set
up identity mapping for the first 8MB — there was no mapping at virtual
address 0xFFFFFFFF800B8000. Any write to VGA would page-fault and panic
the kernel before it could print anything.

**After:** `vga_buffer = (volatile uint16_t*)0xB8000;` — the identity-mapped
physical address. Works because boot.asm identity-maps the first 1GB.

### Bug 3: Higher-half mapping was broken

**Before:** boot.asm tried to set up a higher-half alias at
0xFFFFFFFF80000000 by mapping both PML4[0] and PML4[511] to the same PDPT.
But the math:

- 0xFFFFFFFF80000000 has PML4 index = 511 ✓
- 0xFFFFFFFF80000000 has PDPT index = 510 (NOT 0!)

So even though PML4[511] pointed to the PDPT, the PDPT only had PDPT[0]
initialized. PDPT[510] was zero (not present), so accessing
0xFFFFFFFF80000000 page-faulted.

**After:** Removed the higher-half alias entirely. The kernel now runs
identity-mapped in the first 1GB of physical memory. This is simpler and
sufficient for a single-task kernel. The boot.asm now maps 512 huge 2MB
pages = 1GB of identity-mapped memory.

### Bug 4: Shadow initrd_init

**Before:** `kernel_main.c` declared:
```c
static void initrd_init(void* base, size_t size) {
    (void)base; (void)size;
    pr_debug("initrd_init stub: no initrd support linked in yet\n");
}
```

This `static` function shadowed the real `initrd_init` in `fs/vfs.c`, so
even when GRUB loaded an initrd module, the kernel's parse code would
just call the no-op stub. The VFS never received any files from the
initrd, leaving the filesystem empty.

**After:** Removed the stub. `kernel_main.c` now declares
`extern void initrd_init(void* addr, uint32_t size);` and calls it when
the multiboot2 module tag is found. The real implementation in `vfs.c`
parses the initrd format and loads files into the in-memory VFS.

### Bug 5: Wrong USER_CS in syscall.c

**Before:** `#define USER_CS 0x23` — but 0x23 is `USER_DS(0x20) | RPL3`.
The correct user code selector is `USER_CS(0x18) | RPL3 = 0x1B`.

This bug would cause SYSRET to load a data segment into CS, immediately
triggering a #GP fault the first time any userspace program tried to
return from a syscall.

**After:** `#define USER_CS 0x1B` and added `#define USER_DS 0x23` for
clarity. Also added a comment explaining the calculation.

### Bug 6: VFS file descriptor collision

**Before:** `vfs_open` returned the file index directly (0, 1, 2, 3, ...).
But fds 0, 1, 2 are reserved by POSIX for stdin/stdout/stderr. The first
file created would get fd 0 — when the user code did `read(0, ...)`, it
would read from the file, not stdin.

**After:** All VFS functions now offset by 3: real fds start at 3, and
the index is computed as `fd - 3` internally.

### Bug 7: pmm mark_region missed last page

**Before:**
```c
size_t end_pfn = addr_to_pfn(ALIGN_DOWN(end, PAGE_SIZE));
for (pfn = start_pfn; pfn < end_pfn; pfn++) { ... }
```

If `end = 0x10200` (just past a page boundary), `ALIGN_DOWN(0x10200)` =
`0x10000`, so `end_pfn = 16`. The loop runs `pfn < 16`, which excludes
page 16, even though page 16 (0x10000-0x10FFF) overlaps with the region.

**After:** Use `ALIGN_UP(end, PAGE_SIZE)` so any page that contains even
one byte of the region is marked as used.

### Bug 8: Makefile cross-compiler detection

**Before:**
```makefile
CROSS_PREFIX := $(shell which x86_64-elf-gcc >/dev/null 2>&1 && echo x86_64-elf- || echo x86_64-lestra-)
```

The fallback `x86_64-lestra-` doesn't exist anywhere — so on a system
without `x86_64-elf-gcc`, the Makefile would try to use `x86_64-lestra-gcc`
and fail. There was no actual fallback to the system `gcc`.

**After:** Fall back to empty prefix (use plain `gcc`, `ld`, etc.). This
works on Linux systems with the standard toolchain installed.

### Bug 9: ld emulation not specified

**Before:** `LDFLAGS := -nostdlib --no-dynamic-linker -z max-page-size=0x1000`

Some `ld` versions can't auto-detect the target architecture from the
input files (especially when only assembly objects are present). This
causes "cannot use non-ELF file" or wrong-emulation errors.

**After:** Added `-m elf_x86_64` to LDFLAGS to force x86_64 emulation.

### Bug 10-11: Missing build scripts

The Makefile referenced `build/mkinitrd.py` and the README referenced
`build/cross-compiler.sh`, but neither file existed in the source tree.
Both have now been added:

- `build/mkinitrd.py` — builds a simple initrd image from a list of files
- `build/cross-compiler.sh` — downloads + builds binutils + gcc as an
  x86_64-elf cross-compiler

## How to verify the fixes

After building and booting the ISO, you should see:

1. GRUB menu with "Lestra OS" entry
2. VGA shows "B" in the top-left (from boot.asm)
3. Serial output (if `-serial stdio`) shows: `B123456L`
4. Kernel boot log appears:
   ```
   Lestra OS kernel initializing...
   Initializing GDT...
   Initializing IDT...
   Initializing PIC...
   Initializing memory management...
   PMM: XXXX MB total, YYYY MB usable
   VMM initialized
   ...
   ```
5. Boot splash with ASCII art logo appears
6. Press a key within 5 seconds → UI menu, otherwise shell

If you see a kernel panic instead, the most likely remaining issue is the
cross-compiler version. Try `make clean && make DEBUG=1 OPTIMIZE=0` for
verbose output.

## What still doesn't work

These are known limitations that are NOT bugs but missing features on the
roadmap:

- **No real multitasking** — scheduler is a single-task stub
- **No real filesystem** — VFS is in-memory only (no ext2/fat32 yet)
- **No network stack** — AI chat returns simulated responses without
  a TCP/IP stack. See `docs/NETWORK.md` (planned) for the roadmap.
- **No userspace** — syscalls are stubbed, userland binaries can't
  actually run yet (the kernel runs the shell in ring 0)

These are documented in the README's roadmap section.

### Bug 13: GDT missing S bit (system vs code/data)

**Before:** gdt.c constructed access bytes for code/data segments using:
```c
GDT_ACCESS_PRESENT | GDT_ACCESS_RING0 | GDT_ACCESS_EXECUTABLE | GDT_ACCESS_RW
```
This evaluates to 0x8A. But bit 4 (S, "system vs code/data") was NOT set,
so the CPU interpreted these as system segments (TSS/gate). The long-mode
far jump in `gdt_flush` would #GP.

**After:** Added `GDT_ACCESS_S` (0x10) to all code/data descriptors. Now
kernel code = 0x9A, kernel data = 0x92, user code = 0xFA, user data = 0xF2.
The CPU correctly recognizes them as code/data segments.

### Bug 14: Heap was in unmapped higher half

**Before:** `KERNEL_HEAP_START = 0xFFFFFFFF90000000` — in the higher half.
After fixing bug #3 (removing the broken higher-half alias), this region
is no longer mapped. Any `kmalloc` call would page-fault.

**After:** Moved heap to `0x40000000 - 0x50000000` (256MB region in the
first 1GB, identity-mapped by boot.asm's huge pages). The kernel itself
is at 0x100000-0x200000, so there's no conflict.

### Bug 15: vmm.c get_pte didn't handle huge pages

**Before:** `get_pte()` walked PML4 → PDPT → PD → PT assuming 4KB pages
throughout. But boot.asm uses 2MB huge pages for the first 1GB. When
`get_pte()` reached a PD entry with the HUGE bit set, it would
dereference that entry as a PT pointer, reading garbage memory.

**After:** `get_pte()` now detects the HUGE bit at both PDPT and PD
levels and returns the entry itself (callers can check `*pte & PAGE_HUGE`).

### Bug 16: vmm_map_page corrupted memory on huge page conflicts

**Before:** Same issue as #15 but in `vmm_map_page`. When mapping any
address in the first 1GB, the function would treat the huge PD entry
as a PT pointer and write to the physical address it points to (e.g.,
0x40000000), corrupting whatever was there.

**After:** `vmm_map_page` now checks for the HUGE bit at PDPT and PD
levels and refuses to map if a huge page is in the way. Callers must
use addresses outside the first 1GB for new 4KB mappings.

### Bug 17: vmm_get_phys returned wrong address for huge pages

**Before:** `vmm_get_phys()` did `(*pte & ~0xFFF) | (virt & 0xFFF)`.
For a 2MB huge page, this would mask off only the low 12 bits of the
entry, but the entry actually contains bits 51:21 of the physical
address (the low 21 bits come from the virtual address). The result
was a wrong physical address.

**After:** `vmm_get_phys()` now handles 1GB and 2MB huge pages
separately, masking the appropriate number of bits.

### Bug 18: Heap didn't reserve its region in PMM

**Before:** `heap_init()` just set `heap_next = KERNEL_HEAP_START` and
started bump-allocating. But PMM had no idea those pages were "owned"
by the heap, so `pmm_alloc_page()` could return a page inside the heap
region for use as a page table or other kernel structure. That page
would then be overwritten by `kmalloc` — silent corruption.

**After:** `heap_init()` calls `pmm_reserve_region(KERNEL_HEAP_START,
KERNEL_HEAP_END)` to mark those pages as used in the PMM bitmap.

### Bug 19: vmm_alloc_page used KERNEL_HEAP_START

**Before:** `vmm_alloc_page()` (separate from kmalloc) used
`KERNEL_HEAP_START` as its virtual address cursor. This collided with
the bump heap — calling `vmm_alloc_page()` would return an address
inside the heap region, and the heap would later overwrite the page
table entry that `vmm_alloc_page` had installed.

**After:** Moved `vmm_alloc_page`'s cursor to `0x80000000 - 0xC0000000`
(2GB-3GB), which is outside both the identity-mapped first 1GB and
the heap region. This gives 1GB of space for 4KB-mapped virtual pages.

### Bug 20: printk format string mismatch

**Before:** Multiple files used `%lx`, `%lu`, `%lld` format specifiers,
but the kernel's `printk` only supports `%x`, `%u`, `%d` (with 32-bit
widths via va_arg promotion). On x86_64, `uint64_t` values would be
truncated or interpreted as 32-bit, producing garbage output.

**After:** All callers now cast to `(unsigned)` or `(int)` and use
`%x`/`%u`/`%d`. For full 64-bit values, callers can use `%x` twice
(high 32 bits, low 32 bits) or extend printk to support `%lx`.
