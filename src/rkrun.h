#ifndef RKPERF_RUN_H
#define RKPERF_RUN_H

#include <string>

// 一次采集的结果：rktrace 填，rksummary 拿去写 OVERVIEW.md / stats.json。
// 单独放一个头，是为了让 rktrace 和 rksummary 互不依赖。
namespace rkrun {

struct outcome {
    std::string ended_at;
    double      duration_s = 0.0;
    std::string exit_reason;      // exited / signal / aborted / not started
    int         exit_code = -1;
    bool        interrupted = false;
};

}  // namespace rkrun

#endif  // RKPERF_RUN_H
