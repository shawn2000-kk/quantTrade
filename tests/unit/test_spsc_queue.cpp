// tests/unit/test_spsc_queue.cpp
// GoogleTest 单元测试：SPSCQueue<T, N>
//
// 编译示例（手动，无 CMake）：
//   c++ -std=c++20 -I src -pthread \
//       tests/unit/test_spsc_queue.cpp \
//       -lgtest -lgtest_main -o test_spsc_queue && ./test_spsc_queue

#include <gtest/gtest.h>
#include <atomic>
#include <thread>
#include <vector>
#include <cstdint>

#include "infra/spsc_queue.hpp"
#include "market_data/market_data_types.hpp"  // BBOEvent (64 bytes, trivially_copyable)
#include "oms/order.hpp"                       // OrderRequest (32 bytes, trivially_copyable)

namespace hft {

// ── 编译期约束验证 ─────────────────────────────────────────────────
// (1) N 不是 2 的幂应触发 static_assert —— 以下代码若取消注释将编译失败：
//   SPSCQueue<int, 3> bad_queue;  // static_assert: N must be a power of 2
// (2) T 非 trivially_copyable 应触发 static_assert：
//   struct NonTrivial { NonTrivial(const NonTrivial&) {} };
//   SPSCQueue<NonTrivial, 4> bad2;  // static_assert: T must be trivially copyable

// ── 基本正确性 ─────────────────────────────────────────────────────

TEST(SPSCQueueTest, InitiallyEmpty) {
    SPSCQueue<int, 8> q;
    EXPECT_TRUE(q.empty());
    EXPECT_EQ(q.size(), 0u);
}

TEST(SPSCQueueTest, PushAndPop) {
    SPSCQueue<int, 8> q;
    EXPECT_TRUE(q.push(42));
    EXPECT_FALSE(q.empty());
    EXPECT_EQ(q.size(), 1u);

    int val = 0;
    EXPECT_TRUE(q.pop(val));
    EXPECT_EQ(val, 42);
    EXPECT_TRUE(q.empty());
    EXPECT_EQ(q.size(), 0u);
}

TEST(SPSCQueueTest, FIFOOrdering) {
    SPSCQueue<int, 16> q;
    for (int i = 0; i < 10; ++i) {
        ASSERT_TRUE(q.push(i));
    }
    for (int i = 0; i < 10; ++i) {
        int val = -1;
        ASSERT_TRUE(q.pop(val));
        EXPECT_EQ(val, i) << "FIFO ordering violated at index " << i;
    }
}

// ── 边界：队列满 ────────────────────────────────────────────────────
// N=4 → 容量 4，第 5 次 push 应返回 false
TEST(SPSCQueueTest, PushReturnsFalseWhenFull) {
    SPSCQueue<int, 4> q;
    EXPECT_TRUE(q.push(1));
    EXPECT_TRUE(q.push(2));
    EXPECT_TRUE(q.push(3));
    EXPECT_TRUE(q.push(4));
    EXPECT_EQ(q.size(), 4u);

    // 第 5 次：队列已满
    EXPECT_FALSE(q.push(5));
    EXPECT_EQ(q.size(), 4u);  // 大小不变
}

// ── 边界：队列空 ────────────────────────────────────────────────────
TEST(SPSCQueueTest, PopReturnsFalseWhenEmpty) {
    SPSCQueue<int, 4> q;
    int val = -1;
    EXPECT_FALSE(q.pop(val));
    EXPECT_EQ(val, -1);  // val 未被修改

    // push 再 pop 再 pop
    ASSERT_TRUE(q.push(99));
    EXPECT_TRUE(q.pop(val));
    EXPECT_EQ(val, 99);
    EXPECT_FALSE(q.pop(val));  // 已空
}

// ── 绕回（wrap-around）正确性 ───────────────────────────────────────
// 反复 push/pop，使内部索引绕过 size_t 的模边界
TEST(SPSCQueueTest, WrapAroundCorrectness) {
    SPSCQueue<uint32_t, 4> q;
    // 做 1000 次 push/pop，验证无数据损坏
    for (uint32_t i = 0; i < 1000u; ++i) {
        ASSERT_TRUE(q.push(i));
        uint32_t val = 0;
        ASSERT_TRUE(q.pop(val));
        EXPECT_EQ(val, i);
    }
    EXPECT_TRUE(q.empty());
}

// ── 以 BBOEvent 为元素（64 字节，1 cacheline） ─────────────────────
TEST(SPSCQueueTest, BBOEventRoundTrip) {
    SPSCQueue<BBOEvent, 8> q;

    BBOEvent ev{};
    ev.instrument_id = 1234;
    ev.bid_px        = 10050;
    ev.ask_px        = 10051;
    ev.bid_qty       = 100;
    ev.ask_qty       = 200;
    ev.type          = EventType::BBO_UPDATE;

    ASSERT_TRUE(q.push(ev));
    BBOEvent out{};
    ASSERT_TRUE(q.pop(out));
    EXPECT_EQ(out.instrument_id, 1234u);
    EXPECT_EQ(out.bid_px, 10050);
    EXPECT_EQ(out.ask_px, 10051);
    EXPECT_EQ(out.bid_qty, 100);
    EXPECT_EQ(out.ask_qty, 200);
}

// ── 以 OrderRequest 为元素（32 字节） ─────────────────────────────
TEST(SPSCQueueTest, OrderRequestRoundTrip) {
    SPSCQueue<OrderRequest, 64> q;

    OrderRequest req{};
    req.instrument_id  = 42;
    req.side           = Side::BUY;
    req.type           = OrderType::LIMIT;
    req.price          = 99900;
    req.qty            = 50;
    req.strategy_ts_ns = 123456789ULL;

    ASSERT_TRUE(q.push(req));
    OrderRequest out{};
    ASSERT_TRUE(q.pop(out));
    EXPECT_EQ(out.instrument_id, 42u);
    EXPECT_EQ(out.price, 99900);
    EXPECT_EQ(out.qty, 50);
    EXPECT_EQ(out.strategy_ts_ns, 123456789ULL);
}

// ── 布局验证：tail_ 和 head_ 各占独立 cacheline ───────────────────
TEST(SPSCQueueTest, SizeReflectsCachelineSeparation) {
    // tail_  @ offset 0  (8 bytes + 56 bytes padding = 64)
    // head_  @ offset 64 (8 bytes + 56 bytes padding = 64)
    // buffer_@ offset 128
    // sizeof(SPSCQueue<char,2>) >= 128 + 2
    EXPECT_GE(sizeof(SPSCQueue<char, 2>), 130u);
    EXPECT_EQ(alignof(SPSCQueue<char, 2>), 64u);
}

// ── 并发：生产者 / 消费者各 1 线程，100 万次无丢失无重复 ──────────
//
// 验证策略：
//   - 生产者按升序推入 0, 1, 2, ..., TOTAL-1
//   - 消费者收集所有值
//   - 结束后验证：count == TOTAL，顺序严格递增（SPSC 保序）
TEST(SPSCQueueTest, ConcurrentOneMillion) {
    constexpr size_t QUEUE_SIZE = 1024;
    constexpr size_t TOTAL      = 1'000'000;

    SPSCQueue<uint64_t, QUEUE_SIZE> q;
    std::vector<uint64_t> received;
    received.reserve(TOTAL);

    std::thread producer([&]() {
        for (uint64_t i = 0; i < TOTAL; ++i) {
            while (!q.push(i)) {
                // busy-spin：队列满时等待消费者腾出空间
            }
        }
    });

    std::thread consumer([&]() {
        for (size_t i = 0; i < TOTAL; ++i) {
            uint64_t val = 0;
            while (!q.pop(val)) {
                // busy-spin：队列空时等待生产者写入
            }
            received.push_back(val);
        }
    });

    producer.join();
    consumer.join();

    // 验证：无丢失（count == TOTAL）
    ASSERT_EQ(received.size(), TOTAL);

    // 验证：严格递增，即无重复且无乱序
    for (size_t i = 0; i < TOTAL; ++i) {
        EXPECT_EQ(received[i], static_cast<uint64_t>(i))
            << "Data corruption at index " << i
            << ": expected " << i << ", got " << received[i];
        if (received[i] != static_cast<uint64_t>(i)) break;  // 快速失败
    }

    // 验证：pop 后队列为空
    EXPECT_TRUE(q.empty());
}

}  // namespace hft
