// rkoplog.cpp —— 从 librknnrt 的 verbose 日志里抠出逐层算子耗时
//
// 数据源（RKNN_LOG_LEVEL=4 时由 runtime 打到 stdout）：
//
//   ---------------------------------------------------------------------------
//                                    Network Layer Information Table
//   ---------------------------------------------------------------------------
//   ID   OpType   DataType Target InputShape  OutputShape  Cycles(DDR/NPU/Total)  Time(us)  MacUsage(%)  WorkLoad(0/1/2)  RW(KB)  FullName
//   ---------------------------------------------------------------------------
//   1    InputOperator  UINT8  CPU  \  (1,3,640,640)  0/0/0  7  ...  InputOperator:images
//   2    ConvExSwish    INT8   NPU  ...                1929  ...  Conv:/model.0/conv/Conv
//
// 每个 rknn_run 打一张表，所以表的序号就是帧号（多线程时按表到达顺序排）。
//
// 产出只有一个纯文本报表 ops_report.txt：
//   每帧一张层表（和官方 eval_perf 同样的列）+ 该帧合计，
//   末尾一张跨帧聚合的 Operator Time Consuming Ranking。

#include "rkoplog.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include "rklog.h"
#include "rkutil.h"

namespace {

int  g_rfd = -1;                  // 读端
int  g_wfd = -1;                  // 写端（交给子进程）
FILE *g_report = nullptr;         // ops_report.txt
FILE *g_folded = nullptr;         // ops_folded.txt（火焰图 collapsed stacks）
std::string g_report_path;
std::string g_folded_path;
std::thread g_reader;
std::atomic<bool> g_stop{false};
std::atomic<bool> g_eof{false};
std::atomic<long> g_frames{0};

struct row {
    int         id = 0;
    std::string op;
    std::string dtype;
    std::string target;
    std::string in_shape;
    std::string out_shape;
    unsigned long long c_ddr = 0, c_npu = 0, c_tot = 0;
    double      time_us = 0;
    bool        has_mac = false;
    double      mac[3] = {0, 0, 0};
    bool        has_wl = false;
    double      wl[3] = {0, 0, 0};
    long        rw_kb = 0;
    std::string name;
};

struct op_agg {
    long   calls = 0;
    double cpu_us = 0, gpu_us = 0, npu_us = 0, total_us = 0;
};

struct frame_agg {
    int    frame = 0;
    int    ops = 0;
    double elapsed_us = 0;
    long   rw_kb = 0;
};

std::map<std::string, op_agg> g_ops;        // 只由读线程写，finish 里 join 之后再读
std::vector<frame_agg>        g_frame_sums;

double mono_ns()
{
    return rkutil::mono_seconds() * 1e9;
}

bool is_sep_line(const std::string &s)
{
    bool seen = false;
    for (char c : s) {
        if (c == '-') { seen = true; continue; }
        if (c == ' ' || c == '\t' || c == '\r') continue;
        return false;
    }
    return seen;
}

std::string trim(const std::string &s)
{
    size_t b = 0, e = s.size();
    while (b < e && isspace((unsigned char)s[b])) b++;
    while (e > b && isspace((unsigned char)s[e - 1])) e--;
    return s.substr(b, e - b);
}

// runtime 的每一行都带日志前缀：
//   D RKNN: [14:56:20.788] 1    InputOperator  UINT8  CPU  ...
// 解析前把它剥掉，剩下的才是表格内容。
std::string strip_prefix(const std::string &line)
{
    size_t p = line.find("RKNN: [");
    if (p == std::string::npos) return line;
    size_t e = line.find("] ", p);
    if (e == std::string::npos) return line;
    return line.substr(e + 2);
}

bool is_uint(const std::string &s)
{
    if (s.empty()) return false;
    for (char c : s)
        if (!isdigit((unsigned char)c)) return false;
    return true;
}

// "762676/921600/921600"
bool parse_triple_u64(const std::string &s, unsigned long long &a,
                      unsigned long long &b, unsigned long long &c)
{
    size_t p1 = s.find('/');
    if (p1 == std::string::npos) return false;
    size_t p2 = s.find('/', p1 + 1);
    if (p2 == std::string::npos) return false;

    std::string x = s.substr(0, p1), y = s.substr(p1 + 1, p2 - p1 - 1), z = s.substr(p2 + 1);
    if (!is_uint(x) || !is_uint(y) || !is_uint(z)) return false;
    a = strtoull(x.c_str(), nullptr, 10);
    b = strtoull(y.c_str(), nullptr, 10);
    c = strtoull(z.c_str(), nullptr, 10);
    return true;
}

// "4.48/0.00/0.00"
bool parse_triple_double(const std::string &s, double v[3])
{
    size_t p1 = s.find('/');
    if (p1 == std::string::npos) return false;
    size_t p2 = s.find('/', p1 + 1);
    if (p2 == std::string::npos) return false;

    std::string x = s.substr(0, p1), y = s.substr(p1 + 1, p2 - p1 - 1), z = s.substr(p2 + 1);
    char *end = nullptr;
    v[0] = strtod(x.c_str(), &end);
    if (end == x.c_str()) return false;
    v[1] = strtod(y.c_str(), nullptr);
    v[2] = strtod(z.c_str(), nullptr);
    return true;
}

// 一行层数据。CPU 行会少 MacUsage 列，所以不能按固定列号取，
// 而是先找 "a/b/c" 那个 Cycles token，再按它往后取。
bool parse_row(const std::string &line, row &r)
{
    if (line.empty() || !isdigit((unsigned char)line[0])) return false;

    std::vector<std::pair<size_t, size_t>> tok;   // [begin, end)
    for (size_t i = 0; i < line.size();) {
        while (i < line.size() && isspace((unsigned char)line[i])) i++;
        if (i >= line.size()) break;
        size_t b = i;
        while (i < line.size() && !isspace((unsigned char)line[i])) i++;
        tok.emplace_back(b, i);
    }
    if (tok.size() < 9) return false;

    auto text = [&](size_t k) { return line.substr(tok[k].first, tok[k].second - tok[k].first); };

    r.id     = atoi(text(0).c_str());
    r.op     = text(1);
    r.dtype  = text(2);
    r.target = text(3);

    size_t cyc = 0;
    bool   got = false;
    for (size_t k = 4; k + 1 < tok.size(); ++k) {
        if (parse_triple_u64(text(k), r.c_ddr, r.c_npu, r.c_tot)) { cyc = k; got = true; break; }
    }
    if (!got) return false;

    // Cycles 前面是两个形状列（InputShape / OutputShape）
    if (cyc >= 6) {
        r.in_shape  = text(cyc - 2);
        r.out_shape = text(cyc - 1);
    }

    r.time_us = strtod(text(cyc + 1).c_str(), nullptr);

    size_t k = cyc + 2;
    if (k < tok.size()) {
        std::string t = text(k);
        if (t.find('%') == std::string::npos && t.find('/') != std::string::npos &&
            t.find('.') != std::string::npos) {
            r.has_mac = parse_triple_double(t, r.mac);
            k++;
        }
    }
    if (k < tok.size()) {
        std::string t = text(k);
        if (t.find('%') != std::string::npos) {
            r.has_wl = parse_triple_double(t, r.wl);
            k++;
        }
    }
    if (k < tok.size() && is_uint(text(k))) {
        r.rw_kb = atol(text(k).c_str());
        k++;
    }

    // FullName 是最后一列，形如 "Conv:/model.0/conv/Conv"。
    // 取行内第一个 ':'，再往前吃掉算子名前缀；这样名字里带空格也不会截断。
    size_t colon = line.find(':');
    if (colon != std::string::npos) {
        size_t b = colon;
        while (b > 0 && (isalnum((unsigned char)line[b - 1]) || line[b - 1] == '_')) b--;
        r.name = trim(line.substr(b));
    } else if (k < tok.size()) {
        r.name = trim(line.substr(tok[k].first));
    }
    return true;
}

enum { ST_IDLE, ST_HEADER, ST_ROWS };

struct frame_state {
    int    frame = 0;
    double ts_ns = 0;
    double sum_us = 0;
    long   sum_rw = 0;
    std::vector<row> rows;
};

// 把一条层数据排成官方那种定宽行
void format_layer_row(char *out, size_t n, const row &r)
{
    char mac[48] = "", wl[48] = "", cyc[64];
    if (r.has_mac) snprintf(mac, sizeof(mac), "%.2f/%.2f/%.2f", r.mac[0], r.mac[1], r.mac[2]);
    if (r.has_wl)  snprintf(wl, sizeof(wl), "%.1f%%/%.1f%%/%.1f%%", r.wl[0], r.wl[1], r.wl[2]);
    snprintf(cyc, sizeof(cyc), "%llu/%llu/%llu",
             (unsigned long long)r.c_ddr, (unsigned long long)r.c_npu,
             (unsigned long long)r.c_tot);
    snprintf(out, n, "%-4d %-16s %-8s %-6s %-40s %-22s %-22s %-8.0f %-16s %-18s %-8ld %s",
             r.id, r.op.c_str(), r.dtype.c_str(), r.target.c_str(),
             r.in_shape.c_str(), r.out_shape.c_str(), cyc, r.time_us,
             mac, wl, r.rw_kb, r.name.c_str());
}

const char *kRule =
    "----------------------------------------------------------------------------"
    "------------------------------------------------------------\n";

// "Conv:/model.22/cv3.0/cv3.0.0/conv/Conv" -> "全部;model.22;cv3.0;cv3.0.0;conv;Conv"
// 这就是火焰图的 collapsed stack 格式，层级来自层名里的模块路径。
std::string folded_stack(const std::string &full)
{
    size_t i = full.find(':');
    const std::string p = (i == std::string::npos) ? full : full.substr(i + 1);

    std::string out = "全部";
    size_t b = 0;
    for (;;) {
        size_t e = p.find('/', b);
        std::string seg = p.substr(b, (e == std::string::npos ? p.size() : e) - b);
        if (!seg.empty()) {
            out += ';';
            out += seg;
        }
        if (e == std::string::npos) break;
        b = e + 1;
    }
    return out;
}

// 一帧结束：把这一帧的表写进报表，并累计统计
void end_frame(frame_state &fs)
{
    if (fs.rows.empty()) return;

    frame_agg fa;
    fa.frame      = fs.frame;
    fa.ops        = (int)fs.rows.size();
    fa.elapsed_us = fs.sum_us;
    fa.rw_kb      = fs.sum_rw;
    g_frame_sums.push_back(fa);

    if (g_report) {
        fprintf(g_report, "%s", kRule);
        fprintf(g_report, " frame %d    %d ops    elapsed %.0f us    rw %ld KB\n",
                fs.frame, fa.ops, fa.elapsed_us, fa.rw_kb);
        fprintf(g_report, "%s", kRule);
        fprintf(g_report, "%-4s %-16s %-8s %-6s %-40s %-22s %-22s %-8s %-16s %-18s %-8s %s\n",
                "ID", "OpType", "DataType", "Target", "InputShape", "OutputShape",
                "Cycles(DDR/NPU/Total)", "Time(us)", "MacUsage(%)",
                "WorkLoad(0/1/2)", "RW(KB)", "FullName");
        fprintf(g_report, "%s", kRule);
        for (const auto &r : fs.rows) {
            char buf[1024];
            format_layer_row(buf, sizeof(buf), r);
            fprintf(g_report, "%s\n", buf);
        }
        fprintf(g_report, "%s", kRule);
        fprintf(g_report, "Total Operator Elapsed Per Frame Time(us): %.0f\n", fa.elapsed_us);
        fprintf(g_report, "Total Memory Read/Write Per Frame Size(KB): %.2f\n\n", (double)fa.rw_kb);
        fflush(g_report);
    }
    if (g_folded) fflush(g_folded);
}

void on_row(const std::string &line, frame_state &fs)
{
    row r;
    if (!parse_row(line, r)) return;

    if (fs.rows.empty()) {
        fs.frame = (int)++g_frames;
        fs.ts_ns = mono_ns();
        fs.sum_us = 0;
        fs.sum_rw = 0;
    }
    fs.rows.push_back(r);
    fs.sum_us += r.time_us;
    fs.sum_rw += r.rw_kb;

    if (g_folded) {
        char val[32];
        snprintf(val, sizeof(val), " %ld", (long)llround(r.time_us));
        fputs(folded_stack(r.name).c_str(), g_folded);
        fputs(val, g_folded);
        fputc('\n', g_folded);
    }

    op_agg &a = g_ops[r.op];
    a.calls++;
    a.total_us += r.time_us;
    if (r.target == "CPU")      a.cpu_us += r.time_us;
    else if (r.target == "GPU") a.gpu_us += r.time_us;
    else                        a.npu_us += r.time_us;
}

void feed(const std::string &raw, int &st, frame_state &fs)
{
    const std::string line = strip_prefix(raw);

    if (st == ST_IDLE) {
        if (line.find("Network Layer Information Table") != std::string::npos) {
            fs = frame_state{};
            st = ST_HEADER;
        }
        return;
    }
    if (st == ST_HEADER) {
        if (!line.empty() && isdigit((unsigned char)line[0])) {
            st = ST_ROWS;
            on_row(line, fs);
        }
        return;
    }
    // ST_ROWS
    if (!line.empty() && isdigit((unsigned char)line[0])) {
        on_row(line, fs);
        return;
    }
    if (is_sep_line(line)) {          // 表尾的分隔线
        end_frame(fs);
        fs = frame_state{};
        st = ST_IDLE;
        return;
    }
    // 其它行（比如 rkperf 自己的告警）忽略，不打断当前表
}

void reader_main()
{
    int st = ST_IDLE;
    frame_state fs;
    std::string pending;
    std::vector<char> buf(64 * 1024);

    while (!g_stop.load()) {
        struct pollfd p = {g_rfd, POLLIN, 0};
        int n = poll(&p, 1, 100);
        if (n <= 0) continue;

        ssize_t r = read(g_rfd, buf.data(), buf.size());
        if (r > 0) {
            // 原样转发，别把被测程序自己的输出吞了
            ssize_t off = 0;
            while (off < r) {
                ssize_t w = write(STDOUT_FILENO, buf.data() + off, (size_t)(r - off));
                if (w <= 0) break;
                off += w;
            }

            pending.append(buf.data(), (size_t)r);
            size_t pos;
            while ((pos = pending.find('\n')) != std::string::npos) {
                std::string line = pending.substr(0, pos);
                pending.erase(0, pos + 1);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                feed(line, st, fs);
            }
        } else if (r == 0) {
            g_eof.store(true);
            break;                     // EOF：子进程（及其子进程）都关了 stdout
        } else if (errno != EINTR) {
            break;
        }
    }

    if (!pending.empty()) feed(pending, st, fs);
    end_frame(fs);                     // 最后一帧可能没有表尾分隔线
}

}  // namespace

