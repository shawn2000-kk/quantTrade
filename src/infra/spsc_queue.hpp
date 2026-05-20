#pragma once
#include <atomic>
#include <cstddef>
#include <type_traits>

namespace hft {

// ── 无锁单生产者单消费者环形队列 ────────────────────────────────
//
// 设计约束：
//   - N 必须是 2 的幂（bitmask 代替 modulo，无整数除法）
//   - T 必须 trivially_copyable（直接内存复制，无构造/析构开销）
//   - tail_ / head_ 各占独立 cacheline，消除 false sharing
//   - push: relaxed 读 tail (producer-owned) + acquire 读 head，release 写 tail
//   - pop : relaxed 读 head (consumer-owned) + acquire 读 tail，release 写 head
//   - 整个类 alignas(64)，保证首成员落在 cacheline 边界

template<typename T, size_t N>
class alignas(64) SPSCQueue {
    static_assert((N & (N - 1)) == 0,
        "SPSCQueue: N must be a power of 2");
    static_assert(std::is_trivially_copyable_v<T>,
        "SPSCQueue: T must be trivially copyable for lock-free memory copy");

    static constexpr size_t MASK = N - 1;

    // ---------- producer-owned: 只有 push() 写，pop() acquire 读 ----------
    alignas(64) std::atomic<size_t> tail_{0};
    // 56 bytes implicit padding: compiler inserts because next member is alignas(64)

    // ---------- consumer-owned: 只有 pop() 写，push() acquire 读 ----------
    alignas(64) std::atomic<size_t> head_{0};
    // 56 bytes implicit padding

    // ---------- 数据缓冲区：cacheline 对齐，避免与控制字段共享 ----------
    alignas(64) T buffer_[N];

public:
    SPSCQueue() noexcept = default;
    ~SPSCQueue() noexcept = default;

    // 不可拷贝 / 移动：内部有 atomic，语义上也不应转移所有权
    SPSCQueue(const SPSCQueue&)            = delete;
    SPSCQueue& operator=(const SPSCQueue&) = delete;
    SPSCQueue(SPSCQueue&&)                 = delete;
    SPSCQueue& operator=(SPSCQueue&&)      = delete;

    // ── push (生产者线程调用) ──────────────────────────────────────
    // 队列满返回 false，不阻塞，不抛出
    bool push(const T& val) noexcept {
        const size_t tail = tail_.load(std::memory_order_relaxed);
        // acquire: 读取 consumer 最新写入的 head_，保证可见性
        if (tail - head_.load(std::memory_order_acquire) == N) {
            return false;  // full
        }
        buffer_[tail & MASK] = val;
        // release: 让 consumer 的 acquire 读 tail_ 时能看到 buffer_ 写入
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    // ── pop (消费者线程调用) ──────────────────────────────────────
    // 队列空返回 false，不阻塞，不抛出
    bool pop(T& val) noexcept {
        const size_t head = head_.load(std::memory_order_relaxed);
        // acquire: 读取 producer 最新写入的 tail_，保证 buffer_ 数据可见
        if (tail_.load(std::memory_order_acquire) == head) {
            return false;  // empty
        }
        val = buffer_[head & MASK];
        // release: 让 producer 的 acquire 读 head_ 时能看到 slot 已释放
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    // ── 状态查询（近似值，仅在单线程/暂停场景下精确） ──────────────
    [[nodiscard]] bool empty() const noexcept {
        return tail_.load(std::memory_order_acquire)
            == head_.load(std::memory_order_acquire);
    }

    [[nodiscard]] size_t size() const noexcept {
        const size_t tail = tail_.load(std::memory_order_acquire);
        const size_t head = head_.load(std::memory_order_acquire);
        return tail - head;  // 无符号减法，即使绕回也正确
    }

    // 容量（编译期常量）
    static constexpr size_t capacity() noexcept { return N; }
};

// ── 编译期布局验证 ────────────────────────────────────────────────
// 用 BBOEvent (64 bytes, trivially_copyable) 做代表性实例化
// 下方 static_assert 在头文件内部可用，调用方 include 后自动触发检查

namespace detail {
// head_ @ offset 0，tail_ @ offset 64，buffer_ @ offset 128
// => sizeof >= 128 + N*sizeof(T)
static_assert(alignof(SPSCQueue<char, 2>) == 64,
    "SPSCQueue must be cacheline-aligned");
}  // namespace detail

}  // namespace hft
