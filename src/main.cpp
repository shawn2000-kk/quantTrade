// src/main.cpp — HFT 实盘主程序（6 线程）
//
// 代码风格：-fno-exceptions 兼容（无 throw / catch，所有接口均为 noexcept 或内部封装异常）
//
// 线程布局：
//   核心 2 — MarketDataThread  （占位符，预留 FeedHandler 接入点）
//   核心 3 — StrategyThread     （策略队列 → 中继给 OMSThread）
//   核心 4 — OMSThread          （风控 + OMS + ExecutionEngine）
//   核心 5 — GatewayThread      （SimGateway Fill 回调 OMS）
//   核心 6 — PositionThread     （FillTracker → PositionManager）
//   核心 7 — MonitorThread      （每秒采集指标，输出到 stderr）

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "infra/config_loader.hpp"
#include "infra/logger.hpp"
#include "infra/cpu_affinity.hpp"
#include "infra/spsc_queue.hpp"
#include "risk/circuit_breaker.hpp"
#include "risk/rate_limiter.hpp"
#include "risk/position_limits.hpp"
#include "risk/pre_trade_risk.hpp"
#include "oms/oms.hpp"
#include "oms/fill_tracker.hpp"
#include "strategy/market_maker.hpp"
#include "strategy/strategy_manager.hpp"
#include "execution/execution_engine.hpp"
#include "connectivity/sim_gateway.hpp"
#include "position/position_manager.hpp"
#include "monitor/latency_tracker.hpp"
#include "monitor/metrics.hpp"

// ── 全局停止标志（信号处理线程安全写，业务线程 relaxed 读）──────────────
static std::atomic<bool> g_stop{false};

static void signal_handler(int /*sig*/) noexcept {
    g_stop.store(true, std::memory_order_relaxed);
}

// ── 命令行参数 ───────────────────────────────────────────────────────────
struct CmdArgs {
    std::string config_path{};
    std::string mode{};
};

static CmdArgs parse_args(int argc, char* argv[]) noexcept {
    CmdArgs args;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
            args.config_path = argv[++i];
        } else if (std::strcmp(argv[i], "--mode") == 0 && i + 1 < argc) {
            args.mode = argv[++i];
        }
    }
    return args;
}

// ── log_level 字符串 → spdlog::level ─────────────────────────────────────
static spdlog::level::level_enum parse_log_level(const std::string& s) noexcept {
    if (s == "debug")    return spdlog::level::debug;
    if (s == "info")     return spdlog::level::info;
    if (s == "warn")     return spdlog::level::warn;
    if (s == "error")    return spdlog::level::err;
    if (s == "critical") return spdlog::level::critical;
    return spdlog::level::info;
}

