// rksummary.cpp —— summary/ 目录：环境快照 + 配置自描述 + 一次采集的总览
//
// 设计参照 nxperf 的 summary/：每份采集自带"当时是什么环境、这次怎么采的"，
// 事后不用再问"这是在哪块板子、什么版本上跑的"。
//
// 采集期只累积整数/向量，落盘全部在 finish() 里做，不干扰热路径。

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "rksummary.h"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <mutex>
#include <numeric>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace {

const char *kVersion = "0.2.0";
const size_t kMaxDurSamples = 4000000;   // 约 32 MB，够一分钟高频率采集

// ---------------- 小工具 ----------------

std::string now_iso()
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

std::string host_name()
{
    char buf[256] = {0};
    if (gethostname(buf, sizeof(buf) - 1) != 0)
        return std::string("unknown");
    return std::string(buf);
}

bool mkdir_p(const std::string &path)
{
    if (path.empty())
        return false;

    std::string cur;
    for (size_t i = 0; i < path.size(); ++i) {
        cur.push_back(path[i]);
        if (path[i] == '/' && cur.size() > 1) {
            if (mkdir(cur.c_str(), 0755) != 0 && errno != EEXIST)
                return false;
        }
    }
    if (mkdir(path.c_str(), 0755) != 0 && errno != EEXIST)
        return false;
    return true;
}

bool write_file(const std::string &path, const std::string &content)
{
    std::ofstream f(path, std::ios::binary);
    if (!f)
        return false;
    f.write(content.data(), (std::streamsize)content.size());
    return f.good();
}

bool read_file(const std::string &path, std::string &out)
{
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

// 跑一条 shell 命令并把 stdout+stderr 收回来
std::string shell_capture(const std::string &cmd)
{
    FILE *p = popen((cmd + " 2>&1").c_str(), "r");
    if (!p)
        return std::string("(popen failed)\n");

    std::string out;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), p)) > 0)
        out.append(buf, n);
    pclose(p);
    if (out.empty())
        out = "(no output)\n";
    return out;
}

void capture_to(const std::string &dir, const std::string &name,
                const std::string &desc, const std::string &cmd)
{
    std::ostringstream ss;
    ss << "# rkperf summary / " << desc << "\n"
       << "# $ " << cmd << "\n\n"
       << shell_capture(cmd);
    write_file(dir + "/" + name, ss.str());
}

std::string json_esc(const std::string &s)
{
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
        default:
            if ((unsigned char)c < 0x20) {
                char esc[8];
                snprintf(esc, sizeof(esc), "\\u%04x", (unsigned char)c);
                out += esc;
            } else {
                out += c;
            }
        }
    }
    return out;
}

std::string hex_u64(unsigned long long v)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "0x%llx", v);
    return std::string(buf);
}

std::string f3(double v)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%.3f", v);
    return std::string(buf);
}

std::string f2(double v)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%.2f", v);
    return std::string(buf);
}

std::string p1(double v)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%.1f%%", v);
    return std::string(buf);
}

// 线性插值分位数，p ∈ [0, 100]；输入必须已排序
template <typename T>
double pctl(const std::vector<T> &sorted, double p)
{
    if (sorted.empty())
        return 0.0;
    if (p <= 0.0)
        return (double)sorted.front();
    if (p >= 100.0)
        return (double)sorted.back();

    const double pos = p / 100.0 * (double)(sorted.size() - 1);
    const size_t lo = (size_t)pos;
    const size_t hi = (lo + 1 < sorted.size()) ? lo + 1 : lo;
    const double frac = pos - (double)lo;
    return (double)sorted[lo] * (1.0 - frac) + (double)sorted[hi] * frac;
}

template <typename T>
std::vector<T> sorted_copy(const std::vector<T> &v)
{
    std::vector<T> c = v;
    std::sort(c.begin(), c.end());
    return c;
}

}  // namespace

