# RKNPU job 与 RKNN 层的对应关系

> 结论：一次 `rknn_run` 内部，**1 个 NPU 层 → 1 次 ioctl → 1 个驱动 job**；每个 job 携带 N 个 task，task 才是硬件执行单位。

## 1. 三层概念

| 概念 | 归属 | 一帧数量 | 怎么看到 |
|---|---|---|---|
| RKNN 层（OpType） | 编译器 | 125 | eval_perf / `RKNN_QUERY_PERF_DETAIL` |
| 驱动 job | 内核驱动 | **117** | `ioctl(fd, 0xC0686441, ...)` 的次数 |
| task | 硬件 | **769** | `rknpu_submit.subcore_task[]` 区间之和 |

对账：`125 − 3（CPU 层） − 5（空 Reshape） = 117 = job 数`。

## 2. 怎么发现的

三个独立测量，最后数量对上：

**① 数 ioctl**

```bash
sudo strace -e trace=ioctl -o /tmp/io.txt ./perfq model.rknn
grep -oE 'ioctl\([0-9]+, _IOC\([^)]*\)' /tmp/io.txt | sort | uniq -c
```

```
234  _IOC(_IOC_READ|_IOC_WRITE, 0x64, 0x41, 0x68)   ← submit，程序跑 2 次推理
 20  _IOC(_IOC_READ|_IOC_WRITE, 0x64, 0x45, 0x20)   ← MEM_SYNC
  6  _IOC(_IOC_READ|_IOC_WRITE, 0x64, 0x42, 0x30)   ← MEM_CREATE
```

→ 234 / 2 = **每帧 117 次 submit**。

**② 确认是"每帧"而不是累计**

程序里两次 `rknn_run` 之间打标记，探针显示 `task_start` 在第二次 run 时归零：

```
task_start==0 的位置: [0, 117]   → 两段各 117 次
```

**③ 拿官方 eval_perf 的层表对账**

`RKNN_QUERY_PERF_DETAIL`（perf_debug 同源）返回 125 行：

- 3 行 `Target=CPU`：`InputOperator` + 2 个 `OutputOperator` → 不下发给 NPU
- 5 行 `Reshape`，`Time=2 us`（ID 104/105/106/111/113）→ 空操作，不提交
- 剩下 **117 行 NPU 层** = 117 次 ioctl

## 3. 一个 job 里有多个 task

`rknpu_submit.subcore_task[i]` 给出每核的 `(task_start, task_number)`。AUTO 模式下三个核填相同区间，顶层 `task_number` 是三者之和：

```
顶层: ts=0 tn=60 | subcore: (0,20) (0,20) (0,20)
```

所以真实 task 数要用 `subcore_task[0]` 合计：

| 量 | 数值 |
|---|---|
| task 总数 | 769 |
| 平均每层 | 6.6 |
| 中位 / 最大 | 2 / 348 |
| 1~3 个 task 的层 | 70% |

## 4. 换 `core_mask` 不影响提交数

```
AUTO    : 117
CORE_0  : 117
CORE_0_1_2: 117
```

→ **拆分在编译期就定死了**，`core_mask` 只决定 job 落到哪些核。

## 5. 源码位置（rknpu 0.9.6）

路径相对 `rknpu-driver-src/rknpu-0.9.6/`。

| 文件:行 | 内容 |
|---|---|
| `include/rknpu_ioctl.h:262` | `struct rknpu_submit`（104 B，就是探针读的结构） |
| `include/rknpu_ioctl.h:238` | `struct rknpu_subcore_task {task_start, task_number}` |
| `include/rknpu_ioctl.h:219` | `struct rknpu_task`（40 B，硬件执行单元） |
| `rknpu_drv.c:592` | ioctl 注册表 `DRM_IOCTL_DEF_DRV(RKNPU_SUBMIT, ...)` |
| `rknpu_job.c:840` | `rknpu_submit_ioctl()` 入口 |
| `rknpu_job.c:737` | `rknpu_submit()` 主流程 |
| `rknpu_job.c:128` | `rknpu_job_alloc()` —— **job 在这里诞生** |
| `rknpu_job.c:523` | `rknpu_job_schedule()` 入队选核 |
| `rknpu_job.c:271` | `rknpu_job_subcore_commit_pc()` 写寄存器 |
| `rknpu_job.c:361-371` | 真正启动硬件（`PC_TASK_CONTROL` + `PC_OP_EN`） |
| `rknpu_job.c:460` | `rknpu_job_done()` 中断收尾/续跑 |
| `rknpu_job.c:332-336` | 按 `max_submit_number`(4095) 分批 |

## 6. 驱动侧细节

- RK3588 配置：`num_irqs=3`、`max_submit_number=4095`、`core_mask=0x7`（`rknpu_drv.c:140-144`）
- 一次 ioctl 的 task 数超过 4095 时，`job_done` 会继续提交下一批
- 122 个 NPU 层里**只有 68 个有非零 cycles**；54 个是 `0/0/0` 但仍会提交 —— 说明它们是搬运类操作，不走 MAC 计数器

## 7. 未解

`core_mask=7` 时行为不同：`core_mask` 字段出现 `1` 和 `7` 两种值，`subcore_task[0..4]` 全非零，task 总量变成 1570。与 `rknpu_job.c:312-330`（3 核时读 `subcore_task[core_index+2]`）对不上，待查。

## 8. 复现要点

- 模型：`yolov8s-2out.rknn`，输入 1×640×640×3，2 输出
- 程序：`rknn_init(RKNN_FLAG_COLLECT_PERF_MASK)` + 两次 `rknn_run`，输入全 0
- 探针：`LD_PRELOAD` 拦 `ioctl`，只放行 `0xC0686441`，打印前 8 个字（含 `core_mask`/`fence_fd`）和 `subcore_task[5]`
- 板子：RK3588 / 5.10.160-rockchip-rk3588 / rknpu 0.9.6 / librknnrt 2.5.0

## 9. 意义

官方只暴露"层"，驱动只暴露"job"，中间没有文档说明二者关系。这份对账说明：

- `eval_perf` 的 125 行 → 117 个真实 NPU 提交，**8 行是纯记账**
- 想数"跑了多少个 NPU 任务"，官方 API 给不出来，只能靠 ioctl 探针
- 反过来，探针拿不到"层名"，只有 `task_start/task_number`，需要 eval_perf 的表来对齐
