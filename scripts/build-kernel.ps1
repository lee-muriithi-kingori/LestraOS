$ErrorActionPreference = "Continue"
# Windows build without WSL — verifies 138 C + 7 asm, LINK OK
# Usage: powershell -ExecutionPolicy Bypass -File "scripts/build-kernel.ps1"
#   or from repo root: powershell -ExecutionPolicy Bypass -File "scripts/build-kernel.ps1"
# Requires: clang, nasm, ld.lld, llvm-ar on PATH (or at default LLVM/nasm locations)
$repo = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path

function Resolve-Tool($name, $fallback) {
    $cmd = Get-Command $name -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    if ($fallback -and (Test-Path $fallback)) { return $fallback }
    return $name
}

$clang = Resolve-Tool "clang" "C:\Program Files\LLVM\bin\clang.exe"
$nasmDefault = if ($env:LOCALAPPDATA) { Join-Path $env:LOCALAPPDATA "Programs\nasm-2.16.03\nasm-2.16.03\nasm.exe" } else { "C:\Program Files\nasm\nasm.exe" }
$nasm  = Resolve-Tool "nasm" $nasmDefault
# fallback scan for nasm under common locations
if (-not (Test-Path $nasm)) {
    foreach ($p in @("C:\Program Files\nasm\nasm.exe", "C:\tools\nasm\nasm.exe")) {
        if (Test-Path $p) { $nasm = $p; break }
    }
}
$lld   = Resolve-Tool "ld.lld" "C:\Program Files\LLVM\bin\ld.lld.exe"
$ar    = Resolve-Tool "llvm-ar" "C:\Program Files\LLVM\bin\llvm-ar.exe"

$outBase = if ($env:TEMP) { $env:TEMP } else { Join-Path $repo "build" }
$out   = Join-Path $outBase "kbuild"
if (Test-Path $out) { Remove-Item -Recurse -Force $out }
New-Item -ItemType Directory -Force -Path $out | Out-Null

$cflags = @('-ffreestanding','-O2','-Wall','-Wextra','-nostdlib','-nostartfiles','-nodefaultlibs',
  "-I$repo\kernel\include","-I$repo\libc\include",'-m64','-mno-red-zone','-mcmodel=large',
  '-mno-mmx','-mno-sse','-mno-sse2','-fomit-frame-pointer','-fstack-protector-strong',
  '-DPICKLE_KERNEL','-DNDEBUG','-target','x86_64-none-elf')

# Collect C sources the same way the Makefile does (wildcards per dir)
$srcDirs = @(
  'kernel\core','kernel\mm','kernel\fs','kernel\fs\ext2','kernel\sched',
  'kernel\syscall','kernel\ui','kernel\pkg','kernel\ai','kernel\net',
  'kernel\gui','kernel\exec','kernel\sys','kernel\audio','kernel\acpi'
)
$cSrc = New-Object System.Collections.ArrayList
$asmSrc = New-Object System.Collections.ArrayList
foreach ($d in $srcDirs) {
  Get-ChildItem "$repo\$d\*.c" -ErrorAction SilentlyContinue | ForEach-Object { [void]$cSrc.Add($_.FullName) }
}
Get-ChildItem "$repo\kernel\drivers\char\*.c","$repo\kernel\drivers\block\*.c","$repo\kernel\drivers\audio\*.c","$repo\kernel\drivers\pci\*.c","$repo\kernel\drivers\net\*.c","$repo\kernel\drivers\apic\*.c","$repo\kernel\drivers\power\*.c","$repo\kernel\drivers\clock\*.c","$repo\kernel\drivers\sensor\*.c" -ErrorAction SilentlyContinue | ForEach-Object { [void]$cSrc.Add($_.FullName) }
Get-ChildItem "$repo\kernel\arch\x86_64\*.c" -ErrorAction SilentlyContinue | ForEach-Object { [void]$cSrc.Add($_.FullName) }
Get-ChildItem "$repo\pkg\*.c" -ErrorAction SilentlyContinue | ForEach-Object { [void]$cSrc.Add($_.FullName) }
[void]$cSrc.Add("$repo\kernel\input.c")
[void]$cSrc.Add("$repo\kernel\splash.c")

