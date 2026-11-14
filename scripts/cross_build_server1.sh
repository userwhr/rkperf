#!/bin/bash
set -e
cd "$HOME"

TC="$HOME/arm-gnu-toolchain-13.3.rel1-x86_64-aarch64-none-linux-gnu"
SYS="$HOME/aarch64-sysroot"
SPD="$HOME/spdlog-1.14.1/include"

if [ ! -d "$TC" ]; then
    echo "== extract toolchain =="
    tar xf "$HOME/arm-tc.tar.xz"
fi
if [ ! -d "$SYS/usr/include" ]; then
    echo "== extract sysroot =="
    mkdir -p "$SYS"
    tar xzf "$HOME/sysroot.tgz" -C "$SYS"
fi
if [ ! -d "$SPD" ]; then
    echo "== fetch spdlog =="
    curl -sL -o "$HOME/spdlog.tar.gz" \
        https://codeload.github.com/gabime/spdlog/tar.gz/refs/tags/v1.14.1
    tar xzf "$HOME/spdlog.tar.gz" -C "$HOME"
fi

cd "$HOME/rkperf-build"

echo "== embed bpf blob (aarch64 ld) =="
"$TC/bin/aarch64-none-linux-gnu-ld" -r -b binary rkperf.bpf.o -o bpf_blob.o

echo "== cross compile C++ (static) =="
"$TC/bin/aarch64-none-linux-gnu-g++" --sysroot="$SYS" \
    -B"$SYS/usr/lib/aarch64-linux-gnu" \
    -static -std=c++17 -O2 \
    -Iinclude -I"$SPD" \
    -I"$SYS/usr/include" -I"$SYS/usr/include/aarch64-linux-gnu" \
    src/rkperf.cpp bpf_blob.o \
    -L"$SYS/usr/lib/gcc/aarch64-linux-gnu/11" \
    -L"$SYS/usr/lib/aarch64-linux-gnu" \
    -lbpf -lelf -lz -lpthread -lm \
    -o rkperf 2>&1 | tail -25

echo "== result =="
ls -lh rkperf
file rkperf 2>/dev/null || true
