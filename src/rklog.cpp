#include "rklog.h"

#include <cstdio>
#include <memory>
#include <vector>

#include <spdlog/spdlog.h>
#include <spdlog/async.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/rotating_file_sink.h>

namespace {

constexpr size_t kQueueSize = 8192;
constexpr size_t kMaxFileSize = 16 * 1024 * 1024;
constexpr int    kMaxFiles = 3;

std::shared_ptr<spdlog::logger> g_log;   // 人类可读
std::shared_ptr<spdlog::logger> g_csv;   // 采集事件
std::shared_ptr<spdlog::logger> g_npu;   // NPU 使用率

}  // namespace

namespace rklog {

bool init(const std::string &log_path,
          const std::string &csv_path,
          const std::string &npu_path)
{
    try {
        spdlog::init_thread_pool(kQueueSize, 1);

        auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            log_path, kMaxFileSize, kMaxFiles);
        auto err_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        err_sink->set_level(spdlog::level::warn);

        std::vector<spdlog::sink_ptr> sinks{file_sink, err_sink};
        g_log = std::make_shared<spdlog::async_logger>(
            "rkperf", sinks.begin(), sinks.end(),
            spdlog::thread_pool(), spdlog::async_overflow_policy::discard_new);
        g_log->set_level(spdlog::level::info);
        g_log->set_pattern("[%Y-%m-%dT%H:%M:%S.%e%z] [%l] %v");
        g_log->flush_on(spdlog::level::warn);

        auto csv_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            csv_path, kMaxFileSize, kMaxFiles);
        g_csv = std::make_shared<spdlog::async_logger>(
            "csv", csv_sink, spdlog::thread_pool(),
            spdlog::async_overflow_policy::discard_new);
        g_csv->set_level(spdlog::level::info);
        g_csv->set_pattern("%v");
        g_csv->info("# ts_in_ns,dur_ns,dur_ms,pid,tid,ctx,ret,non_block,core_mask,comm");

        auto npu_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            npu_path, kMaxFileSize, kMaxFiles);
        g_npu = std::make_shared<spdlog::async_logger>(
            "npu", npu_sink, spdlog::thread_pool(),
            spdlog::async_overflow_policy::discard_new);
        g_npu->set_level(spdlog::level::info);
        g_npu->set_pattern("%v");
        g_npu->info("# ts_ns,time,core0,core1,core2");
        return true;
    } catch (const spdlog::spdlog_ex &ex) {
        fprintf(stderr, "spdlog init failed: %s\n", ex.what());
        return false;
    }
}

void write_event(const rk_event &e)
{
    // 热路径：只做一次 snprintf 然后入队，落盘交给 spdlog 线程
    char buf[256];
    int n = snprintf(buf, sizeof(buf), "%llu,%llu,%.3f,%u,%u,0x%llx,%d,%d,%u,%s",
                     (unsigned long long)e.ts_in,
                     (unsigned long long)e.dur_ns,
                     e.dur_ns / 1000000.0,
                     e.pid, e.tid,
                     (unsigned long long)e.ctx,
                     e.ret, e.non_block, e.core_mask, e.comm);
    if (n > 0)
        g_csv->log(spdlog::level::info, spdlog::string_view_t(buf, (size_t)n));

    g_log->info("rknn_run pid={} tid={} ctx=0x{:x} {} core={} dur={:.3f} ms ret={} comm={}",
                e.pid, e.tid, e.ctx, e.non_block ? "ASYNC" : "SYNC ",
                core_mask_name(e.core_mask), e.dur_ns / 1000000.0, e.ret, e.comm);
}

void write_npu(uint64_t ts_ns, const char *iso, int c0, int c1, int c2)
{
    char buf[160];
    int n = snprintf(buf, sizeof(buf), "%llu,%s,%d,%d,%d",
                     (unsigned long long)ts_ns, iso, c0, c1, c2);
    if (n > 0)
        g_npu->log(spdlog::level::info, spdlog::string_view_t(buf, (size_t)n));
}

void info(const std::string &msg)  { if (g_log) g_log->info(msg); }
void warn(const std::string &msg)  { if (g_log) g_log->warn(msg); }
void error(const std::string &msg) { if (g_log) g_log->error(msg); }

void flush()
{
    if (g_log) g_log->flush();
    if (g_csv) g_csv->flush();
    if (g_npu) g_npu->flush();
}

void shutdown() { spdlog::shutdown(); }

}  // namespace rklog
