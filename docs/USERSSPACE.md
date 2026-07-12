# LestraOS Userspace Architecture

This document describes the syscall ABI, the user-side ABI, and how to
add a new userspace binary. It is the authoritative reference for
userland developers.

## Syscall ABI

LestraOS uses the SYSCALL/SYSRET instruction pair on x86_64. The
calling convention differs from C: the syscall number goes in `rax`,
arguments in `rdi, rsi, rdx, r10, r8, r9`, and the return value is in
`rax`. The `rcx` and `r11` registers are clobbered.

| Register | Role          |
|----------|---------------|
| rax      | syscall num / return |
| rdi      | arg 1         |
| rsi      | arg 2         |
| rdx      | arg 3         |
| r10      | arg 4 (note: NOT rcx — syscall clobbers rcx) |
| r8       | arg 5         |
| r9       | arg 6         |

Negative return values are `-errno`. Non-negative values are success.

## Syscall Numbers

These are stable — never renumber. New ones get appended.

```
 0  read(fd, buf, count)
 1  write(fd, buf, count)
 2  open(path, flags, mode)
 3  close(fd)
 4  lseek(fd, off, whence)
 5  stat(path, stat*)
 6  fstat(fd, stat*)
 7  mkdir(path, mode)
 8  unlink(path)
 9  getdents(fd, buf, size)
10  chdir(path)
11  getcwd(buf, size)
12  dup(fd)
13  dup2(oldfd, newfd)
14  pipe(fds[2])
15  fcntl(fd, cmd, arg)

16  fork()
17  execve(path, argv, envp)
18  exit(code)           [noreturn]
19  wait4(pid, status*, options)
20  getpid()
21  getppid()
22  (reserved)
23  kill(pid, sig)
24  sigaction(sig, act*, old*)
25  sigprocmask(how, set*, oldset*)
26  sigreturn()
27  alarm(sec)
28  pause()
29  nice(inc)
30  uname(utsname*)

31  brk(addr)
32  sbrk(inc)
33  mmap(addr, len, prot, flags, fd, off)
34  munmap(addr, len)

41  mount(dev, dir, type)
42  umount(dir)
43  pivot_root(new, old)
44  sync()
45  statfs(path, statfs*)

51  socket(domain, type, proto)
52  connect(fd, sockaddr*, len)
53  bind(fd, sockaddr*, len)
54  listen(fd, backlog)
55  accept(fd, sockaddr*, len*)
56  send(fd, buf, len, flags)
57  recv(fd, buf, len, flags)
58  shutdown(fd, how)
59  getaddrinfo(host, addr*)   — kernel DNS resolver

61  time(t*)
62  gettimeofday(tv*, tz*)
63  nanosleep(req*, rem*)

71  lestra_comp_create()
72  lestra_comp_destroy(win)
73  lestra_comp_blit(win, buf, w, h, fmt)
74  lestra_comp_event(win, event*, max)
75  lestra_comp_focus(win)
76  lestra_log(msg)
77  lestra_reboot()
78  lestra_poweroff()
79  lestra_version(buf, len)
```

## Errno values

```
 1 EPERM        13 EACCES       24 EMFILE
 2 ENOENT       14 EFAULT       28 ENOSPC
 3 ESRCH        16 EBUSY        29 ESPIPE
 4 EINTR        17 EEXIST       30 EROFS
 5 EIO          20 ENOTDIR      32 EPIPE
 9 EBADF        21 EISDIR
11 EAGAIN       22 EINVAL
12 ENOMEM       23 ENFILE
```

## User ABI

### Memory layout

```
0x0000000000000000  +-----------------------+
                   |  user PT_LOAD segments|
                   |  (binary .text/.data) |
                   |                       |
0x00007FFFEFE00000  +-----------------------+
                   |  user stack (256 KiB) |
                   |  grows down           |
0x00007FFFFFE00000  +-----------------------+  <- USER_STACK_TOP
                   |  sigreturn trampoline |
0x00007FFFFFD00000  +-----------------------+
                   |  (unmapped guard)     |
0xFFFF800000000000  +-----------------------+
                   |  kernel (higher-half) |
                   |  NOT accessible to user|
0xFFFFFFFFFFFFFFFF  +-----------------------+
```

### Segment selectors

```
USER_CS = 0x1B    (GDT[3] = user 64-bit code, RPL=3)
USER_DS = 0x23    (GDT[4] = user 64-bit data, RPL=3)
```

### Process startup

