#!/bin/bash
echo "===== CPU ====="
lscpu 2>/dev/null | grep -E "^(Architecture|CPU\(s\)|Model name|Thread|Core|Socket|CPU max|CPU min|BogoMIPS)" 
echo "--- 频率 ---"
for c in 0 4 6; do
    f=/sys/devices/system/cpu/cpu$c/cpufreq/scaling_cur_freq
    [ -f "$f" ] && echo "cpu$c: $(( $(cat $f) / 1000 )) MHz"
done
echo "--- governor ---"
cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null
echo "===== MEM ====="
free -h
echo "===== STORAGE ====="
lsblk -o NAME,SIZE,TYPE,MOUNTPOINT 2>/dev/null | head -10
echo "--- 挂载 ---"
df -h / /home 2>/dev/null | tail -3
echo "===== THERMAL ====="
for z in /sys/class/thermal/thermal_zone*/temp; do
    [ -f "$z" ] && echo "$z: $(( $(cat $z) / 1000 )) C"
done 2>/dev/null | head -5
echo "===== NPU devfreq ====="
ls /sys/class/devfreq/ 2>/dev/null
for d in /sys/class/devfreq/*/; do
    n=$(basename "$d")
    [ -f "$d/cur_freq" ] && echo "$n cur=$(cat $d/cur_freq)"
done 2>/dev/null | head -6
echo "===== KERNEL ====="
uname -r
echo "===== LIBBPF ====="
ls -l /usr/lib/aarch64-linux-gnu/libbpf.so.0.5.0 2>/dev/null | awk '{print $5" bytes"}'
