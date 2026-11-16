// rkperf.cpp —— 采集进程在 rknn_run 里的耗时
//
// 用法:
//   sudo rkperf <程序> [参数...]
//   sudo rkperf python3 infer.py            # Python SDK 也可以
//
// 目标库自动发现（不写死路径）:
//   /usr/lib/librknnrt.so 等系统路径
//   Python 包里自带的副本（rknn/runtime/Linux/librknn_api/*/librknnrt.so）
// 找到几个挂几个，目标进程加载哪个就命中哪个。
//
// 日志固定写 /tmp/rkperf.log 和 /tmp/rkperf.log.csv
// 落盘逻辑在 rklog.cpp，这里只负责：找库 -> 解析符号 -> 加载 BPF ->
// 拉起进程 -> 挂探针 -> 收事件。

#define _GNU_SOURCE
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <string>
#include <vector>
#include <ctime>
#include <unistd.h>
#include <csignal>
#include <fcntl.h>
#include <glob.h>
#include <sys/wait.h>
#include <sys/types.h>

#include <libelf.h>
#include <gelf.h>

#include <bpf/libbpf.h>
#include <bpf/libbpf_common.h>
#include <bpf/bpf.h>

#include "rk_event.h"
#include "rklog.h"

#define SYM_NAME   "rknn_run"
#define LOG_PATH   "/tmp/rkperf.log"
#define CSV_PATH   "/tmp/rkperf.log.csv"

#define PERF_PAGES 64

/* 一个待挂载的目标库 */
struct probe_target {
    std::string path;
    unsigned long long offset = 0;
};

static volatile sig_atomic_t g_stop = 0;
static void on_signal(int sig) { (void)sig; g_stop = 1; }

/* ---------------- 时间工具 ---------------- */

static double mono_seconds()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static std::string iso_now()
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

/* ---------------- 找 librknnrt.so ---------------- */

static void add_unique(std::vector<std::string> &v, const std::string &p)
{
    if (p.empty()) return;
    if (access(p.c_str(), R_OK) != 0) return;
    for (const auto &x : v)
        if (x == p) return;
    v.push_back(p);
}

static void glob_add(std::vector<std::string> &v, const std::string &pattern)
{
    glob_t g;
    if (glob(pattern.c_str(), GLOB_NOSORT, nullptr, &g) == 0) {
        for (size_t i = 0; i < g.gl_pathc; i++)
            add_unique(v, g.gl_pathv[i]);
    }
    globfree(&g);
}

/* 返回系统里所有可能的 librknnrt.so */
static std::vector<std::string> find_rknn_libs()
{
    std::vector<std::string> out;

    /* 系统路径 */
    add_unique(out, "/usr/lib/librknnrt.so");
    add_unique(out, "/usr/local/lib/librknnrt.so");
    add_unique(out, "/usr/lib/aarch64-linux-gnu/librknnrt.so");

    /* Python 包里自带的副本（rknn-toolkit-lite2 等） */
    glob_add(out, "/usr/local/lib/python3.*/dist-packages/rknn/runtime/Linux/librknn_api/*/librknnrt.so");
    glob_add(out, "/usr/lib/python3/dist-packages/rknn/runtime/Linux/librknn_api/*/librknnrt.so");
    glob_add(out, "/usr/local/lib/python3.*/site-packages/rknn/runtime/Linux/librknn_api/*/librknnrt.so");
    glob_add(out, "/root/.local/lib/python3.*/site-packages/rknn/runtime/Linux/librknn_api/*/librknnrt.so");
    glob_add(out, "/home/*/.local/lib/python3.*/site-packages/rknn/runtime/Linux/librknn_api/*/librknnrt.so");
    glob_add(out, "/opt/*/lib/python3.*/site-packages/rknn/runtime/Linux/librknn_api/*/librknnrt.so");

    /* 虚拟环境 */
    const char *venv = getenv("VIRTUAL_ENV");
    if (venv && *venv)
        glob_add(out, std::string(venv) +
                      "/lib/python3.*/site-packages/rknn/runtime/Linux/librknn_api/*/librknnrt.so");

    /* 常见的解压版 SDK / 工具目录（如 ~/rk-tools/rknn-sdk/lib/） */
    glob_add(out, "/home/*/*/lib/librknnrt.so");
    glob_add(out, "/home/*/*/*/lib/librknnrt.so");
    glob_add(out, "/root/*/lib/librknnrt.so");
    glob_add(out, "/root/*/*/lib/librknnrt.so");
    glob_add(out, "/opt/*/lib/librknnrt.so");
    glob_add(out, "/opt/*/*/lib/librknnrt.so");
    glob_add(out, "/usr/local/*/lib/librknnrt.so");
    glob_add(out, "/usr/local/*/*/lib/librknnrt.so");

    return out;
}

