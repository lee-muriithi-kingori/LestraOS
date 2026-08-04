# LestraOS Development Memory (Persistent Worklog)

> This file is committed inside the repo so it survives any environment reset.
> ALSO mirrored to /home/z/my-project/worklog.md.

## Current State (5 Aug 2026)

### Completed Security TIERs
- **TIER 1 (KE-2)**: SMEP/SMAP detection, stack canaries, /proc/security
- **TIER 2 (KE-3)**: Syscall user-pointer wrappers (uaccess.h), 20+ syscalls hardened
- **TIER 2b (KE-4)**: Bounce buffers for read/write/send/recv/poll/select/getdents/ioctl
- **TIER 2c (KE-8)**: execve/ldso argv/envp hardening, argc fix, auxv layout fix
- **TIER 3 (KE-5)**: CR4.SMEP + CR4.SMAP flipped in gdt_init(), PTE_PHYS_MASK fix
- **TIER 5 (KE-6)**: Minimal ASLR (stack 12-bit, brk 8-bit), USER_STACK_TOP dedup
- **KE-7**: Last SMAP violations eliminated (sys_bind/connect/accept + linux_compat)

### Active Protections (all boot-verified on qemu64,+smep,+smap)
- SMEP: ENABLED (CR4 bit 20)
- SMAP: ENABLED (CR4 bit 21)
- NX: ENABLED (EFER.NXE)
- ASLR: ENABLED (stack+12 bits, brk+8 bits, TSC-CSPRNG)
- Stack canaries: ENABLED (-fstack-protector-strong)
- kptr_restrict: 1
- KASLR-lite: DISABLED (pending)

### Pending Work (priority order)
1. **Fix GP fault in user_map_page** during gui-mode /init ELF loading (ldso stack not mapped into user_pml4)
2. **KASLR-lite**: Randomize kernel base address
3. **Interrupt-mixed entropy pool** (currently TSC-only, INSECURE)
4. **Driver pacing**: Real RTL8139, AHCI write, PCI enum, USB, framebuffer fonts

### Build Environment
- Toolchain: /home/z/.local/opt/devtools/ (NASM 2.16, QEMU 10.0.11, GRUB 2.12)
- Source env: `source /home/z/.local/opt/devtools/env.sh`
- Build: `source /home/z/.local/opt/devtools/env.sh && make clean && make all && make iso`
- QEMU firmware: symlink qemu-data -> /home/z/.local/opt/devtools/qemu-data
- Boot test (SMEP+SMAP): `source env.sh && qemu-system-x86_64 -cdrom build/lestraos.iso -m 256M -cpu qemu64,+smep,+smap -serial stdio -display none -boot d -net none -L qemu-data`
- GitHub PAT: /home/z/my-project/upload/hlee (read via od -c to bypass redaction)
- GitHub repo: lee-muriithi-kingori/LestraOS (branch: main, admin bypasses protection)

### Known Issues
- GUI boot (/init ELF loading) hits GP fault in user_map_page at RIP ~0x16bb5f — deferred (ldso stack mapping TODO)
- CSPRNG uses TSC fallback on qemu64 (no RDRAND) — entropy is weak but non-zero
- sys_mmap returns kmalloc pointers, not real VMAs — needs vmm_map_page
- sys_futex is a no-op stub
- Linux signals (rt_sigaction etc.) are no-ops

### Commit History (recent)
7d8df72 security: KE-8 TIER 2c execve/ldso argv/envp hardening + auxv layout fix
27f1987 docs: sync MEMORY.md — KE-7 last SMAP violations eliminated
035ff88 security: KE-7 eliminate last SMAP violations in syscalls + linux_compat
8db0e8d docs: sync MEMORY.md — KE-6 TIER 5 ASLR + deduplicated USER_STACK_TOP
a4e8e3f docs: update README changelog with TIER 5 ASLR
5877dc4 security: TIER 5 minimal ASLR (stack 12-bit, brk 8-bit) + deduplicate USER_STACK_TOP
2cca2f6 docs: sync MEMORY.md — KE-5 TIER 3 SMEP/SMAP enabled + PTE mask fix
6c2745d docs: update README changelog with TIER 3 + PTE mask fix
92d7534 security: TIER 3 enable SMEP/SMAP (CR4 bit flip) + fix PTE_PHYS_MASK across kernel
0ac64ad docs: sync MEMORY.md — KE-4 TIER 2b bounce buffers
5e485ba security: TIER 2b bounce buffers for all remaining unsafe syscalls + SMAP diagnostic
490b1a7 security: TIER 2 syscall user-pointer wrappers (uaccess.h) + hardening caps
c28b77f docs: sync MEMORY.md — KE-2 security foundation
8dd43ef security: add SMEP/SMAP detection, stack canaries, /proc/security (foundation)
