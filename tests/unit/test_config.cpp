// tests/unit/test_config.cpp — ConfigLoader 单元测试
#include <gtest/gtest.h>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>

#include "infra/config_loader.hpp"

// ============================================================
// 测试：加载 config/dev.yaml 后各字段值正确
// ============================================================
TEST(ConfigLoader, LoadDevYaml) {
    // 从项目根目录的 config/dev.yaml 加载（CTest 运行目录为 build/）
    // 测试程序编译时将项目根目录注入为 SOURCE_DIR
    const std::string path = HFT_SOURCE_DIR "/config/dev.yaml";
    const hft::SystemConfig cfg = hft::ConfigLoader::load(path);

    EXPECT_EQ(cfg.mode,                 "development");
    EXPECT_EQ(cfg.log_level,            "debug");
    EXPECT_EQ(cfg.log_dir,              "/tmp/hft_logs");
    EXPECT_EQ(cfg.risk.max_notional,    1000000);
    EXPECT_EQ(cfg.risk.max_net_qty,     10000);
    EXPECT_EQ(cfg.risk.max_order_rate,  100);
    EXPECT_EQ(cfg.risk.daily_loss_limit, -500000);
    EXPECT_EQ(cfg.market_maker.instrument_id, 1u);
    EXPECT_EQ(cfg.market_maker.spread_ticks,  2);
    EXPECT_EQ(cfg.market_maker.order_qty,     100);
    EXPECT_EQ(cfg.market_maker.max_inventory, 1000);
    EXPECT_EQ(cfg.monitoring.prometheus_port, 9090);
    EXPECT_EQ(cfg.monitoring.alert_webhook,   "");
}

// ============================================================
// 测试：文件不存在时返回默认配置，不崩溃
// ============================================================
TEST(ConfigLoader, MissingFileReturnsDefaults) {
    const hft::SystemConfig cfg = hft::ConfigLoader::load("/nonexistent/path/hft.yaml");

    // 所有字段应等于 SystemConfig{} 的默认值
    EXPECT_EQ(cfg.mode,                  "development");
    EXPECT_EQ(cfg.log_level,             "debug");
    EXPECT_EQ(cfg.risk.max_notional,     1000000);
    EXPECT_EQ(cfg.risk.max_order_rate,   100);
    EXPECT_EQ(cfg.risk.daily_loss_limit, -500000);
}

// ============================================================
// 测试：YAML 中缺少部分字段时使用默认值，不崩溃
// ============================================================
TEST(ConfigLoader, PartialYamlUsesDefaults) {
    // 写一个只有 system.mode 的最小 YAML 到临时文件
    const std::string tmp_path = "/tmp/hft_test_partial.yaml";
    {
        std::ofstream f(tmp_path);
        f << "system:\n  mode: minimal\n";
    }

    const hft::SystemConfig cfg = hft::ConfigLoader::load(tmp_path);
    EXPECT_EQ(cfg.mode, "minimal");
    // 其余字段保持默认
    EXPECT_EQ(cfg.risk.max_notional,   1000000);
    EXPECT_EQ(cfg.risk.max_order_rate, 100);
    EXPECT_EQ(cfg.market_maker.spread_ticks, 2);
    EXPECT_EQ(cfg.monitoring.prometheus_port, 9090);

    std::filesystem::remove(tmp_path);
}

// ============================================================
// 测试：HFT_CONFIG 环境变量覆盖默认路径
// ============================================================
TEST(ConfigLoader, EnvVarOverridesDefaultPath) {
    // 写一个有独特字段值的 YAML 到临时文件
    const std::string tmp_path = "/tmp/hft_test_env.yaml";
    {
        std::ofstream f(tmp_path);
        f << "system:\n  mode: env_test\n  log_level: warn\n";
        f << "risk:\n  max_order_rate: 42\n";
    }

    // 设置环境变量
#if defined(_WIN32)
    _putenv_s("HFT_CONFIG", tmp_path.c_str());
#else
    ::setenv("HFT_CONFIG", tmp_path.c_str(), 1);
#endif

    const hft::SystemConfig cfg = hft::ConfigLoader::load_from_env();
    EXPECT_EQ(cfg.mode,                 "env_test");
    EXPECT_EQ(cfg.log_level,            "warn");
    EXPECT_EQ(cfg.risk.max_order_rate,  42);

    // 清理环境变量，避免影响后续测试
#if defined(_WIN32)
    _putenv_s("HFT_CONFIG", "");
#else
    ::unsetenv("HFT_CONFIG");
#endif

    std::filesystem::remove(tmp_path);
}

// ============================================================
// 测试：load_from_env 在变量未设置时 fallback 到 dev.yaml
// ============================================================
TEST(ConfigLoader, EnvVarFallbackToDevYaml) {
    // 确保 HFT_CONFIG 未设置
#if !defined(_WIN32)
    ::unsetenv("HFT_CONFIG");
#endif

    // load_from_env fallback 到 "config/dev.yaml"
    // CTest 工作目录是 build/，相对路径可能找不到文件；此时应返回默认配置而不崩溃
    const hft::SystemConfig cfg = hft::ConfigLoader::load_from_env();
    // 只验证不崩溃、返回有效默认值
    EXPECT_FALSE(cfg.mode.empty());
    EXPECT_GT(cfg.risk.max_notional, 0);
}
