// rktrace.cpp —— 采集主流程
//
// 从 rkperf.cpp 拆出来的原因：入口只该管命令行、输出目录和 summary，
// 这里才是"找库 -> 挂探针 -> 收事件"的干货。
//
// 目标库自动发现（不写死路径）：系统路径 + Python 包里自带的副本，
// 找到几个挂几个，目标进程加载哪个就命中哪个。

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "rktrace.h"

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <glob.h>
#include <unistd.h>

#include <sys/types.h>
#include <sys/wait.h>

#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include <bpf/libbpf_common.h>
#include <gelf.h>
#include <libelf.h>

#include "rk_event.h"
#include "rklog.h"
#include "rkoplog.h"
#include "rksample.h"
#include "rksummary.h"
#include "rkutil.h"

// 由 `ld -r -b binary` 生成的符号。
// 必须声明在全局命名空间：放进匿名命名空间会变成内部链接，链接期找不到。
extern const unsigned char _binary_rkperf_bpf_o_start[];
extern const unsigned char _binary_rkperf_bpf_o_end[];

namespace {

using rkutil::hex_u64;
using rkutil::iso_now;
using rkutil::mono_seconds;

constexpr const char *kSymName = "rknn_run";
constexpr int kPerfPages = 64;

/* 一个待挂载的目标库 */
struct probe_target {
    std::string path;
    unsigned long long offset = 0;
};

volatile sig_atomic_t g_stop = 0;
void on_signal(int sig) { (void)sig; g_stop = 1; }

/* ---------------- 找 librknnrt.so ---------------- */

void add_unique(std::vector<std::string> &v, const std::string &p)
{
    if (p.empty()) return;
    if (access(p.c_str(), R_OK) != 0) return;
    for (const auto &x : v)
        if (x == p) return;
    v.push_back(p);
}

void glob_add(std::vector<std::string> &v, const std::string &pattern)
{
    glob_t g;
    if (glob(pattern.c_str(), GLOB_NOSORT, nullptr, &g) == 0) {
        for (size_t i = 0; i < g.gl_pathc; i++)
            add_unique(v, g.gl_pathv[i]);
    }
    globfree(&g);
}

/* 返回系统里所有可能的 librknnrt.so */
std::vector<std::string> find_rknn_libs()
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
std::string find_libc()
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

void on_event(void *ctx, int cpu, void *data, __u32 size)
{
    (void)ctx; (void)cpu;
    if (size < sizeof(rk_event)) return;
    const rk_event *e = static_cast<const rk_event *>(data);
    rklog::write_event(*e);
    rksummary::note_event(*e);
}

void on_lost(void *ctx, int cpu, unsigned long long cnt)
{
    (void)ctx;
    rklog::warn("lost " + std::to_string(cnt) + " events on cpu " + std::to_string(cpu));
}

/* ---------------- ELF 符号 -> 文件偏移 ---------------- */

int elf_sym_offset(const char *path, const char *sym, unsigned long long *out)
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

/* ---------------- 等子进程 exec 完 ---------------- */

/* comm 不再是 rkperf 就认为已经 execvp 成功；最多等 timeout_ms */
void wait_child_exec(pid_t pid, int timeout_ms)
{
    char path[64], comm[64] = {0};
    snprintf(path, sizeof(path), "/proc/%d/comm", (int)pid);

    int waited = 0;
    while (waited < timeout_ms) {
        FILE *f = fopen(path, "r");
        if (!f)
            return;                       /* 进程已经没了 */
        bool still_me = false;
        if (fgets(comm, sizeof(comm), f))
            still_me = (strncmp(comm, "rkperf", 6) == 0);
        fclose(f);
        if (!still_me)
            return;                       /* 已经是目标程序 */
        usleep(20000);
        waited += 20;
    }
}

/* 等 exec 完再抓 /proc —— SIGSTOP 期间子进程还是 rkperf 自己的副本。
   放到独立线程里做，主线程立刻进 poll 循环，避免开头那批事件把 perf buffer 撑爆。 */
void snapshot_after_exec(pid_t pid)
{
    wait_child_exec(pid, 2000);
    rksummary::attach_child(pid);
}

/* ---------------- 主流程 ---------------- */

int do_trace(char **child_argv, rkrun::outcome *out, const rktrace::options &opt)
{
    /* 1. 找所有 librknnrt.so 并解析符号偏移 */
    std::vector<probe_target> targets;
    for (const auto &lib : find_rknn_libs()) {
        unsigned long long off = 0;
        if (elf_sym_offset(lib.c_str(), kSymName, &off) != 0) {
            rklog::warn("skip " + lib + " (no " + kSymName + ")");
            continue;
        }
        targets.push_back({lib, off});
        rklog::info("target: " + lib + " @" + hex_u64(off));
    }
    if (targets.empty()) {
        rklog::error("no librknnrt.so found (looked in system paths and python packages)");
        return 1;
    }

    /* 2. 内嵌的 BPF 对象（符号在文件顶部声明） */
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

    /* 3. perf buffer —— 必须在 attach 之前建好，否则早期事件会丢 */
    struct perf_buffer_opts pb_opts = {
        .sample_cb = on_event,
        .lost_cb   = on_lost,
        .ctx       = nullptr,
    };
    struct perf_buffer *pb = perf_buffer__new(bpf_map__fd(map_events), kPerfPages, &pb_opts);
    if (!pb) { rklog::error("perf_buffer__new failed"); return 1; }

    /* 4. fork + exec：子进程先停住，等探针挂好再放行 */
    const bool op_time = opt.op_time && rkoplog::enabled();
    pid_t child = fork();
    if (child < 0) { rklog::error("fork: " + std::string(strerror(errno))); return 1; }
    if (child == 0) {
        setpgid(0, 0);
        raise(SIGSTOP);            /* 等父进程 attach 完 */

        /* --op-time：让 runtime 打逐层表（RKNN_LOG_LEVEL>=4 才有），
           并把 stdout 接到采集管道上。这两件事都只影响子进程。 */
        if (op_time) {
            const char *lv = getenv("RKNN_LOG_LEVEL");
            if (!lv || atoi(lv) < 4)
                setenv("RKNN_LOG_LEVEL", "4", 1);
            int w = rkoplog::child_fd();
            if (w >= 0) {
                dup2(w, STDOUT_FILENO);
                close(w);
            }
        }

        execvp(child_argv[0], child_argv);
        fprintf(stderr, "execvp %s: %s\n", child_argv[0], strerror(errno));
        _exit(127);
    }
    {
        int st = 0;
        waitpid(child, &st, WUNTRACED);   /* 等子进程真正停下来 */
    }

    /* 4b. 读线程先起来：子进程一放行就会打 init 期的日志，不能晚 */
    if (op_time)
        rkoplog::start();

    /* 5. 每个库都挂探针（只挂子进程） */
    std::vector<struct bpf_link *> links;
    std::vector<rksummary::probe_info> probes;
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
        probes.push_back(rksummary::probe_info{t.path, kSymName, t.offset});

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
        if (op_time) rkoplog::finish();
        return 1;
    }

