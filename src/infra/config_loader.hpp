// src/infra/config_loader.hpp — 基于 yaml-cpp 的配置加载
//
// 嵌套结构体覆盖 dev.yaml / prod.yaml / backtest.yaml 中的所有字段。
// 所有字段均提供合理默认值，即使 yaml 文件不存在也能正常运行。
// namespace hft
#pragma once

#include <cstdint>
#include <string>

namespace hft {

// ── 风控参数 ─────────────────────────────────────────────────────────────
struct RiskConfig {
    int64_t max_notional{1000000};    ///< 单笔最大名义金额（分）
    int64_t max_net_qty{10000};       ///< 最大净持仓（手），绝对值上限
    int64_t max_order_rate{100};      ///< 每秒最大报单数（Token Bucket 速率）
    int64_t daily_loss_limit{-500000};///< 日亏损熔断阈值（分，负值）
};

// ── 做市策略参数 ──────────────────────────────────────────────────────────
struct MarketMakerConfig {
    uint32_t instrument_id{1};  ///< 策略关注品种
    int64_t  spread_ticks{2};   ///< 目标报价 spread（tick 数）
    int64_t  order_qty{100};    ///< 每边报单数量（手）
    int64_t  max_inventory{1000};///< 最大净持仓绝对值（手）
};

// ── 行情接入参数 ──────────────────────────────────────────────────────────
struct MarketDataConfig {
    std::string mcast_addr{"239.0.0.1"};  ///< 组播地址
    uint16_t    mcast_port{9001};         ///< 组播端口
    std::string iface{};                  ///< 绑定网口名（空=默认路由）
    std::string pcap_path{};             ///< 回测 PCAP 文件路径（非空时启用回放模式）
};

// ── 监控参数 ─────────────────────────────────────────────────────────────
struct MonitoringConfig {
    int         prometheus_port{9090}; ///< Prometheus HTTP exporter 端口
    std::string alert_webhook{};       ///< Webhook 告警 URL（空表示禁用）
};

// ── 顶层系统配置 ──────────────────────────────────────────────────────────
struct SystemConfig {
    std::string      mode{"development"};      ///< 运行模式：development / production
    std::string      log_level{"debug"};       ///< 日志级别：debug / info / warn / error
    std::string      log_dir{"/tmp/hft_logs"}; ///< 日志文件目录
    RiskConfig        risk{};
    MarketMakerConfig market_maker{};
    MarketDataConfig  market_data{};
    MonitoringConfig  monitoring{};
};

// ============================================================
// ConfigLoader — YAML 配置加载器
// ============================================================
class ConfigLoader {
public:
    /// 从指定路径加载 YAML 配置。
    /// 字段缺失时使用 SystemConfig 默认值；文件不存在时直接返回默认配置。
    /// noexcept：内部捕获所有 yaml-cpp 异常，对外无抛出。
    static SystemConfig load(const std::string& yaml_path) noexcept;

    /// 优先读 HFT_CONFIG 环境变量指定的路径；
    /// 变量未设置或文件不存在时 fallback 到 "config/dev.yaml"。
    static SystemConfig load_from_env() noexcept;
};

} // namespace hft