namespace rkoplog {

bool setup(const std::string &report_path)
{
    if (g_wfd >= 0) return true;

    int fds[2];
    if (pipe(fds) != 0) {
        rklog::warn(std::string("op-time: pipe() failed: ") + strerror(errno));
        return false;
    }
    // 读端 close-on-exec；写端要跟着 exec 走（清掉 CLOEXEC）
    fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    fcntl(fds[1], F_SETFD, 0);
    g_rfd = fds[0];
    g_wfd = fds[1];

    g_report = fopen(report_path.c_str(), "w");
    if (!g_report) {
        rklog::warn("op-time: open " + report_path + " failed: " + strerror(errno));
        close(g_rfd);
        close(g_wfd);
        g_rfd = g_wfd = -1;
        return false;
    }
    g_report_path = report_path;
    {
        // ops_report.txt -> ops_folded.txt
        size_t dot = report_path.rfind('.');
        g_folded_path = (dot == std::string::npos)
                            ? report_path + "_folded.txt"
                            : report_path.substr(0, dot) + "_folded.txt";
    }

    g_folded = fopen(g_folded_path.c_str(), "w");
    if (!g_folded)
        rklog::warn("op-time: open " + g_folded_path + " failed: " + strerror(errno));

    fprintf(g_report,
            "# rkperf --op-time 逐层算子耗时\n"
            "# 数据来源：librknnrt 在 RKNN_LOG_LEVEL=4 时打印的 "
            "Network Layer Information Table（每个 rknn_run 一张）\n\n");
    fflush(g_report);
    return true;
}

int child_fd() { return g_wfd; }

bool enabled() { return g_wfd >= 0; }

void start()
{
    if (g_wfd < 0) return;
    close(g_wfd);                 // 父进程不再持有写端，否则永远等不到 EOF
    g_wfd = -1;
    g_reader = std::thread(reader_main);
}

void finish()
{
    if (g_rfd < 0) return;

    // 先等读线程把管道里剩下的数据读完（子进程退出后写端就关了，EOF 很快到）。
    // 万一有孙进程攥着 stdout 不放，最多等 2 秒就强制收工。
    for (int i = 0; i < 200 && !g_eof.load(); ++i) {
        struct timespec ts = {0, 10L * 1000 * 1000};
        nanosleep(&ts, nullptr);
    }
    g_stop.store(true);
    if (g_reader.joinable()) g_reader.join();

    if (g_rfd >= 0) {
        close(g_rfd);
        g_rfd = -1;
    }

    const long frames = g_frames.load();
    if (frames == 0) {
        if (g_report) {
        fprintf(g_report, "没有捕获到层表（RKNN_LOG_LEVEL 未生效？模型没跑？）\n");
        fclose(g_report);
        g_report = nullptr;
    }
    if (g_folded) {
        fclose(g_folded);
        g_folded = nullptr;
    }
        rklog::warn("op-time: 没有捕获到层表（RKNN_LOG_LEVEL 未生效？模型没跑？）");
        return;
    }

    double sum = 0, mn = 1e18, mx = 0;
    for (const auto &fa : g_frame_sums) {
        sum += fa.elapsed_us;
        mn = std::min(mn, fa.elapsed_us);
        mx = std::max(mx, fa.elapsed_us);
    }
    std::vector<double> sorted;
    for (const auto &fa : g_frame_sums) sorted.push_back(fa.elapsed_us);
    std::sort(sorted.begin(), sorted.end());
    const double med = sorted.empty() ? 0 : sorted[sorted.size() / 2];

    char line[512];
    rklog::info("op-time: " + std::to_string(frames) + " 帧, " +
                std::to_string(g_ops.size()) + " 种算子");
    snprintf(line, sizeof(line),
             "op-time: 每帧层耗时合计 中位 %.1f us, 最小 %.1f, 最大 %.1f", med, mn, mx);
    rklog::info(line);

    // 按总耗时排序
    std::vector<std::pair<std::string, op_agg>> rank(g_ops.begin(), g_ops.end());
    std::sort(rank.begin(), rank.end(), [](const auto &a, const auto &b) {
        return a.second.total_us > b.second.total_us;
    });

    double grand = 0;
    for (const auto &kv : rank) grand += kv.second.total_us;

    if (g_report) {
        fprintf(g_report, "%s", kRule);
        fprintf(g_report, "      Operator Time Consuming Ranking (%ld frames)\n", frames);
        fprintf(g_report, "%s", kRule);
        fprintf(g_report, "%-18s %-12s %-13s %-13s %-13s %-14s %s\n",
                "OpType", "CallNumber", "CPUTime(us)", "GPUTime(us)",
                "NPUTime(us)", "TotalTime(us)", "TimeRatio(%)");
        fprintf(g_report, "%s", kRule);
        double tc = 0, tg = 0, tn = 0, tt = 0;
        for (const auto &kv : rank) {
            const op_agg &a = kv.second;
            double ratio = grand > 0 ? a.total_us * 100.0 / grand : 0;
            fprintf(g_report, "%-18s %-12ld %-13.1f %-13.1f %-13.1f %-14.1f %.2f\n",
                    kv.first.c_str(), a.calls, a.cpu_us, a.gpu_us, a.npu_us,
                    a.total_us, ratio);
            tc += a.cpu_us; tg += a.gpu_us; tn += a.npu_us; tt += a.total_us;
        }
        fprintf(g_report, "%s", kRule);
        fprintf(g_report, "%-18s %-12s %-13.1f %-13.1f %-13.1f %-14.1f\n",
                "Total", "", tc, tg, tn, tt);
        fprintf(g_report, "%s", kRule);
        fclose(g_report);
        g_report = nullptr;
    }
    if (g_folded) {
        fflush(g_folded);
        fclose(g_folded);
        g_folded = nullptr;
    }

    for (size_t i = 0; i < rank.size() && i < 8; ++i) {
        const auto &kv = rank[i];
        double ratio = grand > 0 ? kv.second.total_us * 100.0 / grand : 0;
        snprintf(line, sizeof(line),
                 "op-time top%zu: %-14s calls=%-5ld %9.1f us  %5.1f%%  (cpu %.1f / npu %.1f)",
                 i + 1, kv.first.c_str(), kv.second.calls, kv.second.total_us, ratio,
                 kv.second.cpu_us, kv.second.npu_us);
        rklog::info(line);
    }
    rklog::info("op-time: 明细 " + g_report_path);
    rklog::info("op-time: 火焰图数据 " + g_folded_path +
                "（flamegraph.pl 或 tools/flamegen.py 可直接转 SVG）");
}

}  // namespace rkoplog
