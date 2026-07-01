@echo off
setlocal

set "CLANG=C:\Users\wahit\AppData\Local\Android\Sdk\ndk\27.1.12297006\toolchains\llvm\prebuilt\windows-x86_64\bin\x86_64-linux-android21-clang"
set "SYSROOT=C:\Users\wahit\AppData\Local\Android\Sdk\ndk\27.1.12297006\toolchains\llvm\prebuilt\windows-x86_64\sysroot"
set "LESTRA_ROOT=C:\Users\wahit\LestraOS"

REM Freestanding kernel flags
set "CFLAGS=-ffreestanding -fno-builtin -Wall -Wextra -fno-exceptions -fno-rtti -nostdlib -nodefaultlibs -nostartfiles -mno-red-zone -mcmodel=kernel -mno-mmx -mno-sse -mno-sse2 -fomit-frame-pointer -O2 -I%LESTRA_ROOT%/kernel/include -I%LESTRA_ROOT%/libc/include"

REM Forward all arguments to clang
"%CLANG%" %CFLAGS% %*