namespace rksummary {

const char *version() { return kVersion; }

namespace {

struct mem_sample {
    long rss = 0;
    long hwm = 0;
    int  threads = 0;
};

std::mutex g_mu;

bool        g_on = false;
bool        g_truncated = false;
std::string g_root;          // /tmp/rkperf_xxx
std::string g_dir;           // /tmp/rkperf_xxx/summary
std::string g_program;
std::string g_started;
std::string g_uname;         // uname -srm
std::vector<std::string>  g_argv;
std::vector<probe_info>   g_probes;
int         g_child = -1;

std::vector<uint64_t> g_dur;      // 每次 rknn_run 的耗时（ns）
uint64_t g_ev = 0;
uint64_t g_sync = 0;
uint64_t g_async = 0;
uint64_t g_fail = 0;
uint64_t g_sum = 0;
uint64_t g_min = 0;
uint64_t g_max = 0;

uint64_t g_core_cnt[8] = {0};
uint64_t g_core_sum[8] = {0};

std::vector<int> g_npu[3];
std::vector<int> g_ddr_load;
std::vector<unsigned long long> g_ddr_freq;
std::vector<mem_sample> g_mem;

std::string trim(const std::string &s)
{
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos)
        return std::string();
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

// ---------------- 环境快照 ----------------

void snapshot_env()
{
    capture_to(g_dir, "uname.txt", "内核与主机", "uname -a");
    capture_to(g_dir, "lscpu.txt", "CPU 拓扑与频率上限",
               "command -v lscpu >/dev/null 2>&1 && lscpu || cat /proc/cpuinfo");
    capture_to(g_dir, "mount.txt", "挂载表（找 NFS / IO 路径）", "mount");
    capture_to(g_dir, "df.txt", "文件系统用量", "df -h");
    capture_to(g_dir, "net.txt", "网卡与流量计数",
               "command -v ip >/dev/null 2>&1 && ip -s link || cat /proc/net/dev");
    capture_to(g_dir, "board.txt", "系统/内核版本",
               "cat /etc/os-release 2>/dev/null; echo; cat /proc/version; echo; "
               "cat /proc/cmdline; echo; head -5 /proc/meminfo");
    capture_to(g_dir, "npu.txt", "NPU 驱动、频点与拓扑",
               "ls -l /sys/kernel/debug/rknpu/ 2>/dev/null; "
               "for f in version load freq; do "
               "  [ -r /sys/kernel/debug/rknpu/$f ] && { echo \"--- $f\"; cat /sys/kernel/debug/rknpu/$f; }; "
               "done; "
               "for d in /sys/class/devfreq/*npu*; do "
               "  [ -d \"$d\" ] || continue; echo \"--- $d\"; "
               "  cat \"$d/cur_freq\" \"$d/governor\" 2>/dev/null; "
               "  cat \"$d/available_frequencies\" 2>/dev/null; "
               "done; "
               "echo '--- dmesg (rknpu)'; "
               "dmesg 2>/dev/null | grep -i -E 'rknpu|npu' | tail -20");
    capture_to(g_dir, "libs.txt", "rknn 运行库",
               "ls -l /usr/lib/librknnrt.so /usr/local/lib/librknnrt.so "
               "/usr/lib/aarch64-linux-gnu/librknnrt.so 2>/dev/null; "
               "echo; ls -l /usr/lib/librknn_api/ 2>/dev/null");

    g_uname = trim(shell_capture("uname -srm"));
}

void snapshot_proc(int pid)
{
    if (pid <= 0)
        return;

    const std::string base = "/proc/" + std::to_string(pid);
    const std::string dst = g_dir + "/proc/" + std::to_string(pid);
    mkdir_p(g_dir + "/proc");
    mkdir_p(dst);

    const char *files[] = {"status", "smaps_rollup", "limits", "cgroup",
                           "stat", "schedstat", "io", "maps"};
    for (const char *name : files) {
        std::string content;
        if (!read_file(base + "/" + name, content))
            continue;
        write_file(dst + "/" + name, content);
    }

    // cmdline 要单独读：里面是 '\0' 分隔的参数
    std::string cl;
    if (read_file(base + "/cmdline", cl)) {
        for (char &c : cl)
            if (c == '\0') c = ' ';
        write_file(dst + "/cmdline", cl + "\n");
    }

    // 线程清单（每行一个 tid -> comm）
    std::string tasks = shell_capture(
        "for t in /proc/" + std::to_string(pid) + "/task/*; do "
        "  [ -r \"$t/comm\" ] && echo \"$(basename $t) $(cat $t/comm)\"; "
        "done");
    write_file(dst + "/tasks.txt",
               "# tid comm\n" + tasks);
}

// ---------------- config.json ----------------

void write_config()
{
    std::ostringstream js;
    js << "{\n";
    js << "  \"tool\": \"rkperf\",\n";
    js << "  \"version\": \"" << kVersion << "\",\n";
    js << "  \"generated_at\": \"" << json_esc(now_iso()) << "\",\n";
    js << "  \"started_at\": \"" << json_esc(g_started) << "\",\n";
    js << "  \"output_dir\": \"" << json_esc(g_root) << "\",\n";
    js << "  \"host\": \"" << json_esc(host_name()) << "\",\n";
    js << "  \"platform\": \"" << json_esc(g_uname) << "\",\n";

    js << "  \"target\": {\"program\": \"" << json_esc(g_program) << "\", ";
    if (g_child > 0)
        js << "\"pid\": " << g_child << ", ";
    else
        js << "\"pid\": null, ";
    js << "\"argv\": [";
    for (size_t i = 0; i < g_argv.size(); ++i) {
        if (i) js << ", ";
        js << "\"" << json_esc(g_argv[i]) << "\"";
    }
    js << "]},\n";

    js << "  \"probes\": [\n";
    for (size_t i = 0; i < g_probes.size(); ++i) {
        js << "    {\"library\": \"" << json_esc(g_probes[i].library)
           << "\", \"symbol\": \"" << json_esc(g_probes[i].symbol)
           << "\", \"offset\": \"" << hex_u64(g_probes[i].offset) << "\"}";
        js << (i + 1 == g_probes.size() ? "\n" : ",\n");
    }
    js << "  ],\n";
    js << "  \"sampling\": {\"npu_hz\": 1, \"ddr_ms\": 100, \"mem_ms\": 5000},\n";
    js << "  \"outputs\": [\"rkperf.log\", \"events.csv\", \"npu.csv\", \"ddr.csv\", \"mem.csv\", "
          "\"summary/\"]\n";
    js << "}\n";

    write_file(g_dir + "/config.json", js.str());
}

}  // namespace

// ============================================================
// 对外接口
// ============================================================

bool begin(const run_info &info)
{
    const char *off = getenv("RKPERF_NO_SUMMARY");
    if (off && off[0] && strcmp(off, "0") != 0)
        return false;

    g_root = info.output_dir;
    g_dir = g_root + "/summary";
    if (!mkdir_p(g_dir))
        return false;

    g_program = info.program;
    g_argv = info.argv;
    g_started = info.started_at.empty() ? now_iso() : info.started_at;

    snapshot_env();
    write_config();

    g_on = true;
    return true;
}

void attach_child(int pid)
{
    if (!g_on)
        return;
    std::lock_guard<std::mutex> lk(g_mu);
    g_child = pid;
    snapshot_proc(pid);
    write_config();
}

void set_probes(const std::vector<probe_info> &probes)
{
    if (!g_on)
        return;
    std::lock_guard<std::mutex> lk(g_mu);
    g_probes = probes;
    write_config();
}

void note_event(const rk_event &e)
{
    if (!g_on)
        return;

    std::lock_guard<std::mutex> lk(g_mu);
    ++g_ev;
    if (e.non_block)
        ++g_async;
    else
        ++g_sync;
    if (e.ret != 0)
        ++g_fail;

    g_sum += e.dur_ns;
    if (g_ev == 1 || e.dur_ns < g_min) g_min = e.dur_ns;
    if (e.dur_ns > g_max) g_max = e.dur_ns;

    if (g_dur.size() < kMaxDurSamples)
        g_dur.push_back(e.dur_ns);
    else
        g_truncated = true;

    const uint32_t mask = e.core_mask & 0x7u;
    g_core_cnt[mask] += 1;
    g_core_sum[mask] += e.dur_ns;
}

void note_npu(int c0, int c1, int c2)
{
    if (!g_on)
        return;
    std::lock_guard<std::mutex> lk(g_mu);
    g_npu[0].push_back(c0);
    g_npu[1].push_back(c1);
    g_npu[2].push_back(c2);
}

void note_ddr(int load_pct, unsigned long long freq_hz)
{
    if (!g_on)
        return;
    std::lock_guard<std::mutex> lk(g_mu);
    g_ddr_load.push_back(load_pct);
    g_ddr_freq.push_back(freq_hz);
}

void note_mem(long rss_kb, long hwm_kb, int threads)
{
    if (!g_on)
        return;
    std::lock_guard<std::mutex> lk(g_mu);
    mem_sample s;
    s.rss = rss_kb;
    s.hwm = hwm_kb;
    s.threads = threads;
    g_mem.push_back(s);
}

// ---------------- 总览 ----------------

void finish(const rkrun::outcome &o)
{
    if (!g_on)
        return;

    // 先把统计量拷出来，落盘期间不再占锁
    std::vector<uint64_t> dur;
    std::vector<int> npu[3];
    std::vector<int> ddr_load;
    std::vector<unsigned long long> ddr_freq;
    std::vector<mem_sample> mem;
    uint64_t ev, nsync, nasync, nfail, sum, vmin, vmax;
    uint64_t core_cnt[8], core_sum[8];
    bool truncated;
    std::vector<probe_info> probes;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        dur = g_dur;
        for (int i = 0; i < 3; ++i) npu[i] = g_npu[i];
        ddr_load = g_ddr_load;
        ddr_freq = g_ddr_freq;
        mem = g_mem;
        ev = g_ev; nsync = g_sync; nasync = g_async; nfail = g_fail;
        sum = g_sum; vmin = g_min; vmax = g_max;
        for (int i = 0; i < 8; ++i) { core_cnt[i] = g_core_cnt[i]; core_sum[i] = g_core_sum[i]; }
        truncated = g_truncated;
        probes = g_probes;
    }

