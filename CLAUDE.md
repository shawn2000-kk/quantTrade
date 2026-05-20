# C++ 超高频量化交易系统 (HFT System)

## 项目概述

基于 C++20 的生产级超高频量化交易系统，端到端延迟目标 < 10 微秒（行情接收到报单发出）。系统采用事件驱动架构，全链路无锁设计，支持多交易所接入、实盘交易与历史回测。

---

## 系统架构总览

```
┌─────────────────────────────────────────────────────────┐
│                    监控 & 运维层                          │
│          (Prometheus + Grafana + 告警 + 日志)             │
├─────────────────────────────────────────────────────────┤
│                    策略引擎层                             │
│     (做市 / 统计套利 / 动量 / 事件驱动策略框架)             │
├──────────────────┬──────────────────────────────────────┤
│   风控层          │         订单管理层 (OMS)               │
│ (Pre-trade 风控)  │   (订单状态机 / 成交跟踪 / 报撤单)       │
├──────────────────┴──────────────────────────────────────┤
│                    执行引擎层                             │
│       (智能路由 / 执行算法 TWAP/VWAP/POV)                 │
├─────────────────────────────────────────────────────────┤
│                  交易所连接层                             │
│   (FIX 4.2/4.4 / 二进制私有协议 / 会话管理)                │
├─────────────────────────────────────────────────────────┤
│                  行情处理层                               │
│   (UDP 组播 Feed / 订单簿重建 / 逐档行情)                   │
├─────────────────────────────────────────────────────────┤
│                    基础设施层                             │
│  (CPU 绑核 / NUMA / 大页内存 / 无锁队列 / 内核旁路网络)      │
└─────────────────────────────────────────────────────────┘
```

数据流向（热路径）：
```
网卡 → FeedHandler → BookBuilder → OrderBook
                                      ↓
                               StrategyEngine
                                      ↓
                              RiskManager (pre-trade)
                                      ↓
                                    OMS
                                      ↓
                               ExecutionEngine
                                      ↓
                                   Gateway → 交易所
```

---

## 目录结构

