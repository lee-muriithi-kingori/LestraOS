# Final Fix Report — Repo Production Reorg (2 Important findings)

**Plan:** `docs/superpowers/plans/2026-08-28-repo-production-reorg.md`
**Base:** `337b1cd` (`chore: editorconfig + CI hygiene`)
**Date:** 2026-08-28
**Executor:** final-fix implementer
**Branch:** `main` (local HEAD `337b1cd` → new commit)
**Global constraints:** Build green 138 C + 7 asm LINK OK, no fake docs, `kernel/include/lestra/*.h` stable

---

## Findings fixed (2 Important — as scoped)

### 1. `docs/BUILD.md:91-122` Project Structure tree stale

**Problem:** Tree listed `drivers` truncated (`vga, keyboard...`), still claimed `syscall (29)` (fixed earlier to 67 but other staleness remained), top-level `pkg/` + `desktop/` (both false — no top-level `pkg/`, no `desktop/`), omitted `kernel/acpi`, `kernel/pkg`, `kernel/sys`, `kernel/ui`, `.github/workflows`, `tools` alias, `third_party/lestramanika`. Contradicted `docs/STRUCTURE.md:4-25` (17 dirs) and real `Get-ChildItem kernel` (17) and top-level (`boot, kernel, libc, user, scripts, docs, third_party, screenshots, .github, Makefile, etc.`).

**Fix:** Replaced `docs/BUILD.md:91-122` tree with canonical 17-dir listing matching `docs/STRUCTURE.md` and `Get-ChildItem`:

```
kernel subdirs (17, verified):
acpi, ai, arch, audio, core, drivers, exec, fs, gui, include, mm, net, pkg, sched, sys, syscall, ui
```

New tree (`docs/BUILD.md:91-122`):

```
LestraOS/
├── boot/               # stage1.asm, boot.asm, grub.cfg — Multiboot2 + MBR
├── kernel/             # 40k lines C+ASM — 17 subdirs, see kernel/README.md & docs/STRUCTURE.md
│   ├── arch/x86_64/    # GDT/IDT/ISR, linker.ld, framebuffer
│   ├── core/           # kernel_main, panic, printk, shell
│   ├── mm/             # PMM bitmap, VMM paging, heap
│   ├── sched/          # preemptive RR, context_switch.asm
│   ├── syscall/        # 67 calls 0-66, SYSCALL/SYSRET
│   ├── exec/           # ELF + PE, ldso, pipe, signals
│   ├── fs/             # VFS + ext2/FAT32/procfs/devfs/tmpfs/tarfs
│   ├── net/            # TCP/IP + TLS 1.2 + HTTP + wifi framework
│   ├── drivers/        # block/net/char/pci/apic/audio/clock/...
│   ├── gui/            # 42-file compositor 60Hz
│   ├── ui/             # cyberpunk text-mode UI (themes, panels)
│   ├── ai/ + audio/    # GGUF inference, TTS/STT
│   ├── acpi/           # ACPI tables & power mgmt
│   ├── pkg/            # lestra-pkg, preinstalled manifests
│   ├── sys/            # cron, sandbox, ssh_server, net_config
│   └── include/lestra/ # public headers <lestra/...> — keep stable
├── libc/               # C library (alias libs/libc in docs)
├── user/               # init + shell + bin (alias userspace)
├── scripts/            # mkinitrd, mkext2, cross-compiler (alias tools/)
├── docs/               # ARCHITECTURE, BUILD, NETWORKING, etc.
├── third_party/lestramanika  # GGUF submodule
├── .github/workflows/  # CI build + smoke boot
├── screenshots/        # boot/docs images
├── tools/              # alias → scripts/ (see tools/README.md)
└── Makefile + env.sh + LICENSE
```

Removed fake `pkg/` + `desktop/` top-level entries, restored `acpi, pkg, sys, ui, .github/workflows, tools alias, third_party/lestramanika, screenshots`. Verbatim check: `Select-String -Path docs/BUILD.md -Pattern "desktop" -SimpleMatch` → 0 hits; `Select-String -Pattern "^├── pkg/"` only inside `kernel/` now.

Cross-check `docs/STRUCTURE.md` (28 lines) unchanged — already correct 17-dir tree with aliases note. `docs/BUILD.md` now matches it.

### 2. Machine-specific absolute Windows path leaking personal TMP

**Problem:** `C:\Users\leeki\AppData\Local\Temp\opencode\build-kernel.ps1` in
- `CONTRIBUTING.md:24`
- `docs/BUILD.md:28`
- `kernel/README.md:24` (ellipsis `.../build-kernel.ps1` — ambiguous but still personal-path lineage)
- `libc/README.md:7` (same ellipsis lineage)

Leaks personal `C:\Users\leeki\...` TMP and is not a repo path.

