#!/bin/bash
#
# Lestra OS - Cross-compiler build script
# Copyright (c) 2026 lestramk.org
#
# Builds an x86_64-elf cross-compiler (binutils + gcc) for freestanding
# kernel development. Installs to $HOME/opt/cross by default.
#
# Prerequisites (Ubuntu/Debian):
#   sudo apt install build-essential bison flex libgmp3-dev libmpc-dev \
#                    libmpfr-dev texinfo wget
#
# Usage:
#   ./build/cross-compiler.sh
#
# After build:
#   export PATH=$HOME/opt/cross/bin:$PATH
#   make all
#

set -e

# Configuration
TARGET=x86_64-elf
PREFIX="$HOME/opt/cross"
BUILD_DIR="$(mktemp -d)"
JOBS=$(nproc)

# Versions
BINUTILS_VERSION=2.42
GCC_VERSION=13.2.0

# Mirror URLs
BINUTILS_URL="https://ftp.gnu.org/gnu/binutils/binutils-${BINUTILS_VERSION}.tar.xz"
GCC_URL="https://ftp.gnu.org/gnu/gcc/gcc-${GCC_VERSION}/gcc-${GCC_VERSION}.tar.xz"

echo "=========================================="
echo "  Lestra OS Cross-Compiler Builder"
echo "=========================================="
echo "Target:   $TARGET"
echo "Prefix:   $PREFIX"
echo "Jobs:     $JOBS"
echo "Build:    $BUILD_DIR"
echo "=========================================="
echo ""

# Check prerequisites
echo "Checking prerequisites..."
for cmd in wget tar make gcc g++; do
    if ! command -v $cmd >/dev/null 2>&1; then
        echo "ERROR: $cmd not found. Please install it first."
        exit 1
    fi
done

# Create prefix directory
mkdir -p "$PREFIX"

# Download and build binutils
echo ""
echo "===== Building binutils $BINUTILS_VERSION ====="
cd "$BUILD_DIR"
if [ ! -f "binutils-${BINUTILS_VERSION}.tar.xz" ]; then
    wget "$BINUTILS_URL"
fi
tar xf "binutils-${BINUTILS_VERSION}.tar.xz"

mkdir -p build-binutils && cd build-binutils
"../binutils-${BINUTILS_VERSION}/configure" \
    --target="$TARGET" \
    --prefix="$PREFIX" \
    --with-sysroot \
    --disable-nls \
    --disable-werror
make -j"$JOBS"
make install

# Download and build GCC
echo ""
echo "===== Building GCC $GCC_VERSION ====="
cd "$BUILD_DIR"
if [ ! -f "gcc-${GCC_VERSION}.tar.xz" ]; then
    wget "$GCC_URL"
fi
tar xf "gcc-${GCC_VERSION}.tar.xz"

# Download GCC prerequisites
cd "gcc-${GCC_VERSION}"
./contrib/download_prerequisites || true
cd ..

mkdir -p build-gcc && cd build-gcc
"../gcc-${GCC_VERSION}/configure" \
    --target="$TARGET" \
    --prefix="$PREFIX" \
    --disable-nls \
    --disable-werror \
    --enable-languages=c,c++ \
    --without-headers \
    --disable-shared \
    --disable-threads \
    --disable-libssp \
    --disable-libgomp \
    --disable-libmudflap \
    --disable-libquadmath \
    --disable-libatomic
make -j"$JOBS" all-gcc
make install-gcc
make -j"$JOBS" all-target-libgcc
make install-target-libgcc

echo ""
echo "=========================================="
echo "  Cross-compiler build complete!"
echo "=========================================="
echo ""
echo "Add to your PATH:"
echo "  export PATH=$PREFIX/bin:\$PATH"
echo ""
echo "Verify with:"
echo "  $TARGET-gcc --version"
echo ""
echo "Then build LestraOS:"
echo "  make all"
echo "  make run"
echo ""

# Cleanup
rm -rf "$BUILD_DIR"