```
quantTrade/
├── CMakeLists.txt                  # 顶层 CMake（C++20，-O3 -march=native）
├── CLAUDE.md                       # 本设计文档
├── cmake/
│   ├── FindDPDK.cmake              # DPDK 查找模块
│   └── CompilerFlags.cmake         # 编译优化选项
├── config/
│   ├── dev.yaml                    # 开发环境配置
│   ├── prod.yaml                   # 生产环境配置
│   └── backtest.yaml               # 回测配置
├── src/
│   ├── common/                     # 跨层共享类型（所有模块的最底层依赖）
│   │   └── types.hpp               # Price/Qty/Timestamp typedef + 所有枚举定义
│   ├── infra/                      # 基础设施层
│   │   ├── cpu_affinity.hpp/cpp    # CPU 绑核，隔离核心
│   │   ├── huge_pages.hpp/cpp      # 大页内存（2MB / 1GB）
│   │   ├── numa.hpp/cpp            # NUMA 感知内存分配
│   │   ├── spsc_queue.hpp          # 无锁 SPSC 环形队列
│   │   ├── mpsc_queue.hpp          # 无锁 MPSC 队列
│   │   ├── memory_pool.hpp         # 固定大小对象池
│   │   ├── rdtsc_clock.hpp         # TSC 高精度时钟（纳秒）
│   │   └── logger.hpp/cpp          # 异步无锁日志（spdlog）
│   ├── market_data/                # 行情处理层
│   │   ├── market_data_types.hpp   # BBOEvent(热路径) / DepthEvent(冷路径) / PriceLevel
│   │   ├── feed_handler.hpp/cpp    # UDP 组播接收，recvmmsg 批量收包
│   │   ├── pcap_replayer.hpp/cpp   # PCAP 文件回放（回测 / 调试）
│   │   ├── order_book.hpp/cpp      # 无锁订单簿，数组存价位
│   │   ├── book_builder.hpp/cpp    # 快照+增量合并，序列号校验
│   │   └── normalizer.hpp/cpp      # 多交易所行情归一化
│   ├── strategy/                   # 策略引擎层
│   │   ├── strategy_base.hpp       # 策略抽象基类
│   │   ├── strategy_manager.hpp/cpp# 策略生命周期管理
│   │   ├── market_maker.hpp/cpp    # 做市策略
│   │   ├── stat_arb.hpp/cpp        # 统计套利
│   │   └── momentum.hpp/cpp        # 动量策略
│   ├── risk/                       # 风控层（硬性门卫）
│   │   ├── pre_trade_risk.hpp/cpp  # Pre-trade 检查汇总
│   │   ├── position_limits.hpp/cpp # 持仓 / 名义金额限额
│   │   ├── rate_limiter.hpp/cpp    # Token Bucket 报单速率限制
│   │   └── circuit_breaker.hpp/cpp # 熔断器状态机
│   ├── oms/                        # 订单管理层
│   │   ├── order.hpp               # Order 数据结构
│   │   ├── order_state_machine.hpp/cpp # 订单状态机
│   │   ├── oms.hpp/cpp             # OMS 主逻辑
│   │   └── fill_tracker.hpp/cpp    # 成交跟踪 & 通知
│   ├── execution/                  # 执行引擎层
│   │   ├── execution_engine.hpp/cpp# 执行引擎主控
│   │   ├── smart_router.hpp/cpp    # 流动性评分 + 智能路由
│   │   ├── twap.hpp/cpp            # TWAP（均匀时间切分）
│   │   ├── vwap.hpp/cpp            # VWAP（历史量曲线加权）
│   │   └── pov.hpp/cpp             # POV（参与率算法）
│   ├── connectivity/               # 交易所连接层
│   │   ├── gateway.hpp             # IGateway 统一接口
│   │   ├── fix_session.hpp/cpp     # FIX 4.2/4.4（QuickFIX/N）
│   │   ├── binary_protocol.hpp/cpp # 交易所私有二进制协议
│   │   └── session_manager.hpp/cpp # 心跳 & 自动重连
│   ├── position/                   # 持仓 & P&L
│   │   ├── position_manager.hpp/cpp# 原子操作实时持仓
│   │   └── pnl_calculator.hpp/cpp  # Realized / Unrealized P&L
│   └── monitor/                    # 监控层
│       ├── latency_tracker.hpp/cpp # TSC 打点，分位数统计
│       ├── metrics.hpp/cpp         # Prometheus Counter / Gauge / Histogram
│       └── alerting.hpp/cpp        # Webhook 告警
├── backtest/                       # 回测框架
│   ├── backtester.hpp/cpp          # 回测引擎主控
│   ├── sim_exchange.hpp/cpp        # 模拟交易所（FIFO 撮合 + 滑点）
│   ├── metrics_report.hpp/cpp      # Sharpe / 最大回撤 / 胜率报告
│   └── main_backtest.cpp           # 回测入口
├── tests/
│   ├── unit/                       # 单元测试（GoogleTest）
│   └── integration/                # 集成测试
├── tools/
│   ├── pcap_capture.sh             # tcpdump 行情抓包脚本
│   └── latency_plot.py             # 延迟分布可视化
└── third_party/                    # 第三方库（git submodule）
    ├── spdlog/
    ├── yaml-cpp/
    ├── prometheus-cpp/
    └── googletest/
```

---

## 各层关键设计

### 层一：基础设施层

**目标：所有基础工具开销 < 1ns**

| 组件 | 技术 | 关键设计 |
|------|------|---------|
| 时钟 | `RDTSC` | 避免 `clock_gettime` 系统调用；启动时与 NTP 校准一次 |
| SPSC 队列 | `std::atomic` 环形缓冲 | 64B cacheline 对齐；head/tail 分离防 false sharing |
| 内存 | `mmap` + `MAP_HUGETLB` | 2MB 大页；减少 TLB miss；启动时预分配 |
| CPU 绑核 | `pthread_setaffinity_np` | 隔离核心（`isolcpus`）；关闭超线程；IRQ 迁移 |
| 日志 | `spdlog` async sink | 日志独占线程，不阻塞交易线程；环形 buffer 溢出丢日志不阻塞 |
| 网络 | Solarflare OpenOnload / DPDK | 内核旁路；零拷贝；busy-poll 替代中断 |

