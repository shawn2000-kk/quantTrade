// src/infra/config_loader.cpp
#include "infra/config_loader.hpp"

#include <yaml-cpp/yaml.h>

#include <cstdlib>
#include <filesystem>

namespace hft {

SystemConfig ConfigLoader::load(const std::string& yaml_path) noexcept {
    SystemConfig cfg{};   // 所有字段已通过成员初始化器设默认值

    // 文件不存在时直接返回默认配置
    if (!std::filesystem::exists(yaml_path)) {
        return cfg;
    }

    try {
        const YAML::Node root = YAML::LoadFile(yaml_path);

        // ── system 节 ──────────────────────────────────────────
        if (const auto& sys = root["system"]) {
            if (sys["mode"])      cfg.mode      = sys["mode"].as<std::string>();
            if (sys["log_level"]) cfg.log_level = sys["log_level"].as<std::string>();
            if (sys["log_dir"])   cfg.log_dir   = sys["log_dir"].as<std::string>();
        }

        // ── risk 节 ────────────────────────────────────────────
        if (const auto& risk = root["risk"]) {
            if (risk["max_notional"])
                cfg.risk.max_notional = risk["max_notional"].as<int64_t>();
            if (risk["max_net_qty"])
                cfg.risk.max_net_qty = risk["max_net_qty"].as<int64_t>();
            if (risk["max_order_rate"])
                cfg.risk.max_order_rate = risk["max_order_rate"].as<int64_t>();
            if (risk["daily_loss_limit"])
                cfg.risk.daily_loss_limit = risk["daily_loss_limit"].as<int64_t>();
        }

        // ── market_maker 节 ────────────────────────────────────
        if (const auto& mm = root["market_maker"]) {
            if (mm["instrument_id"])
                cfg.market_maker.instrument_id = mm["instrument_id"].as<uint32_t>();
            if (mm["spread_ticks"])
                cfg.market_maker.spread_ticks = mm["spread_ticks"].as<int64_t>();
            if (mm["order_qty"])
                cfg.market_maker.order_qty = mm["order_qty"].as<int64_t>();
            if (mm["max_inventory"])
                cfg.market_maker.max_inventory = mm["max_inventory"].as<int64_t>();
        }

        // ── market_data 节 ────────────────────────────────────
        if (const auto& md = root["market_data"]) {
            if (md["mcast_addr"])
                cfg.market_data.mcast_addr = md["mcast_addr"].as<std::string>();
            if (md["mcast_port"])
                cfg.market_data.mcast_port = md["mcast_port"].as<uint16_t>();
            if (md["iface"])
                cfg.market_data.iface = md["iface"].as<std::string>();
            if (md["pcap_path"])
                cfg.market_data.pcap_path = md["pcap_path"].as<std::string>();
        }

        // ── monitoring 节 ──────────────────────────────────────
        if (const auto& mon = root["monitoring"]) {
            if (mon["prometheus_port"])
                cfg.monitoring.prometheus_port = mon["prometheus_port"].as<int>();
            if (mon["alert_webhook"])
                cfg.monitoring.alert_webhook = mon["alert_webhook"].as<std::string>();
        }

    } catch (...) {
        // 解析出错时返回已解析的部分 + 未解析字段的默认值
        // 不向上传播异常（noexcept 保证）
    }

    return cfg;
}

SystemConfig ConfigLoader::load_from_env() noexcept {
    const char* env_path = std::getenv("HFT_CONFIG");
    if (env_path != nullptr && std::filesystem::exists(env_path)) {
        return load(env_path);
    }
    return load("config/dev.yaml");
}

} // namespace hft
