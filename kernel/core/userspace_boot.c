/*
 * Lestra OS - Userspace boot
 * Copyright (c) 2026 lestramk.org / Lee Muriithi Kingori
 * Loads /init from initrd-backed VFS and jumps to ring 3 via ELF loader
 */

#include <lestra/types.h>
#include <lestra/printk.h>
#include <lestra/vfs.h>
#include <lestra/panic.h>

extern int  elf_exec(const char* path);
extern int  ldso_load_and_run(const char* exe_path, int argc, char** argv, char** envp);
extern int  ldso_is_dynamic(const char* path);
extern void sched_enable(void);

extern void sched_start_first(const char* name, const void* elf_data, size_t elf_size);

/* Read an entire VFS file into a caller-provided buffer. */
static int read_file_into(const char* path, uint8_t* buf, size_t bufsize) {
    int fd = vfs_open(path, 0);
    if (fd < 0) return -1;
    size_t total = 0;
    while (total < bufsize) {
        ssize_t n = vfs_read(fd, buf + total, bufsize - total);
        if (n <= 0) break;
        total += (size_t)n;
    }
    vfs_close(fd);
    return (int)total;
}

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

    pr_info("userspace_boot: %s is static — sched_start_first\n", path);

    /* KE-25: Read the full ELF into a buffer and hand it to the
     * scheduler's first-process launcher. sched_start_first creates a
     * proper struct process (PID 1) with an fd table, sets `current`,
     * and jumps to userspace — so syscalls like write() that need a
     * current process actually work. It never returns. */
    static uint8_t elf_buf[65536];
    int sz = read_file_into(path, elf_buf, sizeof(elf_buf));
    if (sz <= 0) {
        pr_warn("userspace_boot: failed to read %s\n", path);
        return -1;
    }
    sched_start_first("init", elf_buf, (size_t)sz);

    /* Should never reach here — sched_start_first jumps to ring 3. */
    pr_warn("userspace_boot: sched_start_first returned (impossible)\n");
    return -1;
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
