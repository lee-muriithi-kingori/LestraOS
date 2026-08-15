#!/usr/bin/env python3
"""Fix SMAP violations in linux_compat.c - replace the dispatch function body."""

path = '/home/z/lestraOS/kernel/exec/linux_compat.c'
with open(path, 'r') as f:
    content = f.read()

# Find the start of the function
func_start = 'int64_t linux_compat_dispatch(uint64_t linux_num,'
start_idx = content.index(func_start)

# Find the matching closing brace for the function
# The function ends with '}' followed by a blank line and '/* Execute a Linux ELF'
end_marker = '\n\n/* Execute a Linux ELF'
end_idx = content.index(end_marker, start_idx)

# New function body
new_func = '''int64_t linux_compat_dispatch(uint64_t linux_num,
                               uint64_t a1, uint64_t a2, uint64_t a3,
                               uint64_t a4, uint64_t a5, uint64_t a6) {
    switch (linux_num) {
        case LINUX_SYS_READ:
            /* Linux read(fd, buf, count) = LestraOS read(fd, buf, count). */
            return (int64_t)lestra_syscall6(LESTRA_SYS_READ, a1, a2, a3, 0, 0, 0);

        case LINUX_SYS_WRITE:
            return (int64_t)lestra_syscall6(LESTRA_SYS_WRITE, a1, a2, a3, 0, 0, 0);

        case LINUX_SYS_OPEN: {
            /* Linux open(path, flags, mode). Flags are different from
             * LestraOS but the low bits (O_RDONLY=0, O_WRONLY=1, O_RDWR=2)
             * are the same. We strip high bits we don't understand. */
            int lestra_flags = (int)(a2 & 0x3);  /* access mode only */
            if (a2 & 0x40) lestra_flags |= 0x0010;  /* O_CREAT */
            if (a2 & 0x200) lestra_flags |= 0x0020; /* O_TRUNC */
            if (a2 & 0x400) lestra_flags |= 0x0040; /* O_APPEND */
            return (int64_t)lestra_syscall6(LESTRA_SYS_OPEN, a1, (uint64_t)lestra_flags, 0, 0, 0, 0);
        }

        case LINUX_SYS_CLOSE:
            return (int64_t)lestra_syscall6(LESTRA_SYS_CLOSE, a1, 0, 0, 0, 0, 0);

        case LINUX_SYS_STAT:
            return (int64_t)lestra_syscall6(LESTRA_SYS_STAT, a1, a2, 0, 0, 0, 0);

        case LINUX_SYS_FSTAT: {
            /* LestraOS doesn't have fstat yet - fake success with a
             * zeroed stat struct so the binary doesn't crash.
             * SMAP-safe: use clear_user instead of direct memset. */
            if (!a2) return -LINUX_EFAULT;
            if (!access_ok((void*)a2, 144)) return -LINUX_EFAULT;
            if (clear_user((void*)a2, 144) < 0) return -LINUX_EFAULT;
            return 0;
        }

        case LINUX_SYS_LSEEK:
            return (int64_t)lestra_syscall6(LESTRA_SYS_LSEEK, a1, a2, a3, 0, 0, 0);

        case LINUX_SYS_MMAP: {
            /* Linux mmap(addr, len, prot, flags, fd, offset). LestraOS
             * only supports anonymous mmap; for file-backed we'd need
             * a page cache. */
            int lestra_flags = (int)a4;
            if (!(lestra_flags & 0x20 /* MAP_ANONYMOUS */)) {
                return -LINUX_ENOSYS;
            }
            return (int64_t)lestra_syscall6(LESTRA_SYS_MMAP, a1, a2, a3, (uint64_t)lestra_flags, a5, a6);
        }

        case LINUX_SYS_MUNMAP:
            return (int64_t)lestra_syscall6(LESTRA_SYS_MUNMAP, a1, a2, 0, 0, 0, 0);

        case LINUX_SYS_BRK:
            return (int64_t)lestra_syscall6(LESTRA_SYS_BRK, a1, 0, 0, 0, 0, 0);

        case LINUX_SYS_IOCTL:
            /* Most ioctls are no-ops for our purposes; pretend success. */
            return 0;

        case LINUX_SYS_ACCESS:
            /* Pretend everything is accessible. */
            return 0;

        case LINUX_SYS_DUP:
            /* We don't have dup; fake it by returning the same fd. */
            return (int64_t)a1;

        case LINUX_SYS_DUP2:
            /* Same as dup but to a specific fd. */
            return (int64_t)a2;

        case LINUX_SYS_GETPID:
            return (int64_t)lestra_syscall6(LESTRA_SYS_GETPID, 0, 0, 0, 0, 0, 0);

        case LINUX_SYS_GETPPID:
            /* LestraOS doesn't expose getppid yet. Return 1 (init). */
            return 1;

        case LINUX_SYS_GETUID:
        case LINUX_SYS_GETGID:
        case LINUX_SYS_GETEUID:
        case LINUX_SYS_GETEGID:
            /* No users; return 0 (root). */
            return 0;

        case LINUX_SYS_GETCWD:
            return (int64_t)lestra_syscall6(LESTRA_SYS_GETCWD, a1, a2, 0, 0, 0, 0);

        case LINUX_SYS_CHDIR:
            return (int64_t)lestra_syscall6(LESTRA_SYS_CHDIR, a1, 0, 0, 0, 0, 0);

        case LINUX_SYS_MKDIR:
            return (int64_t)lestra_syscall6(LESTRA_SYS_MKDIR, a1, a2, 0, 0, 0, 0);

        case LINUX_SYS_RMDIR:
            return (int64_t)lestra_syscall6(LESTRA_SYS_RMDIR, a1, 0, 0, 0, 0, 0);

        case LINUX_SYS_UNAME: {
            /* Linux struct utsname is 6x65-byte strings.
             * SMAP-safe: build in kernel buffer, copy_to_user. */
            if (!a1) return -LINUX_EFAULT;
            if (!access_ok((void*)a1, 6 * 65)) return -LINUX_EFAULT;
            char kbuf[6 * 65];
            memset(kbuf, 0, sizeof(kbuf));
            memcpy(kbuf,       "Linux",   5);  /* sysname */
            memcpy(kbuf + 65,  "lestra",  6);  /* nodename */
            memcpy(kbuf + 130, "5.15.0",  6);  /* release */
            memcpy(kbuf + 195, "#1",      2);  /* version */
            memcpy(kbuf + 260, "x86_64",  6);  /* machine */
            memcpy(kbuf + 325, "(none)",  6);  /* domainname */
            if (copy_to_user((void*)a1, kbuf, sizeof(kbuf)) < 0) return -LINUX_EFAULT;
            return 0;
        }

        case LINUX_SYS_GETTIMEOFDAY: {
            /* Linux struct timeval = { tv_sec, tv_usec } (8+8 bytes).
             * SMAP-safe: build in kernel buffer, copy_to_user. */
            int64_t ms = (int64_t)lestra_syscall6(LESTRA_SYS_GETTIMEOFDAY, 0, 0, 0, 0, 0, 0);
            if (a1) {
                if (!access_ok((void*)a1, 16)) return -LINUX_EFAULT;
                int64_t tv[2];
                tv[0] = ms / 1000;
                tv[1] = (ms % 1000) * 1000;
                if (copy_to_user((void*)a1, tv, 16) < 0) return -LINUX_EFAULT;
            }
            return 0;
        }

        case LINUX_SYS_NANOSLEEP: {
            /* Linux: nanosleep(req, rem). req = { seconds, nanoseconds }.
             * SMAP-safe: copy_from_user the request struct. */
            if (!a1) return -LINUX_EFAULT;
            if (!access_ok((void*)a1, 16)) return -LINUX_EFAULT;
            int64_t req[2];
            if (copy_from_user(req, (void*)a1, 16) < 0) return -LINUX_EFAULT;
            int64_t ms = req[0] * 1000 + req[1] / 1000000;
            return (int64_t)lestra_syscall6(LESTRA_SYS_SLEEP, (uint64_t)ms, 0, 0, 0, 0, 0);
        }

        case LINUX_SYS_EXIT:
            lestra_syscall6(LESTRA_SYS_EXIT, a1, 0, 0, 0, 0, 0);
            return 0;  /* never reached */

        case LINUX_SYS_EXECVE:
            return (int64_t)lestra_syscall6(LESTRA_SYS_EXECVE, a1, a2, a3, 0, 0, 0);

        case LINUX_SYS_RT_SIGACTION:
        case LINUX_SYS_RT_SIGPROCMASK:
        case LINUX_SYS_RT_SIGRETURN:
            /* Signals not implemented. Pretend success so binaries that
             * install signal handlers during startup don't crash. */
            return 0;

        case LINUX_SYS_CLONE:
        case LINUX_SYS_FORK:
        case LINUX_SYS_VFORK:
            /* No threads/fork yet. */
            return -LINUX_ENOSYS;

        case LINUX_SYS_WAIT4:
            return (int64_t)lestra_syscall6(LESTRA_SYS_WAITPID, a1, a2, a3, 0, 0, 0);

        case LINUX_SYS_KILL:
            /* No signals. */
            return -LINUX_ENOSYS;

        case LINUX_SYS_FCNTL:
            /* Most fcntl cmds are no-ops for us. */
            return 0;

        case LINUX_SYS_SOCKET:
        case LINUX_SYS_CONNECT:
        case LINUX_SYS_ACCEPT:
        case LINUX_SYS_SENDTO:
        case LINUX_SYS_RECVFROM:
        case LINUX_SYS_BIND:
        case LINUX_SYS_LISTEN:
            /* Socket syscalls - return -ENOSYS so binaries fall back
             * to other I/O. A future port could translate to LestraOS
             * net_connect/net_send/net_recv. */
            return -LINUX_ENOSYS;

        case LINUX_SYS_SYSINFO: {
            /* Linux struct sysinfo is 112 bytes.
             * SMAP-safe: build in kernel buffer, copy_to_user. */
            if (!a1) return -LINUX_EFAULT;
            if (!access_ok((void*)a1, 112)) return -LINUX_EFAULT;
            uint8_t kbuf[112];
            memset(kbuf, 0, 112);
            *(int64_t*)(kbuf + 0)  = 4 * 1024 * 1024;  /* totalram (4 GB) */
            *(int64_t*)(kbuf + 8)  = 1 * 1024 * 1024;  /* freeram (1 GB) */
            *(int64_t*)(kbuf + 16) = 0;                /* sharedram */
            *(int64_t*)(kbuf + 24) = 0;                /* bufferram */
            *(int32_t*)(kbuf + 40) = 100;              /* procs */
            if (copy_to_user((void*)a1, kbuf, 112) < 0) return -LINUX_EFAULT;
            return 0;
        }

        default:
            pr_info("linux_compat: unhandled syscall %u\\n", (unsigned)linux_num);
            return -LINUX_ENOSYS;
    }
}
'''

# Replace from func_start to end_idx
content = content[:start_idx] + new_func + content[end_idx:]

with open(path, 'w') as f:
    f.write(content)

print('Done! linux_compat.c fully rewritten with SMAP-safe dispatch function.')
