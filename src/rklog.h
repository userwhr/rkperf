#ifndef RKPERF_LOG_H
#define RKPERF_LOG_H

#include <cstdint>
#include <string>

#include "rk_event.h"

// 落盘模块：spdlog 异步日志
//   - 可读日志   -> log_path
//   - 采集事件 CSV   -> csv_path
//   - NPU 使用率 CSV -> npu_path
//   - DDR 带宽 CSV   -> ddr_path
// 采集线程只做 snprintf + 入队，真正的写盘在 spdlog 的工作线程里，
// 队列满时丢弃新日志，不阻塞采集。
namespace rklog {

bool init(const std::string &log_path,
          const std::string &csv_path,
          const std::string &npu_path,
          const std::string &ddr_path);

// 写一条采集事件（热路径，调用方是 perf buffer 回调）
void write_event(const rk_event &e);

// 写一条 NPU 使用率采样（三核百分比）
void write_npu(uint64_t ts_ns, const char *iso, int c0, int c1, int c2);

// 写一条 DDR 带宽采样
void write_ddr(uint64_t ts_ns, const char *iso, int load_pct, unsigned long long freq_hz);

// 工具自身的消息
void info(const std::string &msg);
void warn(const std::string &msg);
void error(const std::string &msg);

// 把队列里剩余日志刷出去
void flush();
void shutdown();

}  // namespace rklog

#endif  // RKPERF_LOG_H