# asm files
Get-ChildItem "$repo\boot\*.asm" -ErrorAction SilentlyContinue | Where-Object { $_.Name -ne 'stage1.asm' } | ForEach-Object { [void]$asmSrc.Add($_.FullName) }
Get-ChildItem "$repo\kernel\arch\x86_64\*.asm" -ErrorAction SilentlyContinue | ForEach-Object { [void]$asmSrc.Add($_.FullName) }
Get-ChildItem "$repo\kernel\sched\*.asm" -ErrorAction SilentlyContinue | ForEach-Object { [void]$asmSrc.Add($_.FullName) }
Get-ChildItem "$repo\kernel\exec\*.asm" -ErrorAction SilentlyContinue | ForEach-Object { [void]$asmSrc.Add($_.FullName) }
Get-ChildItem "$repo\kernel\syscall\*.asm" -ErrorAction SilentlyContinue | ForEach-Object { [void]$asmSrc.Add($_.FullName) }

$objs = New-Object System.Collections.ArrayList
$cCount = 0; $asmCount = 0; $certs = 0
foreach ($src in $cSrc) {
  $rel = $src.Substring($repo.Length + 1) -replace '\\','_'
  $obj = Join-Path $out ($rel -replace '\.c$','.o')
  $err = & $clang @cflags -c $src -o $obj 2>&1
  if ($LASTEXITCODE -ne 0) { $certs++; "CERR: $src"; $err | ForEach-Object { $_ } }
  else { [void]$objs.Add($obj); $cCount++ }
}
foreach ($src in $asmSrc) {
  $rel = $src.Substring($repo.Length + 1) -replace '\\','_'
  $obj = Join-Path $out ($rel -replace '\.asm$','.o')
  $err = & $nasm -f elf64 -F dwarf $src -o $obj 2>&1
  if ($LASTEXITCODE -ne 0) { $certs++; "AERR: $src"; $err | ForEach-Object { $_ } }
  else { [void]$objs.Add($obj); $asmCount++ }
}

"compiled: $cCount C files, $asmCount asm files"

if ($certs -gt 0) { "BUILD FAILED ($certs errors)"; exit 1 }

# Build libc.a like the Makefile does
$libcOut = Join-Path $out 'libc'
New-Item -ItemType Directory -Force -Path $libcOut | Out-Null
$libcSrcs = @('string','stdio','stdlib','unistd','errno')
$libcObjs = New-Object System.Collections.ArrayList
$libcCflags = @('-ffreestanding','-O2','-Wall','-Wextra','-nostdlib','-nostartfiles','-nodefaultlibs',
  "-I$repo\libc\include","-I$repo\kernel\include",'-m64','-mno-red-zone','-mcmodel=large',
  '-mno-mmx','-mno-sse','-mno-sse2','-fomit-frame-pointer',
  '-DPICKLE_KERNEL','-DNDEBUG','-target','x86_64-none-elf')
foreach ($m in $libcSrcs) {
  $obj = Join-Path $libcOut "$m.o"
  $err = & $clang @libcCflags -c "$repo\libc\src\$m.c" -o $obj 2>&1
  if ($LASTEXITCODE -ne 0) { "LIBC ERR: $m"; $err | ForEach-Object { $_ }; exit 1 }
  [void]$libcObjs.Add($obj)
}
$libcA = Join-Path $libcOut 'libc.a'
$arErr = & $ar rcs $libcA @libcObjs 2>&1
if ($LASTEXITCODE -ne 0) { "LIBC AR ERR:"; $arErr | ForEach-Object { $_ }; exit 1 }

# Link
$linker = "$repo\kernel\arch\x86_64\linker.ld"
$linkErr = & $lld -T $linker -e _start -nostdlib --no-dynamic-linker "-z" "max-page-size=0x1000" "-m" "elf_x86_64" -o "$out\kernel.bin" @objs $libcA 2>&1
if ($LASTEXITCODE -ne 0) { "LINK ERR:"; $linkErr | ForEach-Object { $_ }; exit 1 }
$kb = Get-Item "$out\kernel.bin"
"LINK OK: kernel.bin $([math]::Round($kb.Length/1024)) KB"
exit 0