    const std::vector<uint64_t> sdur = sorted_copy(dur);
    const double mean_ns = ev ? (double)sum / (double)ev : 0.0;
    const double med_ns = pctl(sdur, 50.0);
    const double p90_ns = pctl(sdur, 90.0);
    const double p99_ns = pctl(sdur, 99.0);
    const double conc = (o.duration_s > 0.0) ? (double)sum / (o.duration_s * 1e9) : 0.0;

    // ---- NPU / DDR / 内存 的派生量 ----
    struct core_stat { double mean = 0, p90 = 0, mx = 0, ge90 = 0; size_t n = 0; };
    core_stat ns[3];
    for (int c = 0; c < 3; ++c) {
        ns[c].n = npu[c].size();
        if (npu[c].empty())
            continue;
        const std::vector<int> s = sorted_copy(npu[c]);
        ns[c].p90 = pctl(s, 90.0);
        ns[c].mx = (double)s.back();
        ns[c].mean = (double)std::accumulate(s.begin(), s.end(), 0LL) / (double)s.size();
        size_t ge = 0;
        for (int v : s)
            if (v >= 90) ++ge;
        ns[c].ge90 = 100.0 * (double)ge / (double)s.size();
    }

    double ddr_mean = 0, ddr_med = 0, ddr_p90 = 0, ddr_max = 0, ddr_freq_mean = 0;
    std::vector<std::pair<unsigned long long, size_t>> freq_hist;
    if (!ddr_load.empty()) {
        const std::vector<int> s = sorted_copy(ddr_load);
        ddr_mean = (double)std::accumulate(ddr_load.begin(), ddr_load.end(), 0LL) / (double)ddr_load.size();
        ddr_med = pctl(s, 50.0);
        ddr_p90 = pctl(s, 90.0);
        ddr_max = (double)s.back();

        unsigned long long fsum = 0;
        for (auto f : ddr_freq) fsum += f;
        ddr_freq_mean = (double)fsum / (double)ddr_freq.size();

        const std::vector<unsigned long long> sf = sorted_copy(ddr_freq);
        for (auto f : sf) {
            if (!freq_hist.empty() && freq_hist.back().first == f)
                freq_hist.back().second++;
            else
                freq_hist.push_back(std::make_pair(f, (size_t)1));
        }
        std::sort(freq_hist.begin(), freq_hist.end(),
                  [](const std::pair<unsigned long long, size_t> &a,
                     const std::pair<unsigned long long, size_t> &b) {
                      return a.second > b.second;
                  });
    }