/* ---------------- perf buffer 回调 ---------------- */

static void on_event(void *ctx, int cpu, void *data, __u32 size)
{
    (void)ctx; (void)cpu;
    if (size < sizeof(rk_event)) return;
    rklog::write_event(*static_cast<const rk_event *>(data));
}

static void on_lost(void *ctx, int cpu, unsigned long long cnt)
{
    (void)ctx;
    rklog::warn("lost " + std::to_string(cnt) + " events on cpu " + std::to_string(cpu));
}

/* ---------------- ELF 符号 -> 文件偏移 ---------------- */

static int elf_sym_offset(const char *path, const char *sym, unsigned long long *out)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;

    if (elf_version(EV_CURRENT) == EV_NONE) { close(fd); return -1; }
    Elf *elf = elf_begin(fd, ELF_C_READ, nullptr);
    if (!elf) { close(fd); return -1; }

    size_t shstrndx = 0;
    if (elf_getshdrstrndx(elf, &shstrndx) != 0) { elf_end(elf); close(fd); return -1; }

    size_t nph = 0;
    if (elf_getphdrnum(elf, &nph) != 0) { elf_end(elf); close(fd); return -1; }

    GElf_Phdr phdr;
    Elf_Scn *scn = nullptr;
    while ((scn = elf_nextscn(elf, scn)) != nullptr) {
        GElf_Shdr shdr;
        if (gelf_getshdr(scn, &shdr) != &shdr) continue;
        if (shdr.sh_type != SHT_SYMTAB && shdr.sh_type != SHT_DYNSYM) continue;

        Elf_Data *data = elf_getdata(scn, nullptr);
        if (!data) continue;
        size_t n = shdr.sh_size / shdr.sh_entsize;

        for (size_t i = 0; i < n; i++) {
            GElf_Sym s;
            if (gelf_getsym(data, (int)i, &s) != &s) continue;
            if (GELF_ST_TYPE(s.st_info) != STT_FUNC) continue;
            const char *name = elf_strptr(elf, shdr.sh_link, s.st_name);
            if (!name || strcmp(name, sym) != 0) continue;
            if (s.st_shndx == SHN_UNDEF) continue;

            unsigned long long v = s.st_value;
            for (size_t p = 0; p < nph; p++) {
                if (gelf_getphdr(elf, (int)p, &phdr) != &phdr) continue;
                if (phdr.p_type != PT_LOAD) continue;
                if (v >= phdr.p_vaddr && v < phdr.p_vaddr + phdr.p_filesz) {
                    *out = v - phdr.p_vaddr + phdr.p_offset;
                    elf_end(elf);
                    close(fd);
                    return 0;
                }
            }
        }
    }

    elf_end(elf);
    close(fd);
    return -1;
}

static std::string hex_u64(unsigned long long v)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "0x%llx", v);
    return std::string(buf);
}

/* ---------------- 主流程 ---------------- */

