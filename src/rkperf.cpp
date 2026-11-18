// rkperf.cpp —— 入口：解析命令行、建输出目录、把主流程交给 rktrace
//
// 用法:
//   sudo rkperf <程序> [参数...]
//   sudo rkperf python3 infer.py            # Python SDK 也可以
//
// 代码分工（以前全挤在这个文件里，现在按职责拆开）:
//   rktrace.cpp     找库 / 解析符号 / 加载 BPF / fork-exec / 挂探针 / poll
//   rksample.cpp    NPU、DDR、内存三个采样线程
//   rklog.cpp       CSV 与可读日志
//   rksummary.cpp   summary/ 环境快照与总览
//   rkutil.h        时间戳、hex、输出目录
//
// 输出固定在每次新建的 /tmp/rkperf_<时间戳>/：
//   rkperf.log  events.csv  npu.csv  ddr.csv  mem.csv  summary/

#include <cstdio>
#include <string>
#include <vector>

#include "rklog.h"
#include "rkrun.h"
#include "rksummary.h"
#include "rktrace.h"
#include "rkutil.h"

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <program> [args...]\n", argv[0]);
        return 1;
    }

    const std::string dir = rkutil::make_output_dir();
    if (dir.empty())
        return 1;

    /* summary：环境快照要在拉起目标进程之前做完 */
    rksummary::run_info ri;
    ri.output_dir = dir;
    ri.program    = argv[1];
    for (int i = 0; i < argc; ++i)
        ri.argv.push_back(argv[i]);
    const bool summary_on = rksummary::begin(ri);

    if (!rklog::init(dir + "/rkperf.log", dir + "/events.csv",
                     dir + "/npu.csv", dir + "/ddr.csv", dir + "/mem.csv"))
        return 1;

    rklog::info(std::string("rkperf ") + rksummary::version() +
                " start: target=" + argv[1]);
    rklog::info("output dir: " + dir);
    if (summary_on)
        rklog::info("summary: " + dir + "/summary/OVERVIEW.md");
    else
        rklog::warn("summary disabled (RKPERF_NO_SUMMARY)");

    rkrun::outcome oc;
    oc.ended_at    = rkutil::iso_now();
    oc.exit_reason = "not started";

    int rc = rktrace::run(&argv[1], &oc);

    if (oc.exit_reason == "not started" && rc != 0) {
        /* 没走到采集循环就退出了（找不到库 / 加载 BPF 失败），也给总览一个说法 */
        oc.ended_at    = rkutil::iso_now();
        oc.exit_reason = "aborted";
        oc.exit_code   = rc;
    }

    rksummary::finish(oc);

    if (summary_on)
        rklog::info("summary written: " + dir + "/summary/OVERVIEW.md");

    rklog::info("output: " + dir);
    rklog::flush();
    rklog::shutdown();
    return rc;
}
