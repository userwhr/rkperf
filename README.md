# rkperf

采集进程在 `rknn_run` 里的耗时（RK3588 + librknnrt）。

## 代码结构

| 文件 | 职责 |
|---|---|
| `src/rkperf.cpp` | `main`：命令行、建输出目录、调 rktrace、收尾 |
| `src/rktrace.cpp` | 找 `librknnrt.so`、解析符号偏移、加载内嵌 BPF、fork/exec、挂探针、poll 收事件 |
| `src/rksample.cpp` | NPU / DDR / 内存三个采样线程 |
| `src/rklog.cpp` | 可读日志 + `events/npu/ddr/mem.csv` |
| `src/rksummary.cpp` | `summary/`：环境快照、`config.json`、`OVERVIEW.md`、`stats.json` |
| `src/rkperf.bpf.c` | BPF 探针本体（uprobe/uretprobe） |
| `src/rk_event.h` | BPF 与用户态共享的事件结构，两边布局必须一致 |
| `src/rkrun.h` | 一次采集的结果结构（rktrace 填，rksummary 用） |
| `src/rkutil.h` | 时间戳、hex、输出目录等 inline 小工具 |

## 编译

```bash
git clone https://github.com/userwhr/rkperf.git
cd rkperf
./scripts/build.sh
```

脚本自动判断架构：**aarch64 本机编译**，其他架构**交叉编译**。产物在 `build/rkperf`。

> 交叉编译需要 **Linux 环境**（物理机、虚拟机、WSL2 或 Docker 都行）。Windows 原生环境跑不了 aarch64 交叉工具链。

### 依赖

**板子上编译（aarch64）**

```bash
sudo apt install clang g++ make libbpf-dev libelf-dev zlib1g-dev curl
```

**x86_64 交叉编译**

需要两样东西：交叉工具链 + 板子的 sysroot。

```bash
# 1) 工具链
sudo apt install g++-aarch64-linux-gnu
#   或者用 Arm 官方工具链，然后 CROSS_PREFIX=aarch64-none-linux-gnu-

# 2) sysroot：在板子上打包，再拷到编译机
#    板子上：
./scripts/build.sh --make-sysroot        # 生成 sysroot.tgz（约 120 MB）
#    编译机上：
scp board:~/rkperf/sysroot.tgz .
mkdir -p ~/aarch64-sysroot
tar xzf sysroot.tgz -C ~/aarch64-sysroot

# 3) 编译
./scripts/build.sh --cross
```

自定义路径：

```bash
SYSROOT=/opt/rk3588-sysroot CROSS_PREFIX=aarch64-none-linux-gnu- ./scripts/build.sh --cross
```

完整依赖说明：`./scripts/build.sh --deps`

> spdlog 会自动下载到 `build/`，不需要提前安装。

## 部署

板子上只需要一个文件：

```bash
scp build/rkperf rk:~/rkperf/
```

## 使用

```bash
sudo rkperf <程序> [程序参数...]
```

例子：

```bash
# C/C++ 程序
sudo ~/rkperf/rkperf ./your_demo model.rknn input.jpg

# Python SDK（rknn-toolkit-lite2 等）
sudo ~/rkperf/rkperf python3 your_script.py
```

固定的东西（不用传参）：

| 项 | 值 |
|---|---|
| 探针符号 | `rknn_run` |
| 目标库 | **自动发现**：系统路径 + Python 包里自带的副本，找到几个挂几个 |
| 输出目录 | `/tmp/rkperf_<时间戳>/`（每次运行新建） |

### 跑 Python SDK

`rkperf` 需要 root，而 `pip install --user` 装的包在 `~/.local` 下；`sudo` 会重置 `HOME`，
所以要显式把 `PYTHONPATH` 传进去：

```bash
sudo env HOME=/home/orangepi \
         PYTHONPATH=/home/orangepi/.local/lib/python3.10/site-packages \
         ~/rkperf/rkperf python3 examples/py_infer.py
```

或者把包装到系统里（`sudo pip install`），就不用管这个了。

## 输出

每次运行会在 `/tmp` 下建一个带时间戳的目录，方便区分不同时间的测试：

```
/tmp/rkperf_20261116_193609/
├── rkperf.log      可读日志
├── events.csv      每次 rknn_run 的数据
├── npu.csv         三个 NPU 核的使用率
├── ddr.csv         DDR 带宽占用率
├── mem.csv         进程 VmRSS / VmHWM / 线程数
└── summary/        环境快照 + 配置 + 总览（见下一节）
```

**`rkperf.log`** — 可读日志

```
[2026-11-15T20:45:19.024+08:00] [info] rkperf 0.2.0 start: target=./infer_demo
[2026-11-15T20:45:19.025+08:00] [info] symbol rknn_run -> file offset 0xcc040
[2026-11-15T20:45:19.100+08:00] [info] process start: pid=1234 at 2026-11-15T20:45:19.100+0800 cmd=./infer_demo
[2026-11-15T20:45:19.150+08:00] [info] rknn_run pid=1234 tid=1234 ctx=0x7f8c SYNC  core=core0 dur=41.802 ms ret=0 comm=infer_demo
[2026-11-15T20:45:31.400+08:00] [info] process end: pid=1234 at 2026-11-15T20:45:31.400+0800 duration=12.300s reason=exited code=0
```