```cpp
// src/infra/rdtsc_clock.hpp
inline uint64_t rdtsc_ns() {
    uint32_t lo, hi;
    __asm__ volatile("rdtscp" : "=a"(lo), "=d"(hi) :: "ecx");
    return (static_cast<uint64_t>(hi) << 32 | lo) * ns_per_cycle_;
}

// src/infra/spsc_queue.hpp — 64B cacheline 分离 head/tail
template<typename T, size_t N>
class alignas(64) SPSCQueue {
    alignas(64) std::atomic<size_t> head_{0};
    alignas(64) std::atomic<size_t> tail_{0};
    T buffer_[N];
public:
    bool push(T&& val) noexcept;
    bool pop(T& val)   noexcept;
};
```

---

### 层二：行情处理层

**目标：行情处理延迟 < 1µs (p99)**

- **FeedHandler**：绑定组播 socket；`SO_RCVBUF` 设为 64MB；`recvmmsg` 批量收包（每批最多 64 包）；收到后立即打 TSC 时间戳
- **OrderBook**：bid/ask 各用 `std::array<PriceLevel, 20>` 预分配；价格用整数表示（避免浮点）；支持 5 档 / 20 档深度
- **BookBuilder**：快照+增量两阶段同步；序列号连续性校验；乱序缓存最多 100 包，超时触发重新请求快照
- **Normalizer**：将各交易所原始格式转为统一的 `BBOEvent` / `DepthEvent`

行情事件拆分为两个结构体，热冷路径分离：

```cpp
// src/market_data/market_data_types.hpp

// 热路径：精确 1 cacheline（64B），走 SPSC 队列触发策略
// TRADE 事件复用字段：bid_px = 成交价，bid_qty = 成交量
struct alignas(64) BBOEvent {
    Timestamp    exchange_ts_ns;   // 交易所时间戳（纳秒）
    Timestamp    local_ts_ns;      // 本地 RDTSC 时间戳（纳秒）
    InstrumentId instrument_id;
    EventType    type;             // TRADE / BBO_UPDATE / DEPTH_UPDATE
    uint8_t      _pad[3];
    Price        bid_px;           // BBO: 最优买价；TRADE: 成交价
    Price        ask_px;           // BBO: 最优卖价；TRADE: 0
    Qty          bid_qty;          // BBO: 最优买量；TRADE: 成交量
    Qty          ask_qty;          // BBO: 最优卖量；TRADE: 0
};  // sizeof == 64 ✓

// 冷路径：完整 5 档深度，不走 SPSC，由 OrderBook 提供引用访问
struct DepthEvent {
    Timestamp    ts_ns;
    InstrumentId instrument_id;
    uint8_t      bid_levels, ask_levels;
    PriceLevel   bids[MARKET_DEPTH];   // 5 档买价
    PriceLevel   asks[MARKET_DEPTH];   // 5 档卖价
};
```

---

### 层三：策略引擎层

**目标：策略决策延迟 < 2µs (p99)**

- 事件驱动：策略实现三个回调，无 `virtual` 调用开销（模板静态分派）
- 策略与 OMS 通过 SPSC Queue 解耦，策略线程**不直接操作 socket**
- 参数热更新：`std::atomic` 原子变量 + `memory_order_relaxed` 读取

```cpp
// src/strategy/strategy_base.hpp
template<typename Derived>
class StrategyBase {
public:
    void on_market_event(const MarketEvent& e) {
        static_cast<Derived*>(this)->on_market_event_impl(e);
    }
    void on_fill(const FillEvent& f)    { static_cast<Derived*>(this)->on_fill_impl(f); }
    void on_order_ack(const OrderAck& a){ static_cast<Derived*>(this)->on_order_ack_impl(a); }

protected:
    void send_order(OrderRequest&& req); // 推入 order_queue_ (SPSC)

private:
    SPSCQueue<OrderRequest, 1024> order_queue_;
};
```

**内置策略**：
- `MarketMaker`：基于 mid-price ± spread 双边报价，动态调整 inventory skew
- `StatArb`：基于协整对价差 z-score 触发开平仓
- `Momentum`：短周期 EMA 交叉信号，配合 VWAP 执行

---

### 层四：风控层

**原则：任何报单必须通过所有风控检查，失败立即拒绝**