    long rss_first = 0, rss_last = 0, rss_peak = 0;
    int  thr_peak = 0;
    if (!mem.empty()) {
        rss_first = mem.front().rss;
        rss_last = mem.back().rss;
        for (const auto &m : mem) {
            if (m.rss > rss_peak) rss_peak = m.rss;
            if (m.threads > thr_peak) thr_peak = m.threads;
        }
    }

    std::string probe_line;
    for (size_t i = 0; i < probes.size(); ++i) {
        if (i) probe_line += "；";
        probe_line += probes[i].library + " @" + hex_u64(probes[i].offset);
    }
    if (probe_line.empty())
        probe_line = "（未挂载）";

    // ---------------- OVERVIEW.md ----------------
    std::ostringstream md;
    md << "# rkperf 采集总览\n\n";
    md << "> `rkperf " << kVersion << "` 自动生成 · " << o.ended_at << "\n\n";

    md << "| 项 | 值 |\n|---|---|\n";
    md << "| 目标程序 | `" << g_program << "` |\n";
    md << "| 采集目录 | `" << g_root << "` |\n";
    md << "| 开始 → 结束 | " << g_started << " → " << o.ended_at << " |\n";
    md << "| 时长 | " << f3(o.duration_s) << " s |\n";
    md << "| 退出 | " << (o.exit_reason.empty() ? "unknown" : o.exit_reason)
       << "，code " << o.exit_code
       << (o.interrupted ? "（被中断）" : "") << " |\n";
    md << "| 平台 | " << (g_uname.empty() ? "unknown" : g_uname) << " · " << host_name() << " |\n";
    md << "| 探针 | " << probe_line << " |\n\n";

