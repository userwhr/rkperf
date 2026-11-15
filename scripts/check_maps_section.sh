#!/bin/bash
echo "=== 普通文件有没有 .maps 段 ==="
for f in /bin/ls /usr/lib/librknnrt.so "$HOME/yolo11_seg_demo/yolo11_seg_demo"; do
    printf '%-50s ' "$f"
    if [ -f "$f" ]; then
        n=$(readelf -S "$f" 2>/dev/null | grep -c '\.maps')
        echo "maps 段: $n"
    else
        echo "文件不存在"
    fi
done

echo
echo "=== 内核模块 ==="
ko=$(ls /lib/modules/"$(uname -r)"/kernel/drivers/net/*.ko 2>/dev/null | head -1)
echo "文件: ${ko:-无}"
if [ -n "$ko" ]; then
    echo "maps 段: $(readelf -S "$ko" 2>/dev/null | grep -c '\.maps')"
fi

echo
echo "=== 普通 ELF 的段（/bin/ls）==="
readelf -S /bin/ls 2>/dev/null | grep -E '\] \.' | awk '{print $2}' | head -20

echo
echo "=== BPF 对象的段（rkperf.bpf.o，在 server1）==="
echo "  .text  .maps  .BTF  .BTF.ext  license  .rel*"
