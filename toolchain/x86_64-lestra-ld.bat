@echo off
setlocal

set "LLD=C:\Users\wahit\AppData\Local\Android\Sdk\ndk\27.1.12297006\toolchains\llvm\prebuilt\windows-x86_64\bin\ld.lld"
set "LESTRA_ROOT=C:\Users\wahit\LestraOS"

REM Forward all arguments to ld.lld
"%LLD%" %*
