#include "numa.hpp"

#include <cstdlib>   // aligned_alloc, free
#include <cstring>   // strncmp
#include <cstdio>    // fopen, fclose, fscanf, sscanf
#include "common/types.hpp"  // CACHELINE_SIZE

#ifdef HAVE_NUMA
#  include <numa.h>
#endif

namespace hft {

// ------------------------------------------------------------
// alloc_on_node
// ------------------------------------------------------------
void* alloc_on_node(size_t size, int node) noexcept {
    if (size == 0) return nullptr;

    // 确保至少 cacheline 对齐
    const size_t alignment = CACHELINE_SIZE; // 64

    // 将 size 向上对齐到 alignment 的倍数（aligned_alloc 要求）
    const size_t aligned_size = (size + alignment - 1) & ~(alignment - 1);

#ifdef HAVE_NUMA
    // libnuma 路径
    if (::numa_available() >= 0) {
        const int max_node = ::numa_max_node();
        const int target   = (node < 0 || node > max_node) ? 0 : node;
        void* p = ::numa_alloc_onnode(aligned_size, target);
        return p; // nullptr on failure
    }
    // numa_available() < 0: 系统无 NUMA，fallback
#else
    (void)node; // suppress unused warning
#endif

    // fallback: posix_memalign（aligned_alloc 要求 size 是 alignment 的倍数）
    void* p = nullptr;
    if (::posix_memalign(&p, alignment, aligned_size) != 0) return nullptr;
    return p;
}

// ------------------------------------------------------------
// free_numa
// ------------------------------------------------------------
void free_numa(void* ptr, size_t size) noexcept {
    if (!ptr) return;

#ifdef HAVE_NUMA
    if (::numa_available() >= 0 && size > 0) {
        const size_t alignment    = CACHELINE_SIZE;
        const size_t aligned_size = (size + alignment - 1) & ~(alignment - 1);
        ::numa_free(ptr, aligned_size);
        return;
    }
#else
    (void)size;
#endif

    ::free(ptr);
}

// ------------------------------------------------------------
// get_current_numa_node
// ------------------------------------------------------------
int get_current_numa_node() noexcept {
#ifdef __linux__
    // 从 /proc/self/status 读取 "Ngid:" 字段（NUMA group id）
    // 注意：该字段在内核 3.13+ 存在，在非 NUMA 系统上值为 0。
    FILE* f = ::fopen("/proc/self/status", "r");
    if (!f) return 0;

    char line[128];
    int  node = 0;
    while (::fgets(line, sizeof(line), f)) {
        if (::strncmp(line, "Ngid:", 5) == 0) {
            ::sscanf(line + 5, "%d", &node);
            break;
        }
    }
    ::fclose(f);
    return node;
#else
    // macOS: NUMA 概念不适用，始终返回 0
    return 0;
#endif
}

} // namespace hft