    /* 5b. 挂 libc 的 ioctl 探针，用来抓驱动回写的 core_mask */
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
                probes.push_back(rksummary::probe_info{libc, "ioctl", off});
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

    /* 5c. summary：先把探针信息补进 config.json */
    rksummary::set_probes(probes);

    /* 6. 放行子进程，开始计时 */
    kill(child, SIGCONT);
    const double t_start = mono_seconds();
    const std::string t_start_iso = iso_now();
    rklog::info("process start: pid=" + std::to_string(child) +
                " at " + t_start_iso + " cmd=" + child_argv[0]);

    /* 7. 采样线程 + 信号处理 */
    rksample::Samplers samplers;
    samplers.start(child);

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    /* summary：/proc 快照放后台做（见 snapshot_after_exec） */
    std::thread snap_thread(snapshot_after_exec, child);

    /* 8. 采集循环 */
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

    /* 9. 收尾：SIGTERM -> 最多 2 秒 -> SIGKILL */
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

    /* 10. 结束时刻 + 总时长 */
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

    if (out) {
        out->ended_at    = t_end_iso;
        out->duration_s  = dur;
        out->exit_reason = reason;
        out->exit_code   = code;
        out->interrupted = g_stop != 0;
    }

    /* 11. 停采样、拆探针 */
    samplers.stop();
    if (snap_thread.joinable())
        snap_thread.join();

    perf_buffer__free(pb);
    for (auto *l : links) bpf_link__destroy(l);
    bpf_object__close(obj);

    /* 12. 逐层算子耗时收尾（读线程要等子进程的 stdout 关掉） */
    if (op_time) rkoplog::finish();
    return 0;
}

}  // namespace

namespace rktrace {

int run(char **child_argv, rkrun::outcome *out, const rktrace::options &opt)
{
    return do_trace(child_argv, out, opt);
}

}  // namespace rktrace
