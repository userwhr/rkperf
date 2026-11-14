#!/bin/bash
echo "--- libbpf version ---"
ls -l /usr/lib/aarch64-linux-gnu/libbpf.so* 2>/dev/null
echo "--- uprobe attach symbols ---"
nm -D /usr/lib/aarch64-linux-gnu/libbpf.so.0.5.0 2>/dev/null | grep -E "attach_uprobe|attach_kprobe|perf_buffer__new|ring_buffer__new" || echo "nm missing or no symbols"
echo "--- headers ---"
ls /usr/include/bpf/ 2>/dev/null || echo NO_BPF_HEADERS
echo "--- libelf / zlib ---"
ls /usr/include/libelf.h /usr/include/gelf.h /usr/include/zlib.h 2>/dev/null || echo MISSING_DEV_HEADERS
echo "--- bpftool ---"
command -v bpftool || echo NO_BPFTOOL
echo "--- clang bpf target ---"
clang --version 2>/dev/null | head -2
