# Scripts (tools)

Tools live at `scripts/` — alias `tools` in docs (real path stays `scripts/` to avoid breaking Makefiles).

## Scripts

- `mkinitrd.py` — build initrd image: `python3 scripts/mkinitrd.py <output.img> <file1> [file2] ...` (format: u32 count + 64-byte name + u32 size + data, parsed by `fs/vfs.c:initrd_load()`)
- `mkext2.py` — build ext2 image: `python3 scripts/mkext2.py <sysroot_dir> <output.img> <size_mb>` (prefers mke2fs, falls back to genext2fs/Python stub)
- `cross-compiler.sh` — build x86_64-elf cross-compiler (binutils 2.42 + gcc 13.2.0) to `$HOME/opt/cross` (`TARGET=x86_64-elf`)
- `smoke_cloud.sh` — QEMU serial smoke boot (30 s, checks "kernel initialized successfully")

Other helpers: `mkrootfs.sh`, `fix_smap_compat.py`, `make_tiny_gguf.py`, `sync_lestramanika.sh`.

## Usage

`mkinitrd` example:

```sh
python3 scripts/mkinitrd.py build/initrd.img user/bin/hello user/bin/sysinfo
```

`make iso` calls `mkinitrd.py` + `mkext2.py` internally. See `docs/BUILD.md` and `docs/STRUCTURE.md` (tools alias).
