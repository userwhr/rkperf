#ifndef RKPERF_SAMPLE_H
#define RKPERF_SAMPLE_H

#include <atomic>
#include <thread>

#include <sys/types.h>

// NPU / DDR / 内存三个采样线程。
// 它们和 BPF 采集无关，只读 sysfs / procfs：
//   1) /sys/kernel/debug/rknpu/load        1 Hz    三核利用率
//   2) /sys/class/devfreq/dmc/load        10 Hz    DDR 占用率 + 频点
//   3) /proc/<pid>/status                0.2 Hz    VmRSS / VmHWM / 线程数
//
// 用法：start(pid) 起三个线程，stop() 置停止位并 join；析构函数兜底。
namespace rksample {

class Samplers {
public:
    void start(pid_t pid);
    void stop();
    ~Samplers() { stop(); }

    Samplers() = default;
    Samplers(const Samplers &) = delete;
    Samplers &operator=(const Samplers &) = delete;

private:
    void loop_npu();
    void loop_ddr();
    void loop_mem(pid_t pid);

    std::atomic<bool> stop_{false};
    std::thread npu_;
    std::thread ddr_;
    std::thread mem_;
};

}  // namespace rksample

#endif  // RKPERF_SAMPLE_H