| 检查项 | 实现 | 阈值示例 |
|--------|------|---------|
| 单笔名义金额 | 原子比较 | ≤ $100,000 |
| 净持仓限额 | CAS 原子扣减 | ≤ 1000 手 |
| 报单速率 | Token Bucket（纳秒精度） | ≤ 1000 单/秒 |
| 价格偏离 | 与 mid-price 比较 | ≤ 10 个 tick |
| P&L 熔断 | 状态机 CLOSED→OPEN | 日亏 ≤ -$50,000 |

```cpp
// src/risk/circuit_breaker.hpp
enum class CBState { CLOSED, OPEN, HALF_OPEN };

class CircuitBreaker {
    std::atomic<CBState> state_{CBState::CLOSED};
    std::atomic<int64_t> daily_pnl_{0};
    int64_t              open_threshold_;    // 触发阈值（负值）
public:
    bool allow_order() const noexcept {
        return state_.load(std::memory_order_relaxed) == CBState::CLOSED;
    }
    void update_pnl(int64_t delta) noexcept;
};
```

---

### 层五：订单管理层（OMS）

**订单状态机**：

```
                    ┌─────────────────────────────┐
PENDING_NEW ──ACK──► NEW ──部分成交──► PARTIALLY_FILLED ──全成──► FILLED
                     │                      │
                  撤单请求                撤单请求
                     ↓                      ↓
               PENDING_CANCEL ──确认──► CANCELLED
                     │
                  拒绝
                     ↓
                 REJECTED
```

- `Order` 对象由 `MemoryPool<Order, 65536>` 分配，零 `new/delete`
- `client_order_id` 为单调递增 `uint64_t`，`std::unordered_map` O(1) 查找
- 成交后立即触发：持仓更新 + 风控归还额度 + 策略回调（均为异步 SPSC 通知）

```cpp
// src/oms/order.hpp
struct alignas(64) Order {           // sizeof == 64，精确 1 cacheline
    uint64_t    client_order_id;
    uint64_t    exchange_order_id;
    uint32_t    instrument_id;
    Side        side;
    OrderType   type;
    OrderStatus status;
    uint8_t     _pad[1];
    Price       price;               // 整数价格（tick 单位）
    Qty         qty;                 // 原始报单数量
    Qty         filled_qty;          // 累计成交数量
    Timestamp   submit_ts_ns;        // 报单发出时间戳
    Timestamp   ack_ts_ns;           // 收到 ACK 时间戳

    Qty remaining_qty() const noexcept { return qty - filled_qty; }
};
```

---

### 层六：执行引擎层

- **SmartRouter**：对每个交易所计算流动性评分（深度 × 速度），选最优路径；自动 failover
- **TWAP**：将大单切分到 N 个等时间窗口；每个子单加 ±10% 随机抖动，防止被算法猎手识别
- **VWAP**：根据历史同期成交量分布计算每个时间片的目标参与量；实时偏差修正
- **POV**：按市场实时成交量的固定百分比（如 5%）动态调整子单量

---

### 层七：交易所连接层

```cpp
// src/connectivity/gateway.hpp — 统一接口
class IGateway {
public:
    virtual ~IGateway() = default;
    virtual void     send_new_order(const OrderRequest&)       = 0;
    virtual void     send_cancel(uint64_t client_order_id)     = 0;
    virtual void     send_modify(const ModifyRequest&)         = 0;
    virtual bool     is_connected() const noexcept             = 0;
    virtual uint64_t get_latency_ns() const noexcept           = 0; // 最近一次 RTT
};
```

- **FIX Session**（`fix_session.cpp`）：QuickFIX/N 封装；支持 FIX 4.2 / 4.4；断线自动重连；序列号持久化到磁盘
- **Binary Protocol**（`binary_protocol.cpp`）：交易所私有协议（如上交所 STEP、深交所 BINARY）；`iovec` 零拷贝发送；解码使用 `reinterpret_cast`（对齐保证）
- **Session Manager**：定时心跳检测；连接断开后指数退避重连（最大 30 秒）

---

### 层八：持仓 & P&L

```cpp
// src/position/position_manager.hpp
class PositionManager {
    // 每个品种独占一个 cacheline，避免 false sharing
    struct alignas(64) InstrumentPosition {
        std::atomic<int64_t> net_qty{0};       // 净持仓（正多负空）
        std::atomic<int64_t> avg_cost{0};      // 加权平均成本（整数）
        std::atomic<int64_t> realized_pnl{0};  // 已实现盈亏
    };
    InstrumentPosition positions_[MAX_INSTRUMENTS];
public:
    void on_fill(const FillEvent&) noexcept;
    int64_t get_unrealized_pnl(uint32_t instrument_id, int64_t mark_price) const noexcept;
};
```

