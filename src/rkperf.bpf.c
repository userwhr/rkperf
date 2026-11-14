// rkperf.bpf.c —— 第一个 feature: 测量进程在 rknn_run 里的耗时
//
// 注意：目标板内核未开 CONFIG_KPROBES，也没有 BTF，
// 所以这里只用 uprobe/uretprobe，且不使用 CO-RE，pt_regs 手工定义。

#include <linux/types.h>
#include <linux/bpf.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>

/* arm64 的寄存器布局（bpf_tracing.h 期望这个名字） */
struct user_pt_regs {
    unsigned long regs[31];
    unsigned long sp;
    unsigned long pc;
    unsigned long pstate;
};

#define MAX_ENTRIES 4096
#define TASK_COMM_LEN 16

struct rk_event {
    __u64 ts_in;      /* 进入时间戳 (monotonic ns) */
    __u64 dur_ns;     /* 耗时 ns */
    __u64 ctx;        /* rknn_context（rknn_run 第 1 个参数） */
    __u32 pid;
    __u32 tid;
    __s32 ret;        /* 返回值（x0） */
    __s32 non_block;  /* 1 = 异步调用（extend->non_block） */
    char  comm[TASK_COMM_LEN];
};

struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __type(key, __u32);
    __type(value, __u64);
    __uint(max_entries, MAX_ENTRIES);
} start SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_PERF_EVENT_ARRAY);
    __uint(key_size, sizeof(__u32));
    __uint(value_size, sizeof(__u32));
} events SEC(".maps");

SEC("uprobe//usr/lib/librknnrt.so:rknn_run")
int rknn_run_enter(struct user_pt_regs *regs)
{
    __u32 tid = (__u32)bpf_get_current_pid_tgid();
    __u64 ts = bpf_ktime_get_ns();
    bpf_map_update_elem(&start, &tid, &ts, BPF_ANY);
    return 0;
}

SEC("uretprobe//usr/lib/librknnrt.so:rknn_run")
int rknn_run_exit(struct user_pt_regs *regs)
{
    __u32 tid = (__u32)bpf_get_current_pid_tgid();
    __u64 *t = bpf_map_lookup_elem(&start, &tid);
    if (!t)
        return 0;

    struct rk_event e = {};
    e.ts_in = *t;
    e.dur_ns = bpf_ktime_get_ns() - *t;
    e.ctx = PT_REGS_PARM1(regs);
    e.pid = bpf_get_current_pid_tgid() >> 32;
    e.tid = tid;
    e.ret = (__s32)PT_REGS_RC(regs);

    /* 第二个参数 rknn_run_extend*，非空时读 non_block（offset 8） */
    __u64 ext = PT_REGS_PARM2(regs);
    if (ext) {
        __s32 nb = 0;
        bpf_probe_read_user(&nb, sizeof(nb), (void *)(ext + 8));
        e.non_block = nb;
    }

    bpf_get_current_comm(&e.comm, sizeof(e.comm));
    bpf_perf_event_output(regs, &events, BPF_F_CURRENT_CPU, &e, sizeof(e));
    bpf_map_delete_elem(&start, &tid);
    return 0;
}

char LICENSE[] SEC("license") = "GPL";
