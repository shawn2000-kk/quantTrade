#pragma once
#include <atomic>
#include <cstddef>
#include <type_traits>

namespace hft {

// ── 无锁多生产者单消费者环形队列 ─────────────────────────────────────
//
// 算法：Dmitry Vyukov MPMC 队列简化为 MPSC
//   - 每个 slot 含序列号（seq），用于判断槽位状态
//   - 生产者：load tail_ → CAS 抢占槽位 → 写数据 → seq = pos+1
//   - 消费者：不竞争，head_ 为普通变量（单线程独享）→ 等待 seq == head_+1
//   - 满时 push 返回 false（非阻塞），空时 pop 返回 false
//
// 与 SPSCQueue 的区别：
//   - tail_ 是共享 atomic，多个生产者 CAS 竞争
//   - head_ 无需 atomic（消费者唯一）→ 节省 cacheline 访问
//   - 每个 slot 独占 1 cacheline，消除 false sharing
//
// 适用场景：
//   - 多个交易线程 → 监控线程（延迟打点、日志缓冲）
//   - 多个策略 → OMS 汇总队列（MPSC 后 OMS 再分发）

template<typename T, size_t N>
class MPSCQueue {
    static_assert((N & (N - 1)) == 0,
        "MPSCQueue: N must be a power of 2");
    static_assert(std::is_trivially_copyable_v<T>,
        "MPSCQueue: T must be trivially copyable");

    static constexpr size_t MASK = N - 1;

    // 每个 slot 独占 cacheline：seq + data，避免 false sharing
    struct alignas(64) Slot {
        std::atomic<size_t> seq{};
        T                   data{};
    };

    // ---------- 生产者共享：多线程 CAS 竞争 ----------
    alignas(64) std::atomic<size_t> tail_{0};

    // ---------- 消费者私有：单线程，无需 atomic ----------
    alignas(64) size_t head_{0};

    // ---------- 数据槽位 ----------
    Slot slots_[N];

public:
    MPSCQueue() noexcept {
        // slot[i].seq 初始化为 i，表示该槽位"空闲，等待第 i 号生产者填入"
        for (size_t i = 0; i < N; ++i)
            slots_[i].seq.store(i, std::memory_order_relaxed);
    }

    ~MPSCQueue() noexcept = default;

    MPSCQueue(const MPSCQueue&)            = delete;
    MPSCQueue& operator=(const MPSCQueue&) = delete;
    MPSCQueue(MPSCQueue&&)                 = delete;
    MPSCQueue& operator=(MPSCQueue&&)      = delete;

    // ── push（多生产者线程调用，非阻塞）─────────────────────────────
    // 返回 false 表示队列满，不阻塞。
    bool push(const T& val) noexcept {
        size_t pos = tail_.load(std::memory_order_relaxed);
        Slot*  slot;

        for (;;) {
            slot        = &slots_[pos & MASK];
            size_t seq  = slot->seq.load(std::memory_order_acquire);
            auto   diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos);

            if (diff == 0) {
                // 槽位空闲，CAS 抢占
                if (tail_.compare_exchange_weak(pos, pos + 1,
                        std::memory_order_relaxed,
                        std::memory_order_relaxed))
                    break;
                // CAS 失败：被其他生产者抢占，重读 pos 重试
            } else if (diff < 0) {
                // diff < 0：槽位被 tail_ 环绕但 consumer 未跟上 → 队列满
                return false;
            } else {
                // diff > 0：其他生产者刚写完但还没更新 tail_（极短暂），重读
                pos = tail_.load(std::memory_order_relaxed);
            }
        }

        slot->data = val;
        // release：让消费者 acquire 读 seq 时能看到 data 写入
        slot->seq.store(pos + 1, std::memory_order_release);
        return true;
    }

    // ── pop（单消费者线程调用，非阻塞）──────────────────────────────
    // 返回 false 表示队列空。
    bool pop(T& val) noexcept {
        Slot*  slot = &slots_[head_ & MASK];
        size_t seq  = slot->seq.load(std::memory_order_acquire);
        auto   diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(head_ + 1);

        if (diff < 0) return false;  // 生产者还未写入

        val = slot->data;
        // release：归还槽位，seq = head_ + N 表示该槽可被下一轮生产者使用
        slot->seq.store(head_ + N, std::memory_order_release);
        ++head_;
        return true;
    }

    // ── 近似状态查询（仅供非热路径诊断使用）────────────────────────
    [[nodiscard]] bool empty() const noexcept {
        const size_t head = head_;
        const size_t tail = tail_.load(std::memory_order_acquire);
        return tail == head;
    }

    [[nodiscard]] size_t size() const noexcept {
        const size_t head = head_;
        const size_t tail = tail_.load(std::memory_order_acquire);
        return tail - head;
    }

    static constexpr size_t capacity() noexcept { return N; }
};

}  // namespace hft
