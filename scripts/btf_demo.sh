#!/bin/bash
set -e
W=/tmp/btftest
rm -rf $W && mkdir -p $W
cd $W

cat > t.bpf.c <<'EOF'
#include <linux/types.h>
#include <linux/bpf.h>
#include <bpf/bpf_helpers.h>

struct my_ctx { __u32 a; __u64 b; };

struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __type(key, __u32);
    __type(value, __u64);
    __uint(max_entries, 16);
} mymap SEC(".maps");

SEC("uprobe")
int probe(void *ctx)
{
    __u32 k = 1;
    __u64 v = 2;
    bpf_map_update_elem(&mymap, &k, &v, BPF_ANY);
    return 0;
}
char LICENSE[] SEC("license") = "GPL";
EOF

CFLAGS="-O2 -target bpf -D__TARGET_ARCH_arm64 -I/usr/include/aarch64-linux-gnu"

echo "=== 不加 -g ==="
clang $CFLAGS -c t.bpf.c -o no_g.o
readelf -S no_g.o | grep -E 'BTF' || echo "  没有 BTF 段"
ls -l no_g.o | awk '{print "  大小:", $5, "字节"}'

echo
echo "=== 加 -g ==="
clang $CFLAGS -g -c t.bpf.c -o with_g.o
readelf -S with_g.o | grep -E '\.BTF'
ls -l with_g.o | awk '{print "  大小:", $5, "字节"}'

echo
echo "=== 我们的 rkperf.bpf.o 的 BTF 段大小 ==="
readelf -S "$HOME/rkperf-build/rkperf.bpf.o" 2>/dev/null | grep -E '\.BTF' || true

rm -rf $W
