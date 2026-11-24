#ifndef RKPERF_OPLOG_H
#define RKPERF_OPLOG_H

#include <string>

// 逐层算子耗时采集（--op-time）
//
// 原理：librknnrt 在 RKNN_LOG_LEVEL>=4 时，每次 rknn_run 结束会把
// "Network Layer Information Table" 打到 **stdout**，每层一行：
//   ID  OpType  DataType  Target  InputShape  OutputShape
//   Cycles(DDR/NPU/Total)  Time(us)  MacUsage  WorkLoad  RW(KB)  FullName
//
// 做法：把子进程的 stdout 接到管道上，读线程一边原样转发到 rkperf 的 stdout，
// 一边按状态机切出每张表，追加到纯文本报表 ops_report.txt：
// 每帧一张层表 + 该帧合计，末尾一张跨帧的 Operator Time Consuming Ranking。
//
// 副作用（必须告诉用户）：开了这个模式，runtime 会按层提交，ioctl 从 63 次/帧
// 变成 117 次/帧，run 时间会涨 8~11%，而且 RKNN_LOG_LEVEL 本身有打印开销。
namespace rkoplog {

// 建管道、打开报表。返回 false 表示不可用（子进程 stdout 保持原样）。
bool setup(const std::string &report_path);

// 子进程 fork 之后、exec 之前调用：拿到要 dup2 到 STDOUT_FILENO 的 fd。
int child_fd();

// 父进程：关掉写端、起读线程。必须在放行子进程（SIGCONT）之前调用。
void start();

// 收尾：停读线程、写统计、关 csv。可以重复调用。
void finish();

bool enabled();

}  // namespace rkoplog

#endif  // RKPERF_OPLOG_H
