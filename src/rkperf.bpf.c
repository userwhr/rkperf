// rkperf.bpf.c —— 测量进程在 rknn_run 里的耗时
//
// 目标板内核未开 CONFIG_KPROBES，也没有 BTF，所以只用 uprobe/uretprobe，
// 不使用 CO-RE，pt_regs 手工定义。
//
// 关键点：入口能读到的参数（ctx / extend->non_block）必须在入口存下来，
// 因为返回时 x1 已被函数体覆盖（caller-saved 寄存器）。

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

/* 入口时保存的信息 */
struct entry_data {
    __u64 ts;         /* 入口时间戳 (ns) */
    __u64 ctx;        /* rknn_context */
    __s32 non_block;  /* 1 = 异步调用 */
    __s32 pad;
};

/* 送到用户态的事件 */
struct rk_event {
    __u64 ts_in;
    __u64 dur_ns;
    __u64 ctx;
    __u32 pid;
    __u32 tid;
    __s32 ret;
    __s32 non_block;
    char  comm[TASK_COMM_LEN];
};

struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __type(key, __u32);
    __type(value, struct entry_data);
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

    struct entry_data d = {};
    d.ts = bpf_ktime_get_ns();

    /* x0 = rknn_context */
    d.ctx = PT_REGS_PARM1(regs);

    /* x1 = rknn_run_extend*，非空时读 offset 8 的 non_block */
    __u64 ext = PT_REGS_PARM2(regs);
    if (ext) {
        __s32 nb = 0;
        if (bpf_probe_read_user(&nb, sizeof(nb), (void *)(ext + 8)) == 0)
            d.non_block = nb;
    }

    bpf_map_update_elem(&start, &tid, &d, BPF_ANY);
    return 0;
}

SEC("uretprobe//usr/lib/librknnrt.so:rknn_run")
int rknn_run_exit(struct user_pt_regs *regs)
{
    __u32 tid = (__u32)bpf_get_current_pid_tgid();
    struct entry_data *d = bpf_map_lookup_elem(&start, &tid);
    if (!d)
        return 0;

    struct rk_event e = {};
    e.ts_in     = d->ts;
    e.dur_ns    = bpf_ktime_get_ns() - d->ts;
    e.ctx       = d->ctx;
    e.non_block = d->non_block;
    e.pid       = bpf_get_current_pid_tgid() >> 32;
    e.tid       = tid;
    e.ret       = (__s32)PT_REGS_RC(regs);   /* x0 = 返回值 */

    bpf_get_current_comm(&e.comm, sizeof(e.comm));
    bpf_perf_event_output(regs, &events, BPF_F_CURRENT_CPU, &e, sizeof(e));
    bpf_map_delete_elem(&start, &tid);
    return 0;
}

char LICENSE[] SEC("license") = "GPL";