按 `Ctrl+C` 中断时，仍然会留下结束时间：

```
[2026-11-15T20:45:24.200+08:00] [info] process end: pid=1234 at 2026-11-15T20:45:24.200+0800 duration=5.120s reason=signal code=15 (interrupted)
```

**`events.csv`** — 数据行，可直接喂 pandas

```
# ts_in_ns,dur_ns,dur_ms,pid,tid,ctx,ret,non_block,core_mask,comm
1731490831123456789,41802000,41.802,1234,1234,0x7f8c,0,0,1,infer_demo
```

`core_mask` 的取值：`0`=auto，`1`=core0，`2`=core1，`4`=core2，`7`=三核。

**`npu.csv`** — 三核使用率，每秒一行

```
# ts_ns,time,core0,core1,core2
1731490831123456789,2026-11-15T20:45:20.100+0800,45,0,0
1731490831124456789,2026-11-15T20:45:21.100+0800,38,0,0
```

数据来自 `/sys/kernel/debug/rknpu/load`，驱动的统计窗口是 **1 秒**，所以采样也是 1 Hz。

**`ddr.csv`** — DDR 带宽占用率，每 100 ms 一行

```
# ts_ns,time,load_pct,freq_hz
26460869979302,2026-11-16T21:24:28.321+0800,35,528000000
26461185884344,2026-11-16T21:24:28.637+0800,46,2112000000
```

数据来自 `/sys/class/devfreq/dmc/load`（格式 `46@2112000000Hz`），底层是 DDR 控制器的硬件计数器。
DMC 频率变化时会额外在主日志里记一条：

```
[info] ddr freq: 528 MHz -> 2112 MHz (up)
[info] ddr freq: 2112 MHz -> 528 MHz (down)
```

**`mem.csv`** — 进程内存，每 5 s 一行

```
# ts_ns,time,rss_kb,hwm_kb,threads
26460869979302,2026-11-16T21:24:28.321+0800,182344,201488,12
```

数据来自 `/proc/<pid>/status` 的 `VmRSS` / `VmHWM` / `Threads`。
注意 dma-buf / DMA heap 这类驱动侧内存不计入 `VmRSS`，所以这里看的是进程自身内存和线程数的趋势，
不是"NPU 用了多少内存"。

## summary/ —— 一次采集的自描述

每次采集都会在输出目录下多写一个 `summary/`，对应 nxperf 的 `summary/`：
**换个人、隔两周来看这份数据，也能知道当时是什么环境、怎么采的。**

```
summary/
├── OVERVIEW.md          总览：调用次数、耗时分布、三核利用率、DDR、内存、环境
├── stats.json           同一份统计的机器可读版（喂 pandas / 报告生成）
├── config.json          本次怎么采的：目标、完整参数、探针地址、平台
├── uname.txt            内核与主机
├── lscpu.txt            CPU 拓扑与频率上限
├── mount.txt / df.txt   挂载表与磁盘用量（找 NFS / IO 路径）
├── net.txt              网卡与流量计数
├── board.txt            os-release / kernel / cmdline / meminfo
├── npu.txt              NPU 驱动、频点、dmesg
├── libs.txt             实际挂上的 librknnrt.so
└── proc/<pid>/          目标进程的 /proc 快照（status / smaps_rollup / limits / maps / tasks）
```

`OVERVIEW.md` 大致长这样（中间几节省略）：

```markdown
| 项 | 值 |
|---|---|
| 目标程序 | `./tile_yolo` |
| 时长 | 12.300 s |
| 退出 | exited，code 0 |
| 平台 | Linux 5.10.160 aarch64 · rk |
| 探针 | /usr/lib/librknnrt.so @0x10c40；/lib/aarch64-linux-gnu/libc.so.6 @0x84230 |

- `rknn_run` **60** 次（SYNC 47 / ASYNC 13），失败 0 次
- 单次耗时：中位 **41.802 ms**，P90 44.132 ms，P99 46.804 ms，最大 47.011 ms
- 平均并发度 **0.20**（三核理论上限 3.00）
- DDR：均值 35.0%，P90 61.0%，峰值 78.0%，主频点 2112.0 MHz
- NPU core0：均值 45.0%，峰值 62.0%
```

采集完先看这一份，不用再翻三个 CSV 做心算。几个口径说明：

- **平均并发度** = Σ`rknn_run` 耗时 / 采集时长。接近 3.00 说明三核在同时跑；0.2 说明大部分时间在等（预处理/后处理/IO）。
- **NPU 利用率**来自驱动 `/sys/kernel/debug/rknpu/load`，统计窗口 1 s，所以是 1 s 平均值，不是瞬时值。
- **DDR 均值频点**是采样期内的加权平均；下面还给了频点分布，调频策略的影响一眼能看出来。
- 数据缺口会在对应小节写"无数据"并给原因（没 root / 节点不可读），不会静默留空。

不想要 `summary/`：

```bash
sudo env RKPERF_NO_SUMMARY=1 ~/rkperf/rkperf ./your_demo
```

## 注意

- 需要 **root**（加载 BPF 程序）
- 只跟踪 fork 出来的子进程
- 板子内核没开 `CONFIG_KPROBES`，只能挂用户态 uprobe
