// rkperf.cpp —— 入口：解析命令行、建输出目录、把主流程交给 rktrace
//
// 用法:
//   sudo rkperf [--op-time] <程序> [参数...]
//   sudo rkperf python3 infer.py                     # Python SDK 也可以
//   sudo rkperf --op-time ./infer_demo model.rknn    # 额外采集逐层算子耗时
//
// 代码分工（以前全挤在这个文件里，现在按职责拆开）:
//   rktrace.cpp     找库 / 解析符号 / 加载 BPF / fork-exec / 挂探针 / poll
//   rksample.cpp    NPU、DDR、内存三个采样线程
//   rklog.cpp       CSV 与可读日志
//   rkoplog.cpp     逐层算子耗时（--op-time）
//   rksummary.cpp   summary/ 环境快照与总览
//   rkutil.h        时间戳、hex、输出目录
//
// 输出固定在每次新建的 /tmp/rkperf_<时间戳>/：
//   rkperf.log  events.csv  npu.csv  ddr.csv  mem.csv  summary/
//   开了 --op-time 还有：ops.csv  ops_summary.csv

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "rklog.h"
#include "rkoplog.h"
#include "rkrun.h"
#include "rksummary.h"
#include "rktrace.h"
#include "rkutil.h"

namespace {

void usage(const char *prog)
{
    fprintf(stderr,
            "usage: %s [--op-time] <program> [args...]\n\n"
            "  --op-time   打开 RKNN_LOG_LEVEL=4，采集每个 rknn 算子的耗时\n"
            "              （ops_report.txt 报表 + ops_report_folded.txt 火焰图数据；\n"
            "               会让提交粒度变细、run 时间 +8~11%%）\n"
            "  -h, --help  显示这段帮助\n\n"
            "例: %s --op-time ./infer_demo model.rknn input.jpg\n",
            prog, prog);
}

}  // namespace

int main(int argc, char **argv)
{
    /* 选项必须在程序名之前：程序名之后的参数全部交给被测程序 */
    int  first = 1;
    bool op_time = false;
    for (; first < argc; ++first) {
        const char *a = argv[first];
        if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            usage(argv[0]);
            return 0;
        }
        if (strcmp(a, "--op-time") == 0) {
            op_time = true;
        } else if (strcmp(a, "--") == 0) {
            ++first;
            break;
        } else if (a[0] == '-' && a[1] != '\0') {
            fprintf(stderr, "unknown option: %s\n", a);
            usage(argv[0]);
            return 1;
        } else {
            break;
        }
    }
    if (first >= argc) {
        usage(argv[0]);
        return 1;
    }
    char **child_argv = &argv[first];

    const std::string dir = rkutil::make_output_dir();
    if (dir.empty())
        return 1;

    /* summary：环境快照要在拉起目标进程之前做完 */
    rksummary::run_info ri;
    ri.output_dir = dir;
    ri.program    = child_argv[0];
    for (int i = 0; i < argc; ++i)
        ri.argv.push_back(argv[i]);
    const bool summary_on = rksummary::begin(ri);

    if (!rklog::init(dir + "/rkperf.log", dir + "/events.csv",
                     dir + "/npu.csv", dir + "/ddr.csv", dir + "/mem.csv"))
        return 1;

    rklog::info(std::string("rkperf ") + rksummary::version() +
                " start: target=" + child_argv[0]);
    rklog::info("output dir: " + dir);
    if (summary_on)
        rklog::info("summary: " + dir + "/summary/OVERVIEW.md");
    else
        rklog::warn("summary disabled (RKPERF_NO_SUMMARY)");

    rktrace::options opt;
    opt.op_time   = op_time;
    opt.op_report = dir + "/ops_report.txt";

    if (op_time) {
        if (rkoplog::setup(opt.op_report)) {
            rklog::info("op-time: on -> " + opt.op_report);
            rklog::info("op-time: RKNN_LOG_LEVEL=4（若原值更低），提交粒度会变细，"
                        "run 时间预计 +8~11%");
        } else {
            rklog::warn("op-time: 初始化失败，退回普通采集");
            opt.op_time = false;
        }
    }

    rkrun::outcome oc;
    oc.ended_at    = rkutil::iso_now();
    oc.exit_reason = "not started";

    int rc = rktrace::run(child_argv, &oc, opt);

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