**Fix:** Replaced with generic repo path `scripts/build-kernel.ps1` + keep one Windows example but not personal path, per instruction `powershell -ExecutionPolicy Bypass -File "scripts/build-kernel.ps1"` or `make all` + WSL note.

- `CONTRIBUTING.md:24-30`:
  ```powershell
  powershell -ExecutionPolicy Bypass -File "scripts/build-kernel.ps1"
  # expected: 138 C + 7 asm, LINK OK
  ```
  + WSL note `make all && make run`

- `docs/BUILD.md:25-31`:
  ```powershell
  powershell -ExecutionPolicy Bypass -File "scripts/build-kernel.ps1"
  ```
  `This verifies 138 C + 7 asm, LINK OK without WSL (requires clang, nasm, ld.lld — see script header). Alternatively use WSL2 … run make all`

- `kernel/README.md:25`: `Built via top-level Makefile (make all) and powershell -ExecutionPolicy Bypass -File "scripts/build-kernel.ps1" — 138 C + 7 asm, LINK OK.`

- `libc/README.md:7,14`: `also via powershell -ExecutionPolicy Bypass -File "scripts/build-kernel.ps1"` and `Linked … via top-level Makefile and scripts/build-kernel.ps1`

**New file:** `scripts/build-kernel.ps1` (94 lines) — repo-relative copy of the `C:\...\opencode\build-kernel.ps1` runner, now committed so the generic path is real and not fake docs. Key changes vs temp runner:
- `$repo = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path` (not hardcoded `C:\Users\leeki\...`)
- `Resolve-Tool` helper + `$env:LOCALAPPDATA` for nasm fallback (no `C:\Users\leeki\...` leak; previous fallback hardcoded personal path replaced with `Join-Path $env:LOCALAPPDATA "Programs\nasm-2.16.03\nasm-2.16.03\nasm.exe"`)
- `$out = Join-Path $env:TEMP "kbuild"` (not `C:\Users\leeki\AppData\Local\Temp\opencode\kbuild`)
- Identical compile/link semantics (same `$srcDirs`, driver globs, cflags `-ffreestanding -m64 -mno-red-zone -mcmodel=large … -target x86_64-none-elf`, same 5-file `libc.a`, same `ld.lld -T kernel/arch/x86_64/linker.ld`)

Verification that no committed doc still leaks personal path (excluding historical `.superpowers` reports and `docs/superpowers/plans` plan which are plan archives):
```
Select-String -Path "CONTRIBUTING.md","docs/BUILD.md","kernel/README.md","libc/README.md","scripts/build-kernel.ps1" -Pattern "C:\\Users\\leeki" → 0 hits
Select-String -Path "CONTRIBUTING.md","docs/BUILD.md","kernel/README.md","libc/README.md" -Pattern "AppData\\Local\\Temp\\opencode" → 0 hits
Select-String -Path "docs/BUILD.md","CONTRIBUTING.md","kernel/README.md","libc/README.md" -Pattern "scripts/build-kernel.ps1" → 4 hits (expected)
```

Preserved one Windows example (`docs/BUILD.md:28`, `CONTRIBUTING.md:26`, `kernel/README.md:25`, `libc/README.md:7`) but generic.

---

## Files modified (5 + report)

| File | Change |
|------|--------|
| `docs/BUILD.md` | Windows section `C:\...\opencode\build-kernel.ps1` → `scripts/build-kernel.ps1` + WSL note; project tree `pkg/desktop` removed, 17-dir kernel tree + top-level `third_party/.github/tools/screenshots` restored, matching `docs/STRUCTURE.md` |
| `CONTRIBUTING.md` | Windows `C:\...\opencode\build-kernel.ps1` → `scripts/build-kernel.ps1` |
| `kernel/README.md` | `powershell .../build-kernel.ps1` → `powershell -ExecutionPolicy Bypass -File "scripts/build-kernel.ps1"` (with `make all`) |
| `libc/README.md` | `powershell .../build-kernel.ps1` → `powershell -ExecutionPolicy Bypass -File "scripts/build-kernel.ps1"`; `build-kernel.ps1` → `scripts/build-kernel.ps1` |
| `scripts/build-kernel.ps1` | **NEW** — repo-relative Windows build script (generic, no personal TMP) |
| `.superpowers/sdd/2026-08-28-repo-production-reorg/task-final-fix-report.md` | this report |

No other files touched. No minors fixed. `kernel/include/lestra/*.h` untouched — stable.

---

## Verification

### Build green 138 C + 7 asm LINK OK

**Commit-time verification (both runners identical):**

```
# old temp runner (still present for CI parity)
powershell -ExecutionPolicy Bypass -File "C:\Users\leeki\AppData\Local\Temp\opencode\build-kernel.ps1"
→ compiled: 138 C files, 7 asm files
  LINK OK: kernel.bin 992 KB
  exit 0

# new repo runner
powershell -ExecutionPolicy Bypass -File "scripts/build-kernel.ps1"
→ compiled: 138 C files, 7 asm files
  LINK OK: kernel.bin 992 KB
  exit 0
```

