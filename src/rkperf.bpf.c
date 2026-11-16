// rkperf.bpf.c —— 测量进程在 rknn_run 里的耗时，并记录实际使用的 NPU 核
//
// 目标板内核未开 CONFIG_KPROBES，也没有 BTF，所以只用 uprobe/uretprobe。
//
// 两个观测点：
//   1. rknn_run 入口/返回 —— 测耗时
//   2. libc 的 ioctl 入口/返回 —— 抓驱动回写的 core_mask
//
// 为什么能从 ioctl 拿到核：
//   驱动 rknpu_job_schedule() 在 AUTO 模式下选完核后会把结果写回
//   job->args->core_mask；同步模式下 job->args 直接指向 ioctl 的 kdata，
//   DRM 框架在 ioctl 返回时把 kdata 拷回用户态。
//   所以 ioctl 返回后，用户态 struct rknpu_submit 的 core_mask 就是实际用的核。

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

/* ioctl 命令号（_IOWR(dir, nr, size)） */
#define RKNPU_DRM_SUBMIT  0xC0686441u   /* _IOWR('d', 0x41, 104) */
#define RKNPU_MISC_SUBMIT 0xC0687201u   /* _IOWR('r', 0x01, 104) */

/* struct rknpu_submit 里 core_mask 的偏移 */
#define SUBMIT_CORE_MASK_OFF 56

/* 入口时保存的信息 */
struct entry_data {
    __u64 ts;
    __u64 ctx;
    __s32 non_block;
    __u32 req_mask;   /* 应用通过 rknn_set_core_mask 请求的核 */
};

/* 送到用户态的事件（与 rk_event.h 一一对应） */
struct rk_event {
    __u64 ts_in;
    __u64 dur_ns;
    __u64 ctx;
    __u32 pid;
    __u32 tid;
    __s32 ret;
    __s32 non_block;
    __u32 core_mask;
    __u32 pad;
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

/* ioctl 入口时记下 arg 指针 */
struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __type(key, __u32);
    __type(value, __u64);
    __uint(max_entries, 1024);
} ioctl_arg SEC(".maps");

/* ioctl 返回时读到的 core_mask */
struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __type(key, __u32);
    __type(value, __u32);
    __uint(max_entries, 1024);
} core_mask_map SEC(".maps");

/* rknn_set_core_mask(ctx, mask) 记录的请求值 */
struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __type(key, __u64);
    __type(value, __u32);
    __uint(max_entries, 256);
} ctx_mask SEC(".maps");

/* ---------------- rknn_set_core_mask ---------------- */

SEC("uprobe//usr/lib/librknnrt.so:rknn_set_core_mask")
int rknn_set_core_mask_enter(struct user_pt_regs *regs)
{
    __u64 ctx  = PT_REGS_PARM1(regs);
    __u32 mask = (__u32)PT_REGS_PARM2(regs);
    bpf_map_update_elem(&ctx_mask, &ctx, &mask, BPF_ANY);
    return 0;
}

/* ---------------- rknn_run ---------------- */

SEC("uprobe//usr/lib/librknnrt.so:rknn_run")
int rknn_run_enter(struct user_pt_regs *regs)
{
    __u32 tid = (__u32)bpf_get_current_pid_tgid();

    struct entry_data d = {};
    d.ts = bpf_ktime_get_ns();
    d.ctx = PT_REGS_PARM1(regs);

    /* 应用是否显式设过核 */
    __u32 *req = bpf_map_lookup_elem(&ctx_mask, &d.ctx);
    if (req)
        d.req_mask = *req;

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
    e.ret       = (__s32)PT_REGS_RC(regs);

    /* 优先用驱动回写的实际核，没有就用应用请求的核 */
    __u32 *cm = bpf_map_lookup_elem(&core_mask_map, &tid);
    e.core_mask = cm ? *cm : d->req_mask;

    bpf_get_current_comm(&e.comm, sizeof(e.comm));
    bpf_perf_event_output(regs, &events, BPF_F_CURRENT_CPU, &e, sizeof(e));

    bpf_map_delete_elem(&start, &tid);
    bpf_map_delete_elem(&core_mask_map, &tid);
    return 0;
}

/* ---------------- ioctl（抓 core_mask） ---------------- */

SEC("uprobe//lib/aarch64-linux-gnu/libc.so.6:ioctl")
int ioctl_enter(struct user_pt_regs *regs)
{
    __u32 cmd = (__u32)PT_REGS_PARM2(regs);
    if (cmd != RKNPU_DRM_SUBMIT && cmd != RKNPU_MISC_SUBMIT)
        return 0;

    __u32 tid = (__u32)bpf_get_current_pid_tgid();
    __u64 arg = PT_REGS_PARM3(regs);
    bpf_map_update_elem(&ioctl_arg, &tid, &arg, BPF_ANY);
    return 0;
}

SEC("uretprobe//lib/aarch64-linux-gnu/libc.so.6:ioctl")
int ioctl_exit(struct user_pt_regs *regs)
{
    __u32 tid = (__u32)bpf_get_current_pid_tgid();
    __u64 *argp = bpf_map_lookup_elem(&ioctl_arg, &tid);
    if (!argp)
        return 0;

    __u32 mask = 0;
    if (bpf_probe_read_user(&mask, sizeof(mask),
                            (void *)(*argp + SUBMIT_CORE_MASK_OFF)) == 0)
        bpf_map_update_elem(&core_mask_map, &tid, &mask, BPF_ANY);

    bpf_map_delete_elem(&ioctl_arg, &tid);
    return 0;
}

char LICENSE[] SEC("license") = "GPL";
