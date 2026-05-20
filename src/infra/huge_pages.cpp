#include "huge_pages.hpp"

#include <sys/mman.h>
#include <unistd.h>
#include <cerrno>
#include <cstddef>

// MAP_HUGETLB 仅 Linux 定义
#ifndef MAP_HUGETLB
#  define MAP_HUGETLB 0  // macOS: 不存在该标志，置零后 fallback 路径仍可编译
#endif

namespace hft {

namespace {

// 2 MB 大页对齐大小
static constexpr size_t HUGE_PAGE_SIZE = 2UL * 1024 * 1024;

// 将 size 向上对齐到 align（align 必须是 2 的幂）
inline size_t align_up(size_t size, size_t align) noexcept {
    return (size + align - 1) & ~(align - 1);
}

} // anonymous namespace

void* alloc_huge_pages(size_t size) noexcept {
    if (size == 0) return nullptr;

    const size_t aligned_size = align_up(size, HUGE_PAGE_SIZE);

#ifdef __linux__
    // 先尝试大页
    void* p = ::mmap(nullptr, aligned_size,
                     PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB,
                     -1, 0);
    if (p != MAP_FAILED) return p;

    // fallback：普通匿名 mmap（透明大页 THP 仍可能生效）
    p = ::mmap(nullptr, aligned_size,
               PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS,
               -1, 0);
    return (p != MAP_FAILED) ? p : nullptr;

#else
    // macOS: MAP_HUGETLB 不支持，直接普通 mmap
    void* p = ::mmap(nullptr, aligned_size,
                     PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS,
                     -1, 0);
    return (p != MAP_FAILED) ? p : nullptr;
#endif
}

void free_huge_pages(void* ptr, size_t size) noexcept {
    if (!ptr || size == 0) return;
    const size_t aligned_size = align_up(size, HUGE_PAGE_SIZE);
    ::munmap(ptr, aligned_size);
}

} // namespace hft