Exit `0` both. Verbatim `compiled: 138 C files, 7 asm files` + `LINK OK: kernel.bin 992 KB` matches global constraint. Kernel `include/lestra/` count: `Get-ChildItem kernel/include/lestra/*.h | Measure-Object → 54` unchanged (no renames).

**Mechanism:** `make all` Makefile wildcards untouched; `scripts/build-kernel.ps1` collects same `$srcDirs` + driver globs + `arch/x86_64` + `input.c` + `splash.c` = 138 C, 7 asm (`boot/boot.asm` + `arch/*.asm` + `sched/*.asm` + `exec/*.asm` + `syscall/*.asm`) — matches `task-5-report.md:90` and `task-6-report.md` lineage.

### No fake docs

- `docs/BUILD.md` tree now lists only real dirs (verified `Get-ChildItem kernel -Directory → 17`, `Get-ChildItem . -Directory → .github, boot, docs, kernel, libc, screenshots, scripts, third_party, tools, user` etc.). `desktop` removed (was dead code per old comment `desktop/ — Dead code — not called; kernel_main.c uses kernel/gui/ directly` — correctly removed). `pkg/` only under `kernel/pkg`.
- `docs/STRUCTURE.md` unchanged 28 lines, still honest 17-dir + aliases note.
- `scripts/build-kernel.ps1` is real file — `Test-Path scripts/build-kernel.ps1 → True`, `powershell -File scripts/build-kernel.ps1` produces LINK OK, so docs reference is not fake.
- No Wi-Fi/BT/USB claims added.

### Header stability

```
kernel/include/lestra/*.h — 54 files, no renames, include path <lestra/...> stable
Before: 54 headers (task-6)
After:  54 headers (Get-ChildItem kernel/include/lestra | Measure-Object → 54)
Diff of kernel/include/lestra/*.h vs base 337b1cd → empty
```

### Structure match

```
Get-ChildItem kernel -Directory | Sort-Object Name
→ acpi ai arch audio core drivers exec fs gui include mm net pkg sched sys syscall ui (17)
docs/BUILD.md kernel tree lists same 17 (acpi, ai, arch, audio, core, drivers, exec, fs, gui, include/lestra, mm, net, pkg, sched, sys, syscall, ui)
Top-level BUILD.md now lists boot, kernel, libc, user, scripts, docs, third_party/lestramanika, .github/workflows, screenshots, tools, Makefile+env.sh+LICENSE — matching docs/STRUCTURE.md 4-25 semantics
```

---

## Git

**Base:** `337b1cd chore: editorconfig + CI hygiene`
**New commit (1):** `fix: scrub personal build path, align BUILD tree with STRUCTURE and filesystem`

```bash
git add docs/BUILD.md CONTRIBUTING.md kernel/README.md libc/README.md scripts/build-kernel.ps1 .superpowers/sdd/2026-08-28-repo-production-reorg/task-final-fix-report.md
git commit -m "fix: scrub personal build path, align BUILD tree with STRUCTURE and filesystem"
```

**Diff stat vs 337b1cd:**

```
 CONTRIBUTING.md                                                 |  4 +-
 docs/BUILD.md                                                   | 38 ++++++++++++++++++++++++---------------
 kernel/README.md                                                |  2 +-
 libc/README.md                                                  |  4 +-
 scripts/build-kernel.ps1                                        | 94 +++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
 .superpowers/sdd/2026-08-28-repo-production-reorg/task-final-fix-report.md | ~this file
 6 files changed
```

Only the 2 Important findings fixed; no minors.

---

## Status: ✅ PASS

- [x] `docs/BUILD.md:91-122` tree now matches `docs/STRUCTURE.md:4-25` and `Get-ChildItem kernel` 17 + top-level real dirs; fake `pkg/desktop` removed
- [x] `C:\Users\leeki\AppData\Local\Temp\opencode\build-kernel.ps1` removed from `CONTRIBUTING.md:24`, `docs/BUILD.md:28`, `kernel/README.md:24`, `libc/README.md:7` → generic `scripts/build-kernel.ps1` / `make all` + WSL; one Windows example kept without personal path; `scripts/build-kernel.ps1` committed and builds
- [x] Build green `138 C + 7 asm LINK OK` (both runners, exit 0, 992 KB)
- [x] No fake docs — tree real, `scripts/build-kernel.ps1` real, headers stable 54
- [x] One commit, conventional `fix:` prefix

**Return:**
- **Status:** PASS
- **Commit:** 1 new commit on top of `337b1cd` (see `git log --oneline -1`)
- **Build:** `138 C + 7 asm LINK OK` (992 KB) — PASS
- **Report file:** `C:\Users\leeki\Documents\Default Project\LestraOS\.superpowers\sdd\2026-08-28-repo-production-reorg\task-final-fix-report.md`
