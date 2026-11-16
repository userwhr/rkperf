# rkperf

采集进程在 `rknn_run` 里的耗时（RK3588 + librknnrt）。

## 编译

在 x86_64 Linux 上交叉编译，产出静态 ELF：

```bash
bash scripts/cross_build_server1.sh
```

## 部署

板子上只需要一个文件：

```bash
scp rkperf rk:~/rkperf/
```

## 使用

```bash
sudo rkperf <程序> [程序参数...]
```

例子：

```bash
# C/C++ 程序
sudo ~/rkperf/rkperf ~/yolo11_seg_demo/yolo11_seg_demo \
     ~/yolo11n-seg-4out.rknn ~/bus.jpg /tmp/o.jpg

# Python SDK（rknn-toolkit-lite2 等）
sudo ~/rkperf/rkperf python3 infer.py
```

固定的东西（不用传参）：

| 项 | 值 |
|---|---|
| 探针符号 | `rknn_run` |
| 目标库 | **自动发现**：系统路径 + Python 包里自带的副本，找到几个挂几个 |
| 日志 | `/tmp/rkperf.log` |
| 数据 | `/tmp/rkperf.log.csv` |

## 输出

**`/tmp/rkperf.log`** — 人类可读

```
[2026-11-15T20:45:19.024+08:00] [info] rkperf start: target=./yolo11_seg_demo lib=/usr/lib/librknnrt.so
[2026-11-15T20:45:19.025+08:00] [info] symbol rknn_run -> file offset 0xcc040
[2026-11-15T20:45:19.100+08:00] [info] process start: pid=1234 at 2026-11-15T20:45:19.100+0800 cmd=./yolo11_seg_demo
[2026-11-15T20:45:19.150+08:00] [info] rknn_run pid=1234 tid=1234 ctx=0x7f8c SYNC  core=core0 dur=41.802 ms ret=0 comm=yolo11_seg_demo
[2026-11-15T20:45:31.400+08:00] [info] process end: pid=1234 at 2026-11-15T20:45:31.400+0800 duration=12.300s reason=exited code=0
```

按 `Ctrl+C` 中断时，仍然会留下结束时间：

```
[2026-11-15T20:45:24.200+08:00] [info] process end: pid=1234 at 2026-11-15T20:45:24.200+0800 duration=5.120s reason=signal code=15 (interrupted)
```

**`/tmp/rkperf.log.csv`** — 数据行，可直接喂 pandas

```
# ts_in_ns,dur_ns,dur_ms,pid,tid,ctx,ret,non_block,core_mask,comm
1731490831123456789,41802000,41.802,1234,1234,0x7f8c,0,0,1,yolo11_seg_demo
```

`core_mask` 的取值：`0`=auto，`1`=core0，`2`=core1，`4`=core2，`7`=三核。

**`/tmp/rkperf.npu.csv`** — 三核使用率，每秒一行

```
# ts_ns,time,core0,core1,core2
1731490831123456789,2026-11-15T20:45:20.100+0800,45,0,0
1731490831124456789,2026-11-15T20:45:21.100+0800,38,0,0
```

数据来自 `/sys/kernel/debug/rknpu/load`，驱动的统计窗口是 **1 秒**，所以采样也是 1 Hz。

## 注意

- 需要 **root**（加载 BPF 程序）
- 只跟踪 fork 出来的子进程
- 板子内核没开 `CONFIG_KPROBES`，只能挂用户态 uprobe