- 实时持仓：`std::atomic<int64_t>` CAS 操作，无锁
- MTM（逐日盯市）：每 100ms 用最新 mid-price 重算 unrealized P&L
- 支持多交易所持仓汇总视图
- 风险限额联动：持仓变动后自动通知风控层更新可用额度

---

## 线程模型

```
核心 2  ──  MarketDataThread   ─ recvmmsg → BookBuilder → OrderBook
核心 3  ──  StrategyThread     ─ on_market_event → send_order (→ SPSC)
核心 4  ──  OMSThread          ─ risk_check → order_state_machine → send (→ SPSC)
核心 5  ──  GatewayThread      ─ FIX/Binary encode → 网卡
核心 6  ──  PositionThread     ─ on_fill → position_update → pnl
核心 7  ──  MonitorThread      ─ metrics 采集 + 日志落盘
其他核心 ── BacktestThread     ─ 仅回测模式，不隔离
```

- **线程间通信**：全部使用 SPSC Lock-Free Queue，**零锁争用**
- **系统配置**：`/etc/systemd/system/hft.service` 设置 `CPUAffinity`；内核启动参数添加 `isolcpus=2-6 nohz_full=2-6 rcu_nocbs=2-6`

---

## 性能目标

| 指标 | 目标值 | 测量点 |
|------|--------|-------|
| 行情处理延迟 | < 1 µs (p99) | 收包 → OrderBook 更新完成 |
| 策略决策延迟 | < 2 µs (p99) | OrderBook 更新 → OrderRequest 入队 |
| OMS + 风控延迟 | < 1 µs (p99) | 出队 → 报单编码完成 |
| 报单网络延迟 | < 5 µs (p99) | 取决于主机托管位置（co-location） |
| **端到端延迟** | **< 10 µs (p99)** | 行情接收 → 报单发出 |
| 订单簿更新吞吐 | > 5M events/sec | 单线程 |
| 日志吞吐 | > 1M lines/sec | 异步，不影响热路径 |

---

## 回测框架

### SimExchange（模拟交易所）
- **撮合规则**：价格优先 + 时间优先（FIFO）
- **滑点模型**：可配置固定滑点 / 按深度成比例滑点
- **部分成交**：随机填充比例（可配置分布）
- **延迟模拟**：可注入服从正态分布的网络延迟

### 回测流程
```
PCAP 文件 → PcapReplayer → (同正常行情通路) → 策略 → SimExchange → 成交回报
```

### 输出指标
| 指标 | 说明 |
|------|------|
| Sharpe Ratio | 年化，基于日收益 |
| Max Drawdown | 最大资金回撤幅度及持续时间 |
| Win Rate | 盈利交易次数比例 |
| Avg Hold Time | 平均持仓时间 |
| Turnover Rate | 日换手率 |
| Fill Rate | 报单实际成交率 |

支持 **Walk-Forward 滚动回测**验证策略是否过拟合。

---

## 监控 & 运维

### 指标（Prometheus）
```
hft_latency_ns{stage="market_data|strategy|oms|gateway", quantile="0.5|0.95|0.99"}
hft_order_count{status="new|filled|cancelled|rejected"}
hft_position{instrument="BTC-USD|..."}
hft_pnl{type="realized|unrealized"}
hft_circuit_breaker_state{exchange="..."}
```

### Grafana 看板
- 实时延迟分位数折线图
- 持仓热力图
- P&L 累计曲线
- 订单成交率仪表盘

### 告警触发条件
- 熔断器打开（任意交易所）
- 连接断开超过 5 秒
- p99 延迟超过阈值的 2 倍
- 日亏超过预警线（50% 熔断阈值）

---

## 构建系统

### CMake 配置要点
```cmake
# CMakeLists.txt 关键设置
cmake_minimum_required(VERSION 3.20)
project(quantTrade CXX)
set(CMAKE_CXX_STANDARD 20)

# 热路径编译选项（禁止 RTTI 和异常，减少 ABI 开销）
set(HOT_PATH_FLAGS "-O3 -march=native -fno-exceptions -fno-rtti
                    -funroll-loops -fomit-frame-pointer
                    -DNDEBUG")
```

