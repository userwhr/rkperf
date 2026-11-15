#!/bin/bash
echo "===== 内核镜像 ====="
ls -lh /boot/vmlinuz* 2>/dev/null | head -3
ls -lh /boot/Image* 2>/dev/null | head -3
echo
echo "===== 模块目录 ====="
ls /lib/modules/$(uname -r)/ 2>/dev/null | head -5
echo "模块数量: $(ls /lib/modules/$(uname -r)/kernel 2>/dev/null | wc -l)"
echo "已加载模块: $(wc -l < /proc/modules)"
echo
echo "===== 内核符号地址（前几个）====="
head -5 /proc/kallsyms 2>/dev/null
echo "--- _text / _stext ---"
grep -E ' (T|t) (_text|_stext)$' /proc/kallsyms 2>/dev/null | head -3
echo "--- rknpu 是否内建 ---"
grep -c rknpu /proc/kallsyms 2>/dev/null
grep -m3 'rknpu' /proc/kallsyms 2>/dev/null
echo
echo "===== kptr_restrict ====="
cat /proc/sys/kernel/kptr_restrict 2>/dev/null
echo
echo "===== 内核态地址范围 ====="
grep -E 'vmalloc|vmemmap|modules' /proc/vmallocinfo 2>/dev/null | head -3
echo
echo "===== BPF 相关符号 ====="
grep -m5 -E 'bpf_prog_run|__bpf_prog' /proc/kallsyms 2>/dev/null
