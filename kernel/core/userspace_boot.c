/*
 * Lestra OS - Userspace boot
 * Copyright (c) 2026 lestramk.org / Lee Muriihi Kingori
 *
 * PREVIOUSLY: this file was a no-op stub. The kernel would initialize
 *             all subsystems, then sit in the in-kernel shell/compositor
 *             forever. The compiled user ELFs in the initrd (init,
 *             shell, sysinfo, hello) were never executed.
 *
 * NOW: this file actually loads /init from the initrd-backed VFS and
 *      jumps to it in ring 3 via the ELF loader. The scheduler is then
 *      enabled so timer IRQs preempt the new PID 1.
 *
 *      If the ELF load fails (no initrd, no /init, bad ELF), we fall
 *      back to the in-kernel shell/compositor so the system is still
 *      usable for debugging.
 *
 * DYNAMIC LINKING:
 *      try_exec_init peeks the ELF header. If the binary is dynamically
 *      linked (PT_INTERP or PT_DYNAMIC present), we hand off to the
 *      in-kernel dynamic linker (ldso_load_and_run) which loads shared
 *      libraries from /lib, applies relocations, and jumps to the
 *      entry. Static binaries go through elf_exec directly.
 */

#include <lestra/types.h>
#include <lestra/printk.h>
#include <lestra/vfs.h>
#include <lestra/panic.h>

extern int  elf_exec(const char* path);
extern int  ldso_load_and_run(const char* exe_path, int argc, char** argv, char** envp);
extern int  ldso_is_dynamic(const char* path);
extern void sched_enable(void);

/* Forward decls from kernel_main.c — fall-back paths. */
extern void shell_run(void);
extern void compositor_run(void);

static int try_exec_init(const char* path) {
    int fd = vfs_open(path, 0);
    if (fd < 0) return -1;

    /* Peek the first 4 bytes to confirm it's an ELF before we burn
     * 64 KB of stack on elf_exec's static buffer. */
    uint8_t magic[4];
    ssize_t n = vfs_read(fd, magic, sizeof(magic));
    vfs_close(fd);
    if (n != 4) return -1;
    if (magic[0] != 0x7F || magic[1] != 'E' ||
        magic[2] != 'L'  || magic[3] != 'F') {
        pr_warn("userspace_boot: %s is not an ELF\n", path);
        return -1;
    }

    /* If the binary is dynamically linked, run it via the dynamic
     * linker. ldso_is_dynamic peeks PT_INTERP / PT_DYNAMIC.
     * FIX: If ldso_load_and_run fails (e.g. because the interpreter
     * doesn't exist on the VFS), fall back to elf_exec. This handles
     * the case where /init was accidentally built as a dynamic ELF
     * on the host system (with a PT_INTERP pointing to glibc's
     * ld-linux.so.2) but should really be treated as a static binary
     * since we don't have glibc. elf_exec will still fail for truly
     * dynamic binaries (unresolved symbols), but for binaries that
     * are effectively static (all symbols resolved at link time) it
     * works fine. */
    if (ldso_is_dynamic(path)) {
        pr_info("userspace_boot: %s is dynamic — ldso_load_and_run\n", path);
        int rc = ldso_load_and_run(path, 1, (char*[]){ (char*)path, NULL }, NULL);
        if (rc < 0) {
            pr_warn("userspace_boot: ldso_load_and_run(%s) failed — "
                    "falling back to elf_exec\n", path);
            /* Try elf_exec as fallback. For a binary that has PT_INTERP
             * but is otherwise self-contained (all code in PT_LOAD
             * segments, no unresolved DT_NEEDED), elf_exec can still
             * load and run the segments directly. */
            int rc2 = elf_exec(path);
            if (rc2 < 0) {
                pr_warn("userspace_boot: elf_exec(%s) also failed\n", path);
                return -1;
            }
            return 0;
        }
        return 0;
    }

    pr_info("userspace_boot: %s is static — elf_exec\n", path);
    int rc = elf_exec(path);
    if (rc < 0) {
        pr_warn("userspace_boot: elf_exec(%s) failed\n", path);
        return -1;
    }
    return 0;
}

void userspace_boot(void) {
    pr_info("userspace_boot: attempting PID 1 (/init)\n");

    /* Try the canonical init paths in order. */
    if (try_exec_init("/init")      == 0) goto enabled;
    if (try_exec_init("/bin/init")  == 0) goto enabled;
    if (try_exec_init("/sbin/init") == 0) goto enabled;

    pr_warn("userspace_boot: no /init found — falling back to in-kernel shell/compositor.\n");
    pr_warn("userspace_boot: this means the initrd was not loaded, /init was\n");
    pr_warn("userspace_boot: not built, or the ELF was rejected. The kernel will\n");
    pr_warn("userspace_boot: continue to run, but in single-kernel-context mode.\n");
    return;

enabled:
    /* If elf_exec / ldso_load_and_run returned 0 it actually jumped to
     * ring 3 and the user process is now running. Enable preemption so
     * future fork()s and timer ticks can context-switch between
     * processes. */
    sched_enable();
    pr_info("userspace_boot: scheduler enabled, PID 1 running\n");
}