### 主要依赖

| 库 | 版本 | 用途 |
|----|------|------|
| spdlog | ≥ 1.12 | 异步日志 |
| yaml-cpp | ≥ 0.8 | 配置解析 |
| prometheus-cpp | ≥ 1.2 | 指标暴露 |
| QuickFIX/N | latest | FIX 协议 |
| GoogleTest | ≥ 1.14 | 单元测试 |
| Boost.Asio | ≥ 1.83 | 异步 IO（非热路径） |
| oneTBB | ≥ 2021.10 | 并发工具（非热路径） |

### 依赖管理
- 第三方库优先使用 **git submodule**（`third_party/`）
- 系统级依赖（DPDK、OpenOnload）通过 `FindXxx.cmake` 查找
- 可选：vcpkg 或 Conan 管理包版本

---

## 实现顺序

按依赖关系由底向上逐步实现，每层完成后有独立的单元测试：

1. **基础设施层** — SPSC 队列、内存池、TSC 时钟、CPU 绑核、日志；*可独立测试*
2. **行情处理层** — FeedHandler（先 PCAP 回放）、OrderBook、BookBuilder；*用录制数据测试*
3. **OMS + 风控层** — 订单状态机、Pre-trade 检查；*用 mock 事件测试*
4. **交易所连接层** — FIX 会话（先接 SIT/UAT 环境）、Gateway 接口；*联调测试*
5. **策略引擎层** — 策略基类 + 简单做市策略 Demo；*回测验证逻辑*
6. **执行算法层** — SmartRouter + TWAP；*回测验证执行质量*
7. **持仓 & P&L** — PositionManager + PnL；*对账验证*
8. **回测框架** — SimExchange + 历史 PCAP 回放；*端到端验证*
9. **监控 & 运维** — Prometheus 指标 + Grafana 看板 + 告警；*生产就绪*

---

## 各层验收标准

| 层次 | 测试方式 | 通过标准 |
|------|---------|---------|
| 基础设施层 | 单元测试 + bench | SPSC 吞吐 > 100M ops/sec；无数据竞争 |
| 行情处理层 | PCAP 回放对账 | 重建订单簿与交易所快照完全一致 |
| OMS + 风控 | mock 事件单元测试 | 所有状态转换路径覆盖；风控拦截准确 |
| 交易所连接 | UAT/SIT 环境联调 | 报单/ACK/成交回报正常；断线重连正常 |
| 策略层 | 回测框架 | Sharpe ≥ 目标值；最大回撤可接受 |
| 端到端 | 全链路延迟 bench | p99 < 10µs |

---

## 生产部署检查清单

### 硬件要求
- [ ] 专用服务器，co-location 或 proximity hosting
- [ ] Solarflare / Mellanox 低延迟网卡
- [ ] 高主频 CPU（≥ 3.5GHz），禁用 C-states 和 Turbo Boost 抖动
- [ ] 足够 RAM（≥ 64GB），预留大页内存

### 系统配置
- [ ] `isolcpus=2-6 nohz_full=2-6 rcu_nocbs=2-6` 内核参数
- [ ] 关闭超线程（Hyperthreading）
- [ ] 调整网卡中断亲和性，避免打断交易核心
- [ ] `SO_RCVBUF` 设为 ≥ 64MB
- [ ] 关闭 swap（`swapoff -a`）

### 上线前验证
- [ ] 在 UAT/SIT 环境全功能联调通过
- [ ] 回测 Sharpe ≥ 目标值，最大回撤在可接受范围
- [ ] 压测：> 5M events/sec，p99 延迟达标
- [ ] 风控参数与业务要求对齐，熔断测试通过
- [ ] 监控告警联调，确认能正常触发

---

*文档版本：v1.2 | 创建时间：2026-05-20 | 最后更新：2026-05-20*

### 变更记录
- **v1.2**：新增各层验收标准表格
- **v1.1**：新增 `src/common/types.hpp`；`MarketEvent` 拆分为 `BBOEvent`（热路径，64B）和 `DepthEvent`（冷路径）；`Order.remaining_qty` 改为计算方法
