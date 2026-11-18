#ifndef RKPERF_SUMMARY_H
#define RKPERF_SUMMARY_H

#include <cstdint>
#include <string>
#include <vector>

#include "rk_event.h"
#include "rkrun.h"

// summary/ 目录 —— 一次采集的自描述，nxperf 的 summary/ 对应物。
//
//   summary/config.json        这次怎么采的（目标、探针、参数、平台）
//   summary/OVERVIEW.md        总览，采集结束时生成（人读）
//   summary/stats.json         同一份统计的机器可读版（喂 pandas / 报告）
//   summary/uname.txt ...      环境快照，采集开始时各跑一条命令
//   summary/proc/<pid>/        目标进程的 /proc 快照
//
// 采集期只累积统计量，落盘在 finish() 里做；
// 设 RKPERF_NO_SUMMARY=1 可整体关掉。
namespace rksummary {

// 工具版本，写进 config.json / OVERVIEW.md / 主日志
const char *version();

struct run_info {
    std::string output_dir;            // /tmp/rkperf_YYYYmmdd_HHMMSS
    std::string program;               // argv[1]
    std::vector<std::string> argv;     // 完整命令行
    std::string started_at;            // 空则自动取当前时间
};

struct probe_info {
    std::string library;
    std::string symbol;                // rknn_run / rknn_set_core_mask / ioctl
    unsigned long long offset = 0;
};

// 建 summary/、跑环境快照、写 config.json。返回 false 表示本次不记 summary
// （被 RKPERF_NO_SUMMARY 关掉，或目录建不出来）。
bool begin(const run_info &info);

// 子进程 fork 出来之后调用：补 pid，并抓 /proc/<pid> 快照
void attach_child(int pid);

// 探针挂完之后调用：把探针信息补进 config.json
void set_probes(const std::vector<probe_info> &probes);

// 采集期累积（轻量，只加锁做一次 push/加法）
void note_event(const rk_event &e);
void note_npu(int c0, int c1, int c2);
void note_ddr(int load_pct, unsigned long long freq_hz);
void note_mem(long rss_kb, long hwm_kb, int threads);

// 采集结束：写 OVERVIEW.md + stats.json
void finish(const rkrun::outcome &o);

}  // namespace rksummary

#endif  // RKPERF_SUMMARY_H