When the kernel `execve`s a binary, it:
1. Loads the ELF and maps PT_LOAD segments at the link-time VMA.
2. Allocates a 256 KiB user stack at USER_STACK_TOP.
3. Pushes argc, argv, envp onto the user stack (POSIX-style).
4. Sets `rip = e_entry`, `rsp = user stack`, `rflags = 0x202` (IF=1).
5. IRETQs to user mode.

The libc shim's `_start` (in `userland/libc/lestra-libc.c`) reads
argc/argv from the stack and calls `main(argc, argv, envp)`.

### Signals

We support 32 signals (1..32). The kernel delivers a signal by:
1. Building a fake return address pointing to `sigreturn_trampoline`
   (mapped at 0x00007FFFFFD00000 in every user address space).
2. Setting `rip = handler`, `rdi = signal number`.
3. On `rt_sigreturn`, restoring the saved user ctx.

Standard signals:

```
 1 SIGHUP      9 SIGKILL    17 SIGCHLD
 2 SIGINT     11 SIGSEGV    18 SIGCONT
 3 SIGQUIT    13 SIGPIPE    19 SIGSTOP
15 SIGTERM
```

`SIGKILL` and `SIGSTOP` cannot be caught or blocked.

## How to add a new userspace binary

1. Write `userland/bin/<name>.c`:
   ```c
   #include <lestra-libc.h>
   int main(int argc, char** argv) {
       printf("hello from %s\n", argv[0]);
       return 0;
   }
   ```

2. Add `<name>` to the `BINS` list in `userland/Makefile`.

3. Run `make -C userland install`. The binary is built statically
   against `userland/libc/lestra-libc.o` and placed in
   `userland/sysroot/bin/<name>`.

4. Rebuild the ext2 image: `./scripts/mkrootfs.sh`. This regenerates
   `build/lestraos-root.ext2` from `userland/sysroot/`.

5. Rebuild the ISO: `make iso` from the project root.

6. Boot in QEMU: `make run`. Login as `root`, run `<name>`.

## Porting existing C code

To port an existing C program to LestraOS:

1. Replace `#include <stdio.h>` etc. with `#include <lestra-libc.h>`.
2. The libc shim supports most of stdio.h, stdlib.h, string.h,
   unistd.h, fcntl.h, and a subset of signal.h.
3. Static linking only — no dynamic linker.
4. No threads, no mmap-based shared memory (yet), no fork+exec combo
   (both work individually).
5. The build is `-ffreestanding -nostdlib -mno-red-zone -mcmodel=large`.
6. SSE/SSE2 are disabled (kernel doesn't save FPU state yet).

## Toolchain

We use the system `gcc` (or `x86_64-elf-gcc` if installed) with
freestanding flags. No special cross-compiler is required for
userspace — only the kernel needs `-mno-red-zone -mcmodel=large`.

## File system layout (post-boot)

```
/                     ext2 (root, mounted from /dev/ram0 -> AHCI disk)
├── bin/              userland binaries
├── etc/              config: passwd, shadow, inittab, resolv.conf, ...
├── dev/              devtmpfs (console, null, tty, ...)
├── proc/             procfs (read-only)
├── tmp/              tmpfs
├── var/
│   ├── lib/
│   │   ├── pkg/      package database (installed, hooks, sigs)
│   │   └── ai/       AI API keys
│   ├── cache/pkg/    downloaded .tar.xz packages
│   └── log/          system logs
├── home/             user home directories
├── root/             root's home
└── lib/              (empty — static linking only)
```

## Init flow (post-boot)

```
kernel_main()
  -> GDT, IDT, PIC, MM, sched, syscall, VFS, initrd
  -> timer + keyboard + RTC + AHCI + E1000 + audio
  -> pkg_init() + ai_init()
  -> sti()
  -> mount ext2 root
  -> execve("/bin/init")  -> ring 3, PID 1

/bin/init (PID 1, ring 3)
  -> parse /etc/inittab
  -> sysinit: run /etc/init.d/rcS
  -> respawn: /bin/getty console 115200  (-> /bin/login -> /bin/sh)
  -> SIGCHLD handler reaps zombies
  -> SIGINT (ctrl-alt-del) -> /bin/shutdown -r now
```

## Build + test

```
make all            # build kernel + libc + userspace + initrd + ISO
make userland       # just the userland binaries
./scripts/mkrootfs.sh  # build the ext2 image from sysroot/
make iso            # package everything into a bootable ISO
make run            # boot in QEMU
tests/smoke.sh      # boot the ISO, assert on serial output
```
