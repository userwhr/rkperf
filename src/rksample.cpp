// rksample.cpp —— NPU / DDR / 内存三个采样线程
//
// 只读 sysfs / procfs，不碰 BPF，所以从 rktrace 里拆出来单独成编译单元。
// 三个线程各自 sleep 轮询；所有退出点都检查 stop_，stop() 之后最多等一个采样周期。

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "rksample.h"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <unistd.h>

#include "rklog.h"
#include "rksummary.h"
#include "rkutil.h"

using rkutil::iso_now;
using rkutil::mono_seconds;

namespace {

constexpr const char *kNpuLoad = "/sys/kernel/debug/rknpu/load";
constexpr const char *kDdrLoad = "/sys/class/devfreq/dmc/load";

/* 解析 "NPU load:  Core0: 45%, Core1:  0%, Core2:  0%," */
bool parse_npu_load(const char *buf, int v[3])
{
    const char *p = buf;
    for (int i = 0; i < 3; i++) {
        char key[16];
        snprintf(key, sizeof(key), "Core%d:", i);
        const char *q = strstr(p, key);
        if (!q) return false;
        q += strlen(key);
        v[i] = atoi(q);
        p = q;
    }
    return true;
}

/* /sys/class/devfreq/dmc/load 的格式: "12@528000000Hz" */
bool read_ddr_load(int *pct, unsigned long long *hz)
{
    FILE *f = fopen(kDdrLoad, "r");
    if (!f) return false;
    char buf[64] = {0};
    bool ok = fgets(buf, sizeof(buf), f) != nullptr;
    fclose(f);
    if (!ok) return false;
    return sscanf(buf, "%d@%lluHz", pct, hz) == 2;
}

}  // namespace

namespace rksample {

void Samplers::start(pid_t pid)
{
    if (npu_.joinable() || ddr_.joinable() || mem_.joinable())
        return;                        // 已经在跑
    stop_ = false;
    npu_ = std::thread(&Samplers::loop_npu, this);
    ddr_ = std::thread(&Samplers::loop_ddr, this);
    mem_ = std::thread(&Samplers::loop_mem, this, pid);
}

void Samplers::stop()
{
    stop_ = true;
    if (npu_.joinable()) npu_.join();
    if (ddr_.joinable()) ddr_.join();
    if (mem_.joinable()) mem_.join();
}

/* 每秒读一次：驱动统计窗口就是 1 秒，采快了没有意义 */
void Samplers::loop_npu()
{
    bool warned = false;
    while (!stop_.load()) {
        FILE *f = fopen(kNpuLoad, "r");
        if (!f) {
            if (!warned) {
                rklog::warn(std::string("npu sampler: 打不开 ") + kNpuLoad +
                            " (" + strerror(errno) + ")，需要 root");
                warned = true;
            }
        } else {
            char buf[256] = {0};
            if (fgets(buf, sizeof(buf), f)) {
                int v[3];
                if (parse_npu_load(buf, v)) {
                    rklog::write_npu((uint64_t)(mono_seconds() * 1e9),
                                     iso_now().c_str(), v[0], v[1], v[2]);
                    rksummary::note_npu(v[0], v[1], v[2]);
                }
            }
            fclose(f);
        }
        for (int i = 0; i < 10 && !stop_.load(); i++)
            usleep(100000);            /* 10 × 100ms = 1s */
    }
}

/* 每 100ms 采一次；频点变化时往主日志里记一条 */
void Samplers::loop_ddr()
{
    bool warned = false;
    unsigned long long last_hz = 0;

    while (!stop_.load()) {
        int pct = 0;
        unsigned long long hz = 0;
        if (!read_ddr_load(&pct, &hz)) {
            if (!warned) {
                rklog::warn(std::string("ddr sampler: 打不开 ") + kDdrLoad + "，跳过");
                warned = true;
            }
        } else {
            rklog::write_ddr((uint64_t)(mono_seconds() * 1e9), iso_now().c_str(), pct, hz);
            rksummary::note_ddr(pct, hz);

            if (last_hz && hz != last_hz) {
                char msg[128];
                snprintf(msg, sizeof(msg), "ddr freq: %llu MHz -> %llu MHz (%s)",
                         last_hz / 1000000ULL, hz / 1000000ULL,
                         hz > last_hz ? "up" : "down");
                rklog::info(msg);
            }
            last_hz = hz;
        }
        for (int i = 0; i < 10 && !stop_.load(); i++)
            usleep(10000);             /* 10 × 10ms = 100ms */
    }
}

/* 每 5 s 读一次 /proc/<pid>/status：VmRSS / VmHWM / Threads */
void Samplers::loop_mem(pid_t pid)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/status", (int)pid);

    const double t0 = mono_seconds();
    bool warned = false;
    while (!stop_.load()) {
        FILE *f = fopen(path, "r");
        if (!f) {
            if (!warned) {
                rklog::warn(std::string("mem sampler: 打不开 ") + path + "，跳过");
                warned = true;
            }
        } else {
            char line[256];
            long rss = 0, hwm = 0;
            int threads = 0;
            while (fgets(line, sizeof(line), f)) {
                if (strncmp(line, "VmRSS:", 6) == 0)        rss = atol(line + 6);
                else if (strncmp(line, "VmHWM:", 6) == 0)   hwm = atol(line + 6);
                else if (strncmp(line, "Threads:", 8) == 0) threads = (int)atol(line + 8);
            }
            fclose(f);
            if (rss > 0 || threads > 0) {
                // exec 之前采到的是 fork 副本（RSS 只有几 KB），不是目标进程，跳过
                const bool early = (mono_seconds() - t0) < 2.0;
                if (!(early && rss < 1024)) {
                    rklog::write_mem((uint64_t)(mono_seconds() * 1e9), iso_now().c_str(),
                                     rss, hwm, threads);
                    rksummary::note_mem(rss, hwm, threads);
                }
            }
        }
        for (int i = 0; i < 50 && !stop_.load(); i++)
            usleep(100000);            /* 50 × 100ms = 5s */
    }
}

}  // namespace rksample
