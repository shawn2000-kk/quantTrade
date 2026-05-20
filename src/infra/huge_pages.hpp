#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include "common/types.hpp"

// ============================================================
// huge_pages.hpp — 大页内存分配工具
//
// Linux:  优先使用 mmap(MAP_HUGETLB)；失败时 fallback 到普通匿名 mmap。
// macOS:  MAP_HUGETLB 不存在，直接使用 mmap(MAP_ANONYMOUS | MAP_PRIVATE)。
// ============================================================

namespace hft {

// ------------------------------------------------------------
// 基础分配 / 释放函数
// ------------------------------------------------------------

/// 分配至少 size 字节的大页内存（2 MB THP）。
/// 内部先尝试 MAP_HUGETLB，失败则 fallback 到普通匿名 mmap。
/// 返回对齐到 2MB 边界的指针；失败返回 nullptr。
[[nodiscard]] void* alloc_huge_pages(size_t size) noexcept;

/// 释放由 alloc_huge_pages 分配的内存。
/// ptr 必须是 alloc_huge_pages 的返回值，size 必须与分配时一致。
void free_huge_pages(void* ptr, size_t size) noexcept;

// ------------------------------------------------------------
// HugePageAllocator — 满足 C++ Named Requirements: Allocator
// ------------------------------------------------------------

template<typename T>
class HugePageAllocator {
public:
    using value_type      = T;
    using pointer         = T*;
    using const_pointer   = const T*;
    using size_type       = std::size_t;
    using difference_type = std::ptrdiff_t;

    // propagate_on_container_* — 默认即可（stateless allocator）
    using propagate_on_container_copy_assignment = std::true_type;
    using propagate_on_container_move_assignment = std::true_type;
    using propagate_on_container_swap            = std::true_type;
    using is_always_equal                        = std::true_type;

    template<typename U>
    struct rebind { using other = HugePageAllocator<U>; };

    HugePageAllocator() noexcept = default;

    template<typename U>
    HugePageAllocator(const HugePageAllocator<U>&) noexcept {}

    [[nodiscard]] T* allocate(size_type n) {
        const size_type bytes = n * sizeof(T);
        void* p = alloc_huge_pages(bytes);
        if (!p) {
            // -fno-exceptions 环境下不能 throw；返回 nullptr 会触发调用方 UB，
            // 此处遵循 C++ Allocator 合约：分配失败必须不返回（抛异常或终止）。
            // 由于禁用异常，使用 __builtin_trap() 令程序立即崩溃，让调用方在
            // 生产前确保内存充足。
            __builtin_trap();
        }
        return static_cast<T*>(p);
    }

    void deallocate(T* p, size_type n) noexcept {
        free_huge_pages(static_cast<void*>(p), n * sizeof(T));
    }

    [[nodiscard]] size_type max_size() const noexcept {
        return std::numeric_limits<size_type>::max() / sizeof(T);
    }
};

// Allocator equality（stateless，所有实例相等）
template<typename T, typename U>
[[nodiscard]] bool operator==(const HugePageAllocator<T>&,
                               const HugePageAllocator<U>&) noexcept { return true; }

template<typename T, typename U>
[[nodiscard]] bool operator!=(const HugePageAllocator<T>&,
                               const HugePageAllocator<U>&) noexcept { return false; }

} // namespace hft
