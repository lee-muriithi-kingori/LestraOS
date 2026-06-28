#!/bin/bash
# Lestra OS - Cross Compiler Build Script
# Copyright (c) 2026 lestramk.org
#
# Builds x86_64-elf cross-compiler for kernel development.

set -e

PREFIX="${HOME}/opt/cross"
TARGET=x86_64-elf
JOBS=$(nproc)
BINUTILS_VER=2.41
GCC_VER=13.2.0

mkdir -p ~/src/build-cross
cd ~/src/build-cross

echo "Downloading Binutils ${BINUTILS_VER}..."
wget -q https://ftp.gnu.org/gnu/binutils/binutils-${BINUTILS_VER}.tar.xz
tar xf binutils-${BINUTILS_VER}.tar.xz

echo "Downloading GCC ${GCC_VER}..."
wget -q https://ftp.gnu.org/gnu/gcc/gcc-${GCC_VER}/gcc-${GCC_VER}.tar.xz
tar xf gcc-${GCC_VER}.tar.xz

echo "Building binutils..."
mkdir -p build-binutils
cd build-binutils
../binutils-${BINUTILS_VER}/configure \
    --target=${TARGET} --prefix=${PREFIX} \
    --with-sysroot --disable-nls --disable-werror
cd ..

echo "Building GCC (stage 1)..."
mkdir -p build-gcc
cd build-gcc
../gcc-${GCC_VER}/configure \
    --target=${TARGET} --prefix=${PREFIX} \
    --disable-nls --enable-languages=c,c++ \
    --without-headers --disable-shared --disable-libssp
make -j${JOBS} all-gcc all-target-libgcc
make install-gcc install-target-libgcc
cd ..

echo "Cross compiler installed to ${PREFIX}"
echo "Add ${PREFIX}/bin to your PATH"