    md << "## 一、速览\n\n";
    if (ev == 0) {
        md << "- 本次没有采到 `rknn_run` 事件。\n";
    } else {
        md << "- `rknn_run` **" << ev << "** 次（SYNC " << nsync << " / ASYNC " << nasync
           << "），失败 " << nfail << " 次\n";
        md << "- 单次耗时：中位 **" << f3(med_ns / 1e6) << " ms**，P90 "
           << f3(p90_ns / 1e6) << " ms，P99 " << f3(p99_ns / 1e6)
           << " ms，最大 " << f3(vmax / 1e6) << " ms\n";
        md << "- 平均并发度 **" << f2(conc) << "**（三核理论上限 3.00）\n";
    }
    if (!ddr_load.empty()) {
        md << "- DDR：均值 " << p1(ddr_mean) << "，P90 " << p1(ddr_p90)
           << "，峰值 " << p1(ddr_max)
           << "，主频点 " << ddr_freq_mean / 1e6 << " MHz\n";
    }
    for (int c = 0; c < 3; ++c) {
        if (ns[c].n == 0)
            continue;
        md << "- NPU core" << c << "：均值 " << p1(ns[c].mean)
           << "，峰值 " << p1(ns[c].mx) << "\n";
    }
    md << "\n";

    md << "## 二、rknn_run 明细\n\n";
    if (ev == 0) {
        md << "_无数据_——检查探针是否挂上（看 `rkperf.log` 里的 `attach` 行）。\n\n";
    } else {
        md << "| 指标 | 值 |\n|---|---|\n";
        md << "| 调用次数 | " << ev << " |\n";
        md << "| SYNC / ASYNC | " << nsync << " / " << nasync << " |\n";
        md << "| 失败（ret≠0） | " << nfail << " |\n";
        md << "| 总耗时 | " << f3((double)sum / 1e6) << " ms |\n";
        md << "| 均值 | " << f3(mean_ns / 1e6) << " ms |\n";
        md << "| 中位 | " << f3(med_ns / 1e6) << " ms |\n";
        md << "| P90 / P99 | " << f3(p90_ns / 1e6) << " / " << f3(p99_ns / 1e6) << " ms |\n";
        md << "| 最大 / 最小 | " << f3((double)vmax / 1e6) << " / "
           << f3((double)vmin / 1e6) << " ms |\n";
        md << "| 平均并发度 | " << f2(conc) << " |\n\n";

        md << "按核（`core_mask` 来自驱动回写，0 表示 auto/未知）：\n\n";
        md << "| core | 次数 | 占比 | 均值 |\n|---|---|---|---|\n";
        for (int i = 0; i < 8; ++i) {
            if (core_cnt[i] == 0)
                continue;
            md << "| " << (i == 0 ? "auto" : std::string("core") + std::to_string(i)) << " | "
               << core_cnt[i] << " | "
               << f2(100.0 * (double)core_cnt[i] / (double)ev) << "% | "
               << f3((double)core_sum[i] / (double)core_cnt[i] / 1e6) << " ms |\n";
        }
        md << "\n";
        if (truncated) {
            md << "> 存放原始耗时的样本数达到上限（" << kMaxDurSamples
               << "），分位数基于前 " << kMaxDurSamples << " 次调用；计数与总耗时仍是全程。\n\n";
        }
    }