static int do_trace(char **child_argv)
{
    /* 1. 找所有 librknnrt.so 并解析符号偏移 */
    std::vector<probe_target> targets;
    for (const auto &lib : find_rknn_libs()) {
        unsigned long long off = 0;
        if (elf_sym_offset(lib.c_str(), SYM_NAME, &off) != 0) {
            rklog::warn("skip " + lib + " (no " SYM_NAME ")");
            continue;
        }
        targets.push_back({lib, off});
        rklog::info("target: " + lib + " @" + hex_u64(off));
    }
    if (targets.empty()) {
        rklog::error("no librknnrt.so found (looked in system paths and python packages)");
        return 1;
    }

    /* 2. 内嵌的 BPF 对象 */
    extern const unsigned char _binary_rkperf_bpf_o_start[];
    extern const unsigned char _binary_rkperf_bpf_o_end[];
    size_t bpf_sz = (size_t)(_binary_rkperf_bpf_o_end - _binary_rkperf_bpf_o_start);

    struct bpf_object *obj = bpf_object__open_mem(_binary_rkperf_bpf_o_start, bpf_sz, nullptr);
    if (!obj) { rklog::error("open embedded bpf object failed"); return 1; }

    if (bpf_object__load(obj) != 0) {
        rklog::error("load bpf object failed (需要 root)");
        return 1;
    }

    struct bpf_program *prog_enter = bpf_object__find_program_by_name(obj, "rknn_run_enter");
    struct bpf_program *prog_exit  = bpf_object__find_program_by_name(obj, "rknn_run_exit");
    struct bpf_map *map_events = bpf_object__find_map_by_name(obj, "events");
    if (!prog_enter || !prog_exit || !map_events) {
        rklog::error("bpf program/map not found");
        return 1;
    }

    /* 3. fork + exec */
    pid_t child = fork();
    if (child < 0) { rklog::error("fork: " + std::string(strerror(errno))); return 1; }
    if (child == 0) {
        setpgid(0, 0);
        execvp(child_argv[0], child_argv);
        fprintf(stderr, "execvp %s: %s\n", child_argv[0], strerror(errno));
        _exit(127);
    }

    const double t_start = mono_seconds();
    const std::string t_start_iso = iso_now();
    rklog::info("process start: pid=" + std::to_string(child) +
                " at " + t_start_iso + " cmd=" + child_argv[0]);

    /* 4. 每个库都挂一对探针（只挂子进程） */
    std::vector<struct bpf_link *> links;
    for (const auto &t : targets) {
        struct bpf_link *le = bpf_program__attach_uprobe(prog_enter, false, child,
                                                         t.path.c_str(), t.offset);
        struct bpf_link *lx = bpf_program__attach_uprobe(prog_exit,  true,  child,
                                                         t.path.c_str(), t.offset);
        if (!le || !lx) {
            rklog::warn("attach failed: " + t.path);
            if (le) bpf_link__destroy(le);
            if (lx) bpf_link__destroy(lx);
            continue;
        }
        links.push_back(le);
        links.push_back(lx);
    }
    if (links.empty()) {
        rklog::error("attach uprobe failed on all targets");
        kill(child, SIGKILL);
        waitpid(child, nullptr, 0);
        return 1;
    }
    rklog::info("attached " + std::to_string(links.size() / 2) + " target(s)");

    /* 5. perf buffer */
    struct perf_buffer_opts pb_opts = {
        .sample_cb = on_event,
        .lost_cb   = on_lost,
        .ctx       = nullptr,
    };
    struct perf_buffer *pb = perf_buffer__new(bpf_map__fd(map_events), PERF_PAGES, &pb_opts);
    if (!pb) { rklog::error("perf_buffer__new failed"); return 1; }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    /* 6. 采集循环 */
    int status = 0;
    bool child_done = false;
    while (!g_stop) {
        int err = perf_buffer__poll(pb, 100);
        if (err < 0 && err != -EINTR) { rklog::warn("poll: " + std::to_string(err)); break; }
        if (waitpid(child, &status, WNOHANG) == child) {
            child_done = true;
            perf_buffer__poll(pb, 200);
            break;
        }
    }

    /* 7. 收尾：SIGTERM -> 最多 2 秒 -> SIGKILL */
    if (!child_done) {
        kill(child, SIGTERM);
        for (int i = 0; i < 20; i++) {
            pid_t r = waitpid(child, &status, WNOHANG);
            if (r == child) { child_done = true; break; }
            if (r < 0 && errno == ECHILD) { child_done = true; break; }
            usleep(100000);
        }
        if (!child_done) {
            kill(child, SIGKILL);
            while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
        }
    }

    /* 8. 结束时刻 + 总时长 */
    const double t_end = mono_seconds();
    const std::string t_end_iso = iso_now();
    const double dur = t_end - t_start;

    const char *reason = "unknown";
    int code = -1;
    if (WIFEXITED(status))        { reason = "exited"; code = WEXITSTATUS(status); }
    else if (WIFSIGNALED(status)) { reason = "signal"; code = WTERMSIG(status); }

    char summary[256];
    snprintf(summary, sizeof(summary),
             "process end: pid=%d at %s duration=%.3fs reason=%s code=%d%s",
             (int)child, t_end_iso.c_str(), dur, reason, code,
             g_stop ? " (interrupted)" : "");
    rklog::info(summary);

    perf_buffer__free(pb);
    for (auto *l : links) bpf_link__destroy(l);
    bpf_object__close(obj);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <program> [args...]\n", argv[0]);
        return 1;
    }
    if (!rklog::init(LOG_PATH, CSV_PATH))
        return 1;

    rklog::info(std::string("rkperf start: target=") + argv[1]);

    int rc = do_trace(&argv[1]);

    rklog::info(std::string("log: " LOG_PATH "  csv: " CSV_PATH));
    rklog::flush();
    rklog::shutdown();
    return rc;
}
