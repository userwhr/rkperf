#ifndef RKPERF_EVENT_H
#define RKPERF_EVENT_H

#include <cstdint>

// 与 BPF 侧 struct rk_event 的布局必须完全一致，字段顺序不能改。
struct rk_event {
    uint64_t ts_in;      // 入口时间戳 (ns)
    uint64_t dur_ns;     // 耗时 (ns)
    uint64_t ctx;        // rknn_context
    uint32_t pid;
    uint32_t tid;
    int32_t  ret;        // rknn_run 返回值
    int32_t  non_block;  // 1 = 异步调用
    char     comm[16];
};

static_assert(sizeof(rk_event) == 56, "rk_event 布局与 BPF 侧不一致");

#endif  // RKPERF_EVENT_H
