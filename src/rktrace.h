#ifndef RKPERF_TRACE_H
#define RKPERF_TRACE_H

#include <string>

#include "rkrun.h"

// 采集主流程：找 librknnrt.so -> 解析 rknn_run 的文件偏移 -> 加载内嵌 BPF ->
// fork/exec 目标进程 -> 挂 uprobe -> poll 收事件 -> 收尾。
//
// 返回 0 表示跑完（不代表被测程序成功，退出码在 outcome 里）；
// 返回 1 表示环境问题（找不到库 / 加载 BPF 失败 / 挂不上探针）。
namespace rktrace {

struct options {
    bool        op_time = false;   // --op-time：抓逐层算子耗时
    std::string op_report;         // 逐层报表写到哪（纯文本）
};

int run(char **child_argv, rkrun::outcome *out, const options &opt);

}  // namespace rktrace

#endif  // RKPERF_TRACE_H
