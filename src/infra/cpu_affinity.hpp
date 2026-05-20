#pragma once

#include <vector>
#include "common/types.hpp"

// ============================================================
// cpu_affinity.hpp — CPU 绑核 & 实时调度工具
//
// Linux only：调用 pthread_setaffinity_np / sched_setscheduler。
// macOS / 其他平台：所有函数返回 false 或 0（桩实现）。
// ============================================================

namespace hft {

/// 将当前线程绑定到指定核心 ID。
/// 成功返回 true，失败（权限不足 / core_id 越界等）返回 false。
bool pin_thread_to_core(int core_id) noexcept;

/// 将当前线程绑定到多个核心（CPU set）。
/// cores 为空时返回 false。
bool pin_thread_to_cores(const std::vector<int>& cores) noexcept;

/// 返回当前线程正在运行的 CPU 核心 ID。
/// 失败或平台不支持时返回 -1。
int get_current_core() noexcept;

/// 将当前线程设置为 SCHED_FIFO 实时调度。
/// priority: 1–99，默认 99（最高优先级）。
/// 需要 CAP_SYS_NICE 或以 root 运行。
/// 成功返回 true，失败返回 false。
bool set_thread_realtime(int priority = 99) noexcept;

} // namespace hft
