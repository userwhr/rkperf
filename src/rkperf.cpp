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
#include <thread>
#include <atomic>
#include <unistd.h>
#include <csignal>
#include <fcntl.h>
#include <glob.h>
#include <sys/stat.h>
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
#define NPU_LOAD   "/sys/kernel/debug/rknpu/load"
#define OUT_ROOT   "/tmp"

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

/* ---------------- 输出目录 ---------------- */

/* 创建 /tmp/rkperf_YYYYmmdd_HHMMSS，三次测试不会互相覆盖 */
static std::string make_output_dir()
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);

    char dir[80];
    strftime(dir, sizeof(dir), OUT_ROOT "/rkperf_%Y%m%d_%H%M%S", &tm);

    if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "mkdir %s: %s\n", dir, strerror(errno));
        return std::string();
    }
    return std::string(dir);
}

/* ---------------- NPU 使用率采样 ---------------- */
static std::atomic<bool> g_npu_stop{false};

/* 解析 "NPU load:  Core0: 45%, Core1:  0%, Core2:  0%," */
static bool parse_npu_load(const char *buf, int v[3])
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

/* 每秒读一次 /sys/kernel/debug/rknpu/load（驱动统计窗口就是 1 秒） */
static void npu_sampler()
{
    bool warned = false;
    while (!g_npu_stop.load()) {
        FILE *f = fopen(NPU_LOAD, "r");
        if (!f) {
            if (!warned) {
                rklog::warn(std::string("npu sampler: 打不开 ") + NPU_LOAD +
                            " (" + strerror(errno) + ")，需要 root");
                warned = true;
            }
        } else {
            char buf[256] = {0};
            if (fgets(buf, sizeof(buf), f)) {
                int v[3];
                if (parse_npu_load(buf, v))
                    rklog::write_npu((uint64_t)(mono_seconds() * 1e9),
                                     iso_now().c_str(), v[0], v[1], v[2]);
            }
            fclose(f);
        }
        for (int i = 0; i < 10 && !g_npu_stop.load(); i++)
            usleep(100000);
    }
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

/* 返回 libc 路径（用于挂 ioctl 探针） */
static std::string find_libc()
{
    const char *cands[] = {
        "/lib/aarch64-linux-gnu/libc.so.6",
        "/usr/lib/aarch64-linux-gnu/libc.so.6",
        "/lib64/libc.so.6",
        "/lib/libc.so.6",
    };
    for (const char *c : cands)
        if (access(c, R_OK) == 0) return std::string(c);

    std::vector<std::string> v;
    glob_add(v, "/lib/*/libc.so.6");
    glob_add(v, "/usr/lib/*/libc.so.6");
    return v.empty() ? std::string() : v[0];
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

    /* 4. 每个库都挂探针（只挂子进程） */
    std::vector<struct bpf_link *> links;
    int attached = 0;
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
        attached++;

        /* 可选：rknn_set_core_mask，记录应用显式设置的核 */
        unsigned long long off_cm = 0;
        struct bpf_program *pcm = bpf_object__find_program_by_name(obj, "rknn_set_core_mask_enter");
        if (pcm && elf_sym_offset(t.path.c_str(), "rknn_set_core_mask", &off_cm) == 0) {
            struct bpf_link *lcm = bpf_program__attach_uprobe(pcm, false, child,
                                                              t.path.c_str(), off_cm);
            if (lcm)
                links.push_back(lcm);
        }
    }
    if (attached == 0) {
        rklog::error("attach uprobe failed on all targets");
        kill(child, SIGKILL);
        waitpid(child, nullptr, 0);
        return 1;
    }

    /* 4b. 挂 libc 的 ioctl 探针，用来抓驱动回写的 core_mask */
    std::string libc = find_libc();
    if (!libc.empty()) {
        unsigned long long off = 0;
        struct bpf_program *pin  = bpf_object__find_program_by_name(obj, "ioctl_enter");
        struct bpf_program *pout = bpf_object__find_program_by_name(obj, "ioctl_exit");
        if (pin && pout && elf_sym_offset(libc.c_str(), "ioctl", &off) == 0) {
            struct bpf_link *a = bpf_program__attach_uprobe(pin,  false, child, libc.c_str(), off);
            struct bpf_link *b = bpf_program__attach_uprobe(pout, true,  child, libc.c_str(), off);
            if (a && b) {
                links.push_back(a);
                links.push_back(b);
                rklog::info("ioctl probe: " + libc + " @" + hex_u64(off));
            } else {
                if (a) bpf_link__destroy(a);
                if (b) bpf_link__destroy(b);
                rklog::warn("ioctl probe attach failed, core_mask will be unknown");
            }
        } else {
            rklog::warn("ioctl symbol not found in " + libc);
        }
    } else {
        rklog::warn("libc not found, core_mask will be unknown");
    }

    rklog::info("attached " + std::to_string(attached) + " target(s)");

    /* 启动 NPU 使用率采样 */
    std::thread npu_thread(npu_sampler);

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

    /* 停止采样线程 */
    g_npu_stop = true;
    if (npu_thread.joinable())
        npu_thread.join();

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

    std::string dir = make_output_dir();
    if (dir.empty())
        return 1;

    if (!rklog::init(dir + "/rkperf.log", dir + "/events.csv", dir + "/npu.csv"))
        return 1;

    rklog::info(std::string("rkperf start: target=") + argv[1]);
    rklog::info("output dir: " + dir);

    int rc = do_trace(&argv[1]);

    rklog::info("output: " + dir);
    rklog::flush();
    rklog::shutdown();
    return rc;
}
