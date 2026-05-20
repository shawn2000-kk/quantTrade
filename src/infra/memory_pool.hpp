#pragma once
#include <cstddef>
#include <cstdint>
#include <new>          // std::launder

namespace hft {

// ── 固定大小对象池（无锁，单线程） ────────────────────────────────
//
// 设计约束：
//   - 预分配 N 个 slot，storage_ 即全部内存，零额外堆分配
//   - free list 为侵入式单链表：空闲 slot 的头 sizeof(void*) 字节存储
//     指向下一个空闲 slot 的指针，无任何额外元数据内存
//   - acquire() 不调用构造函数，调用方负责 placement new（如需）
//   - release() 不调用析构函数，调用方负责手动析构（如需）
//   - 不可拷贝 / 移动
//   - 无锁设计：不含任何 mutex / atomic，适合单线程热路径

template<typename T, size_t N>
class MemoryPool {
    static_assert(N > 0, "MemoryPool: N must be > 0");
    static_assert(sizeof(T) >= sizeof(void*),
        "MemoryPool: sizeof(T) must be >= sizeof(void*) "
        "to store the intrusive free-list next pointer");

    // ── 内部 free-list 节点：覆盖在 slot 字节上 ─────────────────
    struct FreeNode {
        FreeNode* next;
    };

    // ── 预分配存储区：对齐到 T 的自然对齐，行间距恰好 sizeof(T) ──
    // alignas(alignof(T)) 保证第 0 slot 满足 T 的对齐要求；
    // 因 sizeof(T) 是 alignof(T) 的整数倍（C++ 标准保证），
    // 后续每个 slot[i] 的地址同样满足对齐。
    alignas(alignof(T)) std::byte storage_[N][sizeof(T)];

    FreeNode* free_head_{nullptr};
    size_t    available_{N};

public:
    // ── 构造：将所有 slot 串成侵入式单链表 ───────────────────────
    MemoryPool() noexcept {
        for (size_t i = 0; i < N - 1; ++i) {
            auto* node  = reinterpret_cast<FreeNode*>(&storage_[i]);
            node->next  = reinterpret_cast<FreeNode*>(&storage_[i + 1]);
        }
        reinterpret_cast<FreeNode*>(&storage_[N - 1])->next = nullptr;
        free_head_ = reinterpret_cast<FreeNode*>(&storage_[0]);
    }

    ~MemoryPool() noexcept = default;

    MemoryPool(const MemoryPool&)            = delete;
    MemoryPool& operator=(const MemoryPool&) = delete;
    MemoryPool(MemoryPool&&)                 = delete;
    MemoryPool& operator=(MemoryPool&&)      = delete;

    // ── acquire: 从 free list 取一块未构造的原始内存 ─────────────
    // 返回指向 slot 的指针（未初始化），池满返回 nullptr
    // 调用方通常随后执行 placement new：new(ptr) T{...}
    [[nodiscard]] T* acquire() noexcept {
        if (!free_head_) [[unlikely]] {
            return nullptr;  // pool exhausted
        }
        FreeNode* node = free_head_;
        free_head_     = node->next;
        --available_;
        return reinterpret_cast<T*>(node);
    }

    // ── release: 归还 slot，不调用析构函数 ──────────────────────
    // 调用方如果在 acquire() 后执行了 placement new，
    // 应在 release() 前手动调用析构：ptr->~T()
    void release(T* ptr) noexcept {
        // ptr 必须是此 pool 分配的地址，调用方保证
        auto* node = reinterpret_cast<FreeNode*>(ptr);
        node->next = free_head_;
        free_head_ = node;
        ++available_;
    }

    // ── 查询 ─────────────────────────────────────────────────────
    [[nodiscard]] size_t available() const noexcept { return available_; }
    [[nodiscard]] size_t capacity()  const noexcept { return N; }
    [[nodiscard]] bool   full()      const noexcept { return available_ == 0; }
    [[nodiscard]] bool   empty()     const noexcept { return available_ == N; }
};

}  // namespace hft
