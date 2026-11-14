// rkperf.cpp —— 采集进程在 rknn_run 里的耗时（spdlog 异步日志版）
//
// 用法:
//   sudo rkperf run [-o rkperf.log] [-l /usr/lib/librknnrt.so] [-s rknn_run] -- <程序> [参数...]
//
// 设计要点（保证不影响被测数据）：
//   1. 时间戳在 BPF（内核）侧采集，用户态日志再慢也改不了测量值
//   2. spdlog async_logger：采集线程只做 snprintf + 入队，落盘由独立线程完成
//   3. 队列满时 discard_new，绝不阻塞采集线程
//   4. perf buffer 放大到 256KB，避免采集线程被日志拖慢后丢事件

#define _GNU_SOURCE
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <unistd.h>
#include <csignal>
#include <getopt.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/types.h>

#include <libelf.h>
#include <gelf.h>

#include <bpf/libbpf.h>
#include <bpf/libbpf_common.h>
#include <bpf/bpf.h>

#include <spdlog/spdlog.h>
#include <spdlog/async.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/rotating_file_sink.h>

#define DEFAULT_LIB  "/usr/lib/librknnrt.so"
#define DEFAULT_SYM  "rknn_run"
#define DEFAULT_LOG  "rkperf.log"

#define PERF_PAGES   64        /* 64 * 4KB = 256KB */
#define QUEUE_SIZE   8192
#define LOG_MAX_SIZE (16 * 1024 * 1024)
#define LOG_MAX_FILES 3

struct rk_event {
    unsigned long long ts_in;
    unsigned long long dur_ns;
    unsigned long long ctx;
    unsigned int  pid;
    unsigned int  tid;
    int  ret;
    int  non_block;
    char comm[16];
};

static volatile sig_atomic_t g_stop = 0;
static void on_signal(int sig) { (void)sig; g_stop = 1; }

static std::shared_ptr<spdlog::logger> g_csv;
static std::shared_ptr<spdlog::logger> g_log;

/* 符号名 -> ELF 文件偏移 */
static int elf_sym_offset(const char *path, const char *sym, unsigned long long *out)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) { g_log->error("open {}: {}", path, strerror(errno)); return -1; }

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

static void on_event(void *ctx, int cpu, void *data, __u32 size)
{
    (void)ctx; (void)cpu;
    if (size < sizeof(struct rk_event)) return;

    const struct rk_event *e = (const struct rk_event *)data;
    double ms = e->dur_ns / 1000000.0;

    /* 采集线程只做 snprintf + 入队；落盘在 spdlog 的异步线程里 */
    char buf[256];
    int n = snprintf(buf, sizeof(buf), "%llu,%llu,%.3f,%u,%u,0x%llx,%d,%d,%s",
                     e->ts_in, e->dur_ns, ms, e->pid, e->tid, e->ctx,
                     e->ret, e->non_block, e->comm);
    if (n > 0)
        g_csv->log(spdlog::level::info, spdlog::string_view_t(buf, (size_t)n));

    g_log->info("rknn_run pid={} tid={} ctx=0x{:x} {} dur={:.3f} ms ret={} comm={}",
                e->pid, e->tid, e->ctx, e->non_block ? "ASYNC" : "SYNC ",
                ms, e->ret, e->comm);
}

static void on_lost(void *ctx, int cpu, unsigned long long cnt)
{
    (void)ctx;
    g_log->warn("lost {} events on cpu {}", cnt, cpu);
}

static void usage(const char *prog)
{
    printf(
        "Usage:\n"
        "  %s run [options] -- <program> [args...]\n"
        "\n"
        "Options:\n"
        "  -o, --out FILE   日志文件（默认 %s）\n"
        "  -l, --lib PATH   librknnrt.so 路径（默认 %s）\n"
        "  -s, --sym NAME   探针符号（默认 %s）\n"
        "  -v, --verbose    输出 DEBUG 日志\n"
        "  -h, --help       帮助\n",
        prog, DEFAULT_LOG, DEFAULT_LIB, DEFAULT_SYM);
}

