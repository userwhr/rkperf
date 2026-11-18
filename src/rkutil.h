#ifndef RKPERF_UTIL_H
#define RKPERF_UTIL_H

// 几个 .cpp 都要用的小工具：单调钟、ISO 时间、hex、输出目录。
// 全部 inline，不再为它单独引入一个编译单元。

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>

#include <sys/stat.h>
#include <sys/types.h>

namespace rkutil {

constexpr const char *kOutRoot = "/tmp";   // 每次运行在这下面建 rkperf_<时间戳>

inline double mono_seconds()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

inline std::string iso_now()
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);

    char base[32], zone[8], out[64];
    strftime(base, sizeof(base), "%Y-%m-%dT%H:%M:%S", &tm);
    strftime(zone, sizeof(zone), "%z", &tm);
    snprintf(out, sizeof(out), "%s.%03ld%s", base, ts.tv_nsec / 1000000L, zone);
    return std::string(out);
}

inline std::string hex_u64(unsigned long long v)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "0x%llx", v);
    return std::string(buf);
}

// 建 /tmp/rkperf_YYYYmmdd_HHMMSS；同一秒内重复调用会复用同一个目录
inline std::string make_output_dir()
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);

    char name[64];
    strftime(name, sizeof(name), "rkperf_%Y%m%d_%H%M%S", &tm);

    const std::string dir = std::string(kOutRoot) + "/" + name;
    if (mkdir(dir.c_str(), 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "mkdir %s: %s\n", dir.c_str(), strerror(errno));
        return std::string();
    }
    return dir;
}

}  // namespace rkutil

#endif  // RKPERF_UTIL_H
