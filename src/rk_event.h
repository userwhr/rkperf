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
    uint32_t core_mask;  // 实际使用的 NPU 核（驱动回写）
    uint32_t pad;
    char     comm[16];
};

static_assert(sizeof(rk_event) == 64, "rk_event 布局与 BPF 侧不一致");

// core_mask -> 可读名字
inline const char *core_mask_name(uint32_t mask)
{
    switch (mask) {
    case 0x0: return "auto";
    case 0x1: return "core0";
    case 0x2: return "core1";
    case 0x4: return "core2";
    case 0x3: return "core0_1";
    case 0x5: return "core0_2";
    case 0x6: return "core1_2";
    case 0x7: return "core0_1_2";
    default:  return "unknown";
    }
}

#endif  // RKPERF_EVENT_H
