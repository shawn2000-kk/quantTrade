#pragma once

#include <cstddef>
#include "common/types.hpp"

// ============================================================
// numa.hpp — NUMA 感知内存分配工具
//
// 编译时通过 HAVE_NUMA 宏控制 libnuma 依赖：
//   - 定义了 HAVE_NUMA：调用 numa_alloc_onnode / numa_free
//   - 未定义 HAVE_NUMA：fallback 到 aligned_alloc(CACHELINE_SIZE, size)
//
// macOS / 非 NUMA 平台：所有函数均可编译，行为退化为普通分配。
// ============================================================

namespace hft {

// ------------------------------------------------------------
// 核心分配 / 释放函数
// ------------------------------------------------------------

/// 在指定 NUMA 节点上分配 size 字节内存，至少对齐到 64 字节（cacheline）。
/// node < 0 时等价于 alloc_on_node(size, 0)（使用节点 0）。
/// 失败返回 nullptr。
[[nodiscard]] void* alloc_on_node(size_t size, int node) noexcept;

/// 释放由 alloc_on_node 分配的内存。
/// size 需与分配时一致（libnuma 路径需要 size）。
void free_numa(void* ptr, size_t size) noexcept;

/// 返回当前线程/进程所在的 NUMA 节点 ID。
/// 通过 /proc/self/status (Ngid 字段) 读取；失败返回 0。
[[nodiscard]] int get_current_numa_node() noexcept;

} // namespace hft
