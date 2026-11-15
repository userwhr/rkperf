// rkperf.cpp —— 采集进程在 rknn_run 里的耗时
//
// 用法:
//   sudo rkperf <程序> [参数...]
//
// 固定的东西:
//   目标库 /usr/lib/librknnrt.so，符号 rknn_run
//   日志   /tmp/rkperf.log 和 /tmp/rkperf.log.csv
//
// 落盘逻辑在 rklog.cpp，这里只负责：解析符号 -> 加载 BPF -> 拉起进程 ->
// 挂探针 -> 收事件。

#define _GNU_SOURCE
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <string>
#include <unistd.h>
#include <csignal>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/types.h>

#include <libelf.h>
#include <gelf.h>

#include <bpf/libbpf.h>
#include <bpf/libbpf_common.h>
#include <bpf/bpf.h>

#include "rk_event.h"
#include "rklog.h"

#define LIB_PATH   "/usr/lib/librknnrt.so"
#define SYM_NAME   "rknn_run"
#define LOG_PATH   "/tmp/rkperf.log"
#define CSV_PATH   "/tmp/rkperf.log.csv"

#define PERF_PAGES 64

static volatile sig_atomic_t g_stop = 0;
static void on_signal(int sig) { (void)sig; g_stop = 1; }

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
    if (fd < 0) { rklog::error("open " + std::string(path) + ": " + strerror(errno)); return -1; }

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

/* ---------------- 主流程 ---------------- */

static int do_trace(char **child_argv)
{
    /* 1. 符号偏移 */
    unsigned long long offset = 0;
    if (elf_sym_offset(LIB_PATH, SYM_NAME, &offset) != 0) {
        rklog::error("symbol " SYM_NAME " not found in " LIB_PATH);
        return 1;
    }
    char off_hex[32];
    snprintf(off_hex, sizeof(off_hex), "0x%llx", offset);
    rklog::info(std::string("symbol " SYM_NAME " -> file offset ") + off_hex);

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
    rklog::info("child pid=" + std::to_string(child));

    /* 4. attach uprobe（只挂子进程） */
    struct bpf_link *link_enter = bpf_program__attach_uprobe(prog_enter, false, child, LIB_PATH, offset);
    struct bpf_link *link_exit  = bpf_program__attach_uprobe(prog_exit,  true,  child, LIB_PATH, offset);
    if (!link_enter || !link_exit) {
        rklog::error("attach uprobe failed: " + std::string(strerror(errno)));
        kill(child, SIGKILL);
        return 1;
    }

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
    while (!g_stop) {
        int err = perf_buffer__poll(pb, 100);
        if (err < 0 && err != -EINTR) { rklog::warn("poll: " + std::to_string(err)); break; }
        if (waitpid(child, &status, WNOHANG) == child) {
            perf_buffer__poll(pb, 200);   /* 收完残余事件 */
            break;
        }
    }
    if (g_stop) kill(child, SIGTERM);
    waitpid(child, &status, 0);

    rklog::info("child exited, status=" +
                std::to_string(WIFEXITED(status) ? WEXITSTATUS(status) : -1));

    perf_buffer__free(pb);
    bpf_link__destroy(link_enter);
    bpf_link__destroy(link_exit);
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

    rklog::info(std::string("rkperf start: target=") + argv[1] + " lib=" LIB_PATH);

    int rc = do_trace(&argv[1]);

    rklog::info(std::string("log: " LOG_PATH "  csv: " CSV_PATH));
    rklog::flush();
    rklog::shutdown();
    return rc;
}
