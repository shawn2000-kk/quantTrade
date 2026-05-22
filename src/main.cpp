// src/main.cpp — HFT 实盘主程序（6 线程）
//
// 代码风格：-fno-exceptions 兼容（无 throw / catch，所有接口均为 noexcept 或内部封装异常）
//
// 线程布局：
//   核心 2 — MarketDataThread  （FeedHandler UDP 组播 / PcapReplayer 回测）
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
#include "market_data/feed_handler.hpp"
#include "market_data/pcap_replayer.hpp"
#include "market_data/book_builder.hpp"
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
    // OMSThread → GatewayThread：传递 client_order_id（SimGateway fill 回填用）
    // SimGateway 每次 send_new_order 同步生成 1 条 fill，需回填正确的 cloid
    hft::SPSCQueue<uint64_t, 4096> oms_to_gw_cloid;

    // 持仓
    hft::PositionManager pos_mgr;

    // 行情接入：实盘用 FeedHandler，回测用 PcapReplayer
    const bool use_pcap = !cfg.market_data.pcap_path.empty();
    hft::BookBuilder book_builder;

    std::unique_ptr<hft::FeedHandler>   feed_handler;
    std::unique_ptr<hft::PcapReplayer>  pcap_replayer;

    if (use_pcap) {
        pcap_replayer = std::make_unique<hft::PcapReplayer>(
            cfg.market_data.pcap_path,
            hft::ExchangeType::INTERNAL,
            hft::PcapReplayer::Mode::FASTEST);
        pcap_replayer->set_bbo_callback([&](const hft::BBOEvent& bbo) noexcept {
            book_builder.on_bbo(bbo);
            strategy_mgr.dispatch_bbo(bbo);
        });
        pcap_replayer->set_depth_callback([&](const hft::DepthEvent& depth) noexcept {
            book_builder.on_depth(depth);
        });
    } else {
        feed_handler = std::make_unique<hft::FeedHandler>(
            cfg.market_data.mcast_addr,
            cfg.market_data.mcast_port,
            cfg.market_data.iface);
        feed_handler->set_bbo_callback([&](const hft::BBOEvent& bbo) noexcept {
            book_builder.on_bbo(bbo);
            strategy_mgr.dispatch_bbo(bbo);
        });
    }

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
    // 实盘：FeedHandler 绑定 UDP 组播 → 解析 BBOEvent → 回调 strategy_mgr
    // 回测：PcapReplayer 从 PCAP 文件逐包回放 → 同上回调
    std::thread md_thread([&]() noexcept {
        hft::pin_thread_to_core(2);

        if (use_pcap) {
            HFT_LOG_INFO("market_data", "MarketDataThread started (pcap: {})",
                         cfg.market_data.pcap_path);
            // stop 信号：g_stop 变为 true 时停止回放
            // PcapReplayer::replay() 在文件读完后自然退出
            // 这里注册 g_stop 轮询回调不可行，直接 replay() 等结束
            if (!pcap_replayer->replay()) {
                HFT_LOG_ERROR("market_data", "pcap replay failed");
            }
            g_stop.store(true, std::memory_order_relaxed);  // 回放结束，通知其他线程退出
        } else {
            HFT_LOG_INFO("market_data", "MarketDataThread started (live: {}:{})",
                         cfg.market_data.mcast_addr, cfg.market_data.mcast_port);
            if (!feed_handler->open()) {
                HFT_LOG_ERROR("market_data", "FeedHandler open failed");
                g_stop.store(true, std::memory_order_relaxed);
                return;
            }
            // run() 阻塞直到 stop() 被调用
            // g_stop 由 SIGINT/SIGTERM 或其他线程设置
            auto stopper = std::thread([&]() noexcept {
                while (!g_stop.load(std::memory_order_relaxed)) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }
                feed_handler->stop();
            });
            feed_handler->run();
            if (stopper.joinable()) stopper.join();
        }

        md_event_count.store(
            use_pcap ? pcap_replayer->messages_replayed()
                     : feed_handler->rx_messages(),
            std::memory_order_relaxed);

        HFT_LOG_INFO("market_data", "MarketDataThread stopped, msgs={}",
                     md_event_count.load());
        std::fprintf(stderr, "[MarketDataThread] stopped, msgs=%llu\n",
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
                    hft::Order* order = oms.submit(req);
                    exec_engine.submit(req);  // SimGateway 同步生成 1 fill
                    oms_submit_count.fetch_add(1, std::memory_order_relaxed);

                    if (order) {
                        // 模拟 ACK（PENDING_NEW → NEW）
                        hft::OrderAck ack{};
                        ack.client_order_id   = order->client_order_id;
                        ack.exchange_order_id = order->client_order_id + 9000;
                        ack.status            = hft::OrderStatus::NEW;
                        oms.on_ack(ack);

                        // 把 cloid 推给 GatewayThread，由它回填 fill 后交 OMS
                        oms_to_gw_cloid.push(order->client_order_id);
                    }
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
        uint64_t       cloid = 0;
        while (!g_stop.load(std::memory_order_relaxed)) {
            // OMSThread 先 exec_engine.submit（生成 fill）再 push cloid
            // 所以 pop cloid 能成功时，fill 一定已在 sim_gw 队列中
            if (oms_to_gw_cloid.pop(cloid)) {
                if (sim_gw.pop_fill(fill)) {
                    fill.client_order_id = cloid;
                    oms.on_fill(fill);
                    if (fill.remaining_qty == 0) oms.release(cloid);
                    gw_fill_count.fetch_add(1, std::memory_order_relaxed);
                }
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