int main(int argc, char **argv)
{
    if (argc < 2) { usage(argv[0]); return 1; }
    if (strcmp(argv[1], "run") != 0) { usage(argv[0]); return 1; }

    const char *log_path = DEFAULT_LOG;
    const char *lib_path = DEFAULT_LIB;
    const char *sym = DEFAULT_SYM;
    int verbose = 0;

    static struct option opts[] = {
        {"out",     required_argument, 0, 'o'},
        {"lib",     required_argument, 0, 'l'},
        {"sym",     required_argument, 0, 's'},
        {"verbose", no_argument,       0, 'v'},
        {"help",    no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    optind = 2;
    int c;
    while ((c = getopt_long(argc, argv, "o:l:s:vh", opts, nullptr)) != -1) {
        switch (c) {
        case 'o': log_path = optarg; break;
        case 'l': lib_path = optarg; break;
        case 's': sym = optarg; break;
        case 'v': verbose = 1; break;
        case 'h': usage(argv[0]); return 0;
        default:  usage(argv[0]); return 1;
        }
    }
    if (optind >= argc) { fprintf(stderr, "缺少目标程序\n"); usage(argv[0]); return 1; }
    char **child_argv = &argv[optind];

    /* ---------- 日志：spdlog 异步 ---------- */
    try {
        spdlog::init_thread_pool(QUEUE_SIZE, 1);

        auto log_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            log_path, LOG_MAX_SIZE, LOG_MAX_FILES);
        auto err_sink = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
        err_sink->set_level(spdlog::level::warn);
        std::vector<spdlog::sink_ptr> sinks{log_sink, err_sink};

        g_log = std::make_shared<spdlog::async_logger>(
            "rkperf", sinks.begin(), sinks.end(),
            spdlog::thread_pool(), spdlog::async_overflow_policy::discard_new);
        g_log->set_level(verbose ? spdlog::level::debug : spdlog::level::info);
        g_log->set_pattern("[%Y-%m-%dT%H:%M:%S.%e%z] [%l] %v");
        g_log->flush_on(spdlog::level::warn);

        /* CSV 单独一个文件，只写数据行 */
        std::string csv_path = std::string(log_path) + ".csv";
        auto csv_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            csv_path, LOG_MAX_SIZE, LOG_MAX_FILES);
        g_csv = std::make_shared<spdlog::async_logger>(
            "csv", csv_sink, spdlog::thread_pool(),
            spdlog::async_overflow_policy::discard_new);
        g_csv->set_level(spdlog::level::info);
        g_csv->set_pattern("%v");
        g_csv->info("# ts_in_ns,dur_ns,dur_ms,pid,tid,ctx,ret,non_block,comm");
    } catch (const spdlog::spdlog_ex &ex) {
        fprintf(stderr, "spdlog init failed: %s\n", ex.what());
        return 1;
    }

    g_log->info("rkperf start: lib={} sym={} log={}", lib_path, sym, log_path);

    /* 1. 符号偏移 */
    unsigned long long offset = 0;
    if (elf_sym_offset(lib_path, sym, &offset) != 0) {
        g_log->error("symbol {} not found in {}", sym, lib_path);
        spdlog::shutdown();
        return 1;
    }
    g_log->info("symbol {} -> file offset 0x{:x}", sym, offset);

    /* 2. 内嵌的 BPF 对象 */
    extern const unsigned char _binary_rkperf_bpf_o_start[];
    extern const unsigned char _binary_rkperf_bpf_o_end[];
    size_t bpf_sz = (size_t)(_binary_rkperf_bpf_o_end - _binary_rkperf_bpf_o_start);

    struct bpf_object *obj = bpf_object__open_mem(_binary_rkperf_bpf_o_start, bpf_sz, nullptr);
    if (!obj) { g_log->error("open embedded bpf object failed: {}", strerror(errno)); return 1; }
    g_log->info("embedded bpf object: {} bytes", bpf_sz);

    if (bpf_object__load(obj) != 0) {
        g_log->error("load bpf object failed（需要 root）");
        return 1;
    }

    struct bpf_program *prog_enter = bpf_object__find_program_by_name(obj, "rknn_run_enter");
    struct bpf_program *prog_exit  = bpf_object__find_program_by_name(obj, "rknn_run_exit");
    struct bpf_map *map_events = bpf_object__find_map_by_name(obj, "events");
    if (!prog_enter || !prog_exit || !map_events) {
        g_log->error("bpf program/map not found");
        return 1;
    }

    /* 3. fork + exec */
    pid_t child = fork();
    if (child < 0) { g_log->error("fork: {}", strerror(errno)); return 1; }
    if (child == 0) {
        setpgid(0, 0);
        execvp(child_argv[0], child_argv);
        fprintf(stderr, "execvp %s: %s\n", child_argv[0], strerror(errno));
        _exit(127);
    }
    g_log->info("child pid={} cmd={}", child, child_argv[0]);

    /* 4. attach uprobe */
    struct bpf_link *link_enter = bpf_program__attach_uprobe(prog_enter, false, child, lib_path, offset);
    struct bpf_link *link_exit  = bpf_program__attach_uprobe(prog_exit,  true,  child, lib_path, offset);
    if (!link_enter || !link_exit) {
        g_log->error("attach uprobe failed: {}", strerror(errno));
        kill(child, SIGKILL);
        return 1;
    }
    g_log->info("uprobe attached (pid={})", child);

    /* 5. perf buffer */
    struct perf_buffer_opts pb_opts = {
        .sample_cb = on_event,
        .lost_cb   = on_lost,
        .ctx       = nullptr,
    };
    struct perf_buffer *pb = perf_buffer__new(bpf_map__fd(map_events), PERF_PAGES, &pb_opts);
    if (!pb) { g_log->error("perf_buffer__new failed"); return 1; }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    /* 6. 采集循环 */
    int status = 0;
    while (!g_stop) {
        int err = perf_buffer__poll(pb, 100);
        if (err < 0 && err != -EINTR) { g_log->warn("poll: {}", err); break; }
        pid_t r = waitpid(child, &status, WNOHANG);
        if (r == child) {
            perf_buffer__poll(pb, 200);
            break;
        }
    }
    if (g_stop) kill(child, SIGTERM);
    waitpid(child, &status, 0);

    g_log->info("child exited, status={}", WIFEXITED(status) ? WEXITSTATUS(status) : -1);

    perf_buffer__free(pb);
    bpf_link__destroy(link_enter);
    bpf_link__destroy(link_exit);
    bpf_object__close(obj);

    g_log->flush();
    g_csv->flush();
    spdlog::shutdown();
    return 0;
}