    md << "## 三、NPU 利用率\n\n";
    md << "口径：`/sys/kernel/debug/rknpu/load`，驱动统计窗口 1 s，采样 1 Hz。\n\n";
    if (ns[0].n == 0) {
        md << "_无数据_——需要 root，且驱动要暴露 debugfs 节点。\n\n";
    } else {
        md << "| core | 样本 | 均值 | P90 | 峰值 | ≥90% 占比 |\n|---|---|---|---|---|---|\n";
        for (int c = 0; c < 3; ++c) {
            md << "| core" << c << " | " << ns[c].n << " | " << p1(ns[c].mean) << " | "
               << p1(ns[c].p90) << " | " << p1(ns[c].mx) << " | " << p1(ns[c].ge90) << " |\n";
        }
        md << "\n";
    }

    md << "## 四、DDR 带宽\n\n";
    if (ddr_load.empty()) {
        md << "_无数据_——`/sys/class/devfreq/dmc/load` 不可读。\n\n";
    } else {
        md << "| 采样 | 均值 | 中位 | P90 | 峰值 | 均值频点 |\n|---|---|---|---|---|---|\n";
        md << "| " << ddr_load.size() << " | " << p1(ddr_mean) << " | " << p1(ddr_med)
           << " | " << p1(ddr_p90) << " | " << p1(ddr_max) << " | "
           << f3(ddr_freq_mean / 1e6) << " MHz |\n\n";

        md << "频点分布（前 5）：\n\n| 频点 | 样本 | 占比 |\n|---|---|---|\n";
        for (size_t i = 0; i < freq_hist.size() && i < 5; ++i) {
            md << "| " << freq_hist[i].first / 1000000ULL << " MHz | " << freq_hist[i].second
               << " | " << p1(100.0 * (double)freq_hist[i].second / (double)ddr_freq.size())
               << " |\n";
        }
        md << "\n";
    }

    md << "## 五、进程内存\n\n";
    if (mem.empty()) {
        md << "_未采集_（进程已退出，或首次采样前就结束）。\n\n";
    } else {
        md << "| VmRSS 起始 | 结束 | 峰值 | 线程数峰值 | 采样 |\n|---|---|---|---|---|\n";
        md << "| " << f2(rss_first / 1024.0) << " MB | " << f2(rss_last / 1024.0)
           << " MB | " << f2(rss_peak / 1024.0) << " MB | " << thr_peak
           << " | " << mem.size() << " |\n\n";
    }

