#ifndef RKPERF_LOG_H
#define RKPERF_LOG_H

#include <string>

#include "rk_event.h"

// 落盘模块：spdlog 异步日志
//   - 人类可读日志 -> log_path
//   - CSV 数据行   -> csv_path
// 采集线程只做 snprintf + 入队，真正的写盘在 spdlog 的工作线程里，
// 队列满时丢弃新日志，不阻塞采集。
namespace rklog {

// 初始化。成功返回 true。
bool init(const std::string &log_path, const std::string &csv_path);

// 写一条采集事件（热路径，调用方是 perf buffer 回调）
void write_event(const rk_event &e);

// 工具自身的消息
void info(const std::string &msg);
void warn(const std::string &msg);
void error(const std::string &msg);

// 把队列里剩余日志刷出去
void flush();
void shutdown();

}  // namespace rklog

#endif  // RKPERF_LOG_H