// ============================================================
int main(int argc, char* argv[]) {
    // ── 1. 解析命令行参数 ──────────────────────────────────────────────
    const auto args = parse_args(argc, argv);

    // ── 2. 加载配置 ────────────────────────────────────────────────────
    hft::SystemConfig cfg;
    if (!args.config_path.empty()) {
        cfg = hft::ConfigLoader::load(args.config_path);
    } else {
        cfg = hft::ConfigLoader::load_from_env();
    }
    // --mode 命令行优先于配置文件中的 mode
    if (!args.mode.empty()) {
        cfg.mode = args.mode;
    }

    // ── 3. 初始化日志 ──────────────────────────────────────────────────
    hft::Logger::instance().init(cfg.log_dir, parse_log_level(cfg.log_level));

    // ── 4. 启动提示 ────────────────────────────────────────────────────
    std::fprintf(stderr,
        "[HFT] Starting quantTrade system (mode=%s)\n",
        cfg.mode.c_str());
    std::fprintf(stderr,
        "[HFT] Config loaded: risk.max_order_rate=%ld, market_maker.spread_ticks=%ld\n",
        static_cast<long>(cfg.risk.max_order_rate),
        static_cast<long>(cfg.market_maker.spread_ticks));

    // ── 5. 构建各层组件 ────────────────────────────────────────────────

    // 风控
    hft::CircuitBreaker cb{cfg.risk.daily_loss_limit};
    hft::RateLimiter    rl{cfg.risk.max_order_rate,
                            cfg.risk.max_order_rate * 2};  // burst = 2× rate
    hft::PositionLimits pl{cfg.risk.max_net_qty, cfg.risk.max_notional};
    hft::PreTradeRisk   risk{cb, rl, pl};

    // OMS
    hft::FillTracker fill_tracker;
    hft::OMS         oms{fill_tracker};

    // 连接 / 执行
    hft::SimGateway               sim_gw{1.0, 1000};  // 100% 成交概率，1µs RTT
    std::vector<hft::IGateway*>   gateways{&sim_gw};
    hft::ExecutionEngine          exec_engine{gateways};

    // 策略：MarketMaker（无需外部 SPSC，内部 order_queue_ 继承自 StrategyBase）
    hft::MarketMaker mm{
        static_cast<hft::InstrumentId>(cfg.market_maker.instrument_id),
        static_cast<hft::Price>(cfg.market_maker.spread_ticks),
        static_cast<hft::Qty>(cfg.market_maker.order_qty),
        static_cast<hft::Qty>(cfg.market_maker.max_inventory)
    };

    hft::StrategyManager strategy_mgr;
    strategy_mgr.register_strategy(mm);

    // StrategyThread → OMSThread 的中继队列
    hft::SPSCQueue<hft::OrderRequest, 1024> strat_to_oms_queue;

    // 持仓
    hft::PositionManager pos_mgr;

    // 监控
    hft::LatencyTracker lat_tracker{"end_to_end", 256};

    // ── 6. 注册信号 ────────────────────────────────────────────────────
    std::signal(SIGINT,  signal_handler);
    std::signal(SIGTERM, signal_handler);

    // ── 7. 各线程事件计数（原子，供退出时打印）─────────────────────────
    std::atomic<uint64_t> md_event_count{0};
    std::atomic<uint64_t> strategy_order_count{0};
    std::atomic<uint64_t> oms_submit_count{0};
    std::atomic<uint64_t> gw_fill_count{0};
    std::atomic<uint64_t> pos_fill_count{0};
    std::atomic<uint64_t> monitor_tick_count{0};

    // ── 8. 启动 6 个线程 ────────────────────────────────────────────────

    // ── MarketDataThread（核心 2）───────────────────────────────────────
    // 占位符：模拟 1ms 周期空循环，预留 feed_bbo(BBOEvent) 接入点。
    // 未来：hft::FeedHandler / hft::PcapReplayer → strategy_mgr.dispatch_bbo()
    std::thread md_thread([&]() noexcept {
        hft::pin_thread_to_core(2);
        HFT_LOG_INFO("market_data", "MarketDataThread started (placeholder)");

        while (!g_stop.load(std::memory_order_relaxed)) {
            // TODO: 接入 FeedHandler，调用 strategy_mgr.dispatch_bbo(bbo)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            md_event_count.fetch_add(1, std::memory_order_relaxed);
        }

        HFT_LOG_INFO("market_data", "MarketDataThread stopped, ticks={}",
                     md_event_count.load());
        std::fprintf(stderr, "[MarketDataThread] stopped, ticks=%llu\n",
                     static_cast<unsigned long long>(md_event_count.load()));
    });

    // ── StrategyThread（核心 3）─────────────────────────────────────────
    // 从策略内部 SPSC 队列 pop 报单请求，推入 strat_to_oms_queue。
    std::thread strategy_thread([&]() noexcept {
        hft::pin_thread_to_core(3);
        HFT_LOG_INFO("strategy", "StrategyThread started");

        hft::OrderRequest req;
        while (!g_stop.load(std::memory_order_relaxed)) {
            if (mm.pop_order(req)) {
                strat_to_oms_queue.push(req);
                strategy_order_count.fetch_add(1, std::memory_order_relaxed);
            } else {
                std::this_thread::sleep_for(std::chrono::microseconds(10));
            }
        }

        HFT_LOG_INFO("strategy", "StrategyThread stopped, orders_relayed={}",
                     strategy_order_count.load());
        std::fprintf(stderr, "[StrategyThread] stopped, orders_relayed=%llu\n",
                     static_cast<unsigned long long>(strategy_order_count.load()));
    });

    // ── OMSThread（核心 4）──────────────────────────────────────────────
    // 从 strat_to_oms_queue pop 报单 → 风控检查 → OMS submit → ExecutionEngine。
    std::thread oms_thread([&]() noexcept {
        hft::pin_thread_to_core(4);
        HFT_LOG_INFO("oms", "OMSThread started");

        hft::OrderRequest req;
        while (!g_stop.load(std::memory_order_relaxed)) {
            if (strat_to_oms_queue.pop(req)) {
                if (risk.check(req)) {
                    (void)oms.submit(req);  // 返回 Order*；生产环境可保存用于 cancel
                    exec_engine.submit(req);
                    oms_submit_count.fetch_add(1, std::memory_order_relaxed);
                }
            } else {
                std::this_thread::sleep_for(std::chrono::microseconds(10));
            }
        }

        HFT_LOG_INFO("oms", "OMSThread stopped, submitted={} active_orders={}",
                     oms_submit_count.load(), oms.active_orders());
        std::fprintf(stderr, "[OMSThread] stopped, submitted=%llu active_orders=%zu\n",
                     static_cast<unsigned long long>(oms_submit_count.load()),
                     oms.active_orders());
    });

    // ── GatewayThread（核心 5）──────────────────────────────────────────
    // SimGateway 模式：弹出模拟成交事件，回调 OMS on_fill。
    // 真实部署时：替换为 FIX/Binary 会话接收线程。
    std::thread gw_thread([&]() noexcept {
        hft::pin_thread_to_core(5);
        HFT_LOG_INFO("gateway", "GatewayThread started (SimGateway mode)");

        hft::FillEvent fill;
        while (!g_stop.load(std::memory_order_relaxed)) {
            if (sim_gw.pop_fill(fill)) {
                oms.on_fill(fill);
                gw_fill_count.fetch_add(1, std::memory_order_relaxed);
            } else {
                std::this_thread::sleep_for(std::chrono::microseconds(10));
            }
        }

        HFT_LOG_INFO("gateway", "GatewayThread stopped, fills_processed={}",
                     gw_fill_count.load());
        std::fprintf(stderr, "[GatewayThread] stopped, fills_processed=%llu\n",
                     static_cast<unsigned long long>(gw_fill_count.load()));
    });

    // ── PositionThread（核心 6）─────────────────────────────────────────
    // 从 FillTracker 消费成交事件 → 更新 PositionManager → 通知风控归还额度。
    std::thread pos_thread([&]() noexcept {
        hft::pin_thread_to_core(6);
        HFT_LOG_INFO("position", "PositionThread started");

        hft::FillEvent fill;
        while (!g_stop.load(std::memory_order_relaxed)) {
            if (fill_tracker.pop_fill(fill)) {
                pos_mgr.on_fill(fill);
                pl.on_fill(fill.instrument_id, fill.side, fill.fill_qty);
                pos_fill_count.fetch_add(1, std::memory_order_relaxed);
            } else {
                std::this_thread::sleep_for(std::chrono::microseconds(10));
            }
        }

        HFT_LOG_INFO("position", "PositionThread stopped, fills_processed={}",
                     pos_fill_count.load());
        std::fprintf(stderr, "[PositionThread] stopped, fills_processed=%llu\n",
                     static_cast<unsigned long long>(pos_fill_count.load()));
    });

    // ── MonitorThread（核心 7）──────────────────────────────────────────
    // 每秒采集一次核心指标，输出到 stderr（非热路径）。
    std::thread monitor_thread([&]() noexcept {
        hft::pin_thread_to_core(7);
        HFT_LOG_INFO("monitor", "MonitorThread started");

        while (!g_stop.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            monitor_tick_count.fetch_add(1, std::memory_order_relaxed);

            const uint64_t tick       = monitor_tick_count.load(std::memory_order_relaxed);
            const size_t   active_ord = oms.active_orders();
            const uint64_t total_fill = fill_tracker.total_fills();
            const uint64_t lat_p99    = lat_tracker.percentile(0.99);

            std::fprintf(stderr,
                "[Monitor] tick=%llu active_orders=%zu total_fills=%llu lat_p99_ns=%llu\n",
                static_cast<unsigned long long>(tick),
                active_ord,
                static_cast<unsigned long long>(total_fill),
                static_cast<unsigned long long>(lat_p99));

            HFT_LOG_INFO("monitor",
                "tick={} active_orders={} total_fills={} lat_p99_ns={}",
                tick, active_ord, total_fill, lat_p99);
        }

        std::fprintf(stderr, "[MonitorThread] stopped, ticks=%llu\n",
                     static_cast<unsigned long long>(monitor_tick_count.load()));
    });

    std::fprintf(stderr, "[HFT] All 6 threads started. Press Ctrl+C to stop.\n");
    HFT_LOG_INFO("hft", "All 6 threads started. mode={}", cfg.mode);

    // ── 9. 主线程等待所有子线程退出 ────────────────────────────────────
    md_thread.join();
    strategy_thread.join();
    oms_thread.join();
    gw_thread.join();
    pos_thread.join();
    monitor_thread.join();

    // ── 10. 优雅退出 ────────────────────────────────────────────────────
    std::fprintf(stderr,
        "[HFT] All threads joined. total_fills=%llu active_orders=%zu\n",
        static_cast<unsigned long long>(fill_tracker.total_fills()),
        oms.active_orders());

    HFT_LOG_INFO("hft", "Shutdown complete. total_fills={} strategy_orders={}",
                 fill_tracker.total_fills(), strategy_order_count.load());

    hft::Logger::instance().flush();
    std::fprintf(stderr, "[HFT] Shutdown complete.\n");

    return 0;
}