    md << "## 六、产出文件\n\n";
    md << "| 文件 | 内容 |\n|---|---|\n";
    md << "| `rkperf.log` | 可读日志（含每次调用的明细行） |\n";
    md << "| `events.csv` | 每次 `rknn_run` 一行 |\n";
    md << "| `npu.csv` | 三核利用率，1 Hz |\n";
    md << "| `ddr.csv` | DDR 占用率与频点，100 ms |\n";
    md << "| `mem.csv` | 进程 VmRSS/VmHWM/线程数，5 s |\n";
    md << "| `summary/OVERVIEW.md` | 本文件 |\n";
    md << "| `summary/stats.json` | 本文件数据的机器可读版 |\n";
    md << "| `summary/config.json` | 本次采集的完整配置（目标、探针、平台） |\n";
    md << "| `summary/*.txt` | 环境快照：uname / lscpu / mount / df / net / board / npu / libs |\n";
    md << "| `summary/proc/<pid>/` | 目标进程的 /proc 快照（status / smaps_rollup / limits / maps / tasks…） |\n";

    write_file(g_dir + "/OVERVIEW.md", md.str());

    // ---------------- stats.json ----------------
    std::ostringstream js;
    js << "{\n";
    js << "  \"tool\": \"rkperf\",\n";
    js << "  \"version\": \"" << kVersion << "\",\n";
    js << "  \"program\": \"" << json_esc(g_program) << "\",\n";
    js << "  \"started_at\": \"" << json_esc(g_started) << "\",\n";
    js << "  \"ended_at\": \"" << json_esc(o.ended_at) << "\",\n";
    js << "  \"duration_s\": " << f3(o.duration_s) << ",\n";
    js << "  \"exit\": {\"reason\": \"" << json_esc(o.exit_reason) << "\", \"code\": "
       << o.exit_code << ", \"interrupted\": " << (o.interrupted ? "true" : "false") << "},\n";

    js << "  \"rknn_run\": {\"count\": " << ev << ", \"sync\": " << nsync
       << ", \"async\": " << nasync << ", \"failed\": " << nfail
       << ", \"total_ms\": " << f3((double)sum / 1e6)
       << ", \"mean_ms\": " << f3(mean_ns / 1e6)
       << ", \"median_ms\": " << f3(med_ns / 1e6)
       << ", \"p90_ms\": " << f3(p90_ns / 1e6)
       << ", \"p99_ms\": " << f3(p99_ns / 1e6)
       << ", \"max_ms\": " << f3((double)vmax / 1e6)
       << ", \"min_ms\": " << f3((double)vmin / 1e6)
       << ", \"concurrency\": " << f2(conc)
       << ", \"samples_truncated\": " << (truncated ? "true" : "false")
       << ", \"core_mask\": {";
    bool first_core = true;
    for (int i = 0; i < 8; ++i) {
        if (core_cnt[i] == 0)
            continue;
        if (!first_core) js << ", ";
        first_core = false;
        js << "\"" << (i == 0 ? "auto" : std::string("core") + std::to_string(i)) << "\": {\"count\": "
           << core_cnt[i] << ", \"mean_ms\": "
           << f3((double)core_sum[i] / (double)core_cnt[i] / 1e6) << "}";
    }
    js << "}},\n";

    js << "  \"npu\": {";
    for (int c = 0; c < 3; ++c) {
        if (c) js << ", ";
        js << "\"core" << c << "\": {\"samples\": " << ns[c].n
           << ", \"mean_pct\": " << f2(ns[c].mean)
           << ", \"p90_pct\": " << f2(ns[c].p90)
           << ", \"max_pct\": " << f2(ns[c].mx)
           << ", \"ge90_ratio_pct\": " << f2(ns[c].ge90) << "}";
    }
    js << "},\n";

    js << "  \"ddr\": {\"samples\": " << ddr_load.size()
       << ", \"mean_pct\": " << f2(ddr_mean)
       << ", \"median_pct\": " << f2(ddr_med)
       << ", \"p90_pct\": " << f2(ddr_p90)
       << ", \"max_pct\": " << f2(ddr_max)
       << ", \"mean_freq_hz\": " << (unsigned long long)(ddr_freq_mean + 0.5) << "},\n";

    js << "  \"mem\": {\"samples\": " << mem.size()
       << ", \"rss_first_kb\": " << rss_first
       << ", \"rss_last_kb\": " << rss_last
       << ", \"rss_peak_kb\": " << rss_peak
       << ", \"threads_peak\": " << thr_peak << "}\n";
    js << "}\n";

    write_file(g_dir + "/stats.json", js.str());
}

}  // namespace rksummary
