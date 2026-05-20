# tests/CMakeLists.txt
# 测试构建配置
# 支持 macOS (Apple Clang) 和 Linux (GCC 12+ / Clang 15+)

cmake_minimum_required(VERSION 3.20)

# ── GoogleTest 引入（顶层已处理，此处仅做 availability 检查）─────────
# GoogleTest targets（GTest::gtest, GTest::gtest_main）由顶层 CMakeLists.txt
# 通过 add_subdirectory(third_party/googletest) 或 FetchContent 提供。
# 此文件无需重复声明，直接使用即可。

# ── 辅助函数：add_hft_test(name) ─────────────────────────────────
# 参数：
#   name — 可执行文件名，对应 unit/${name}.cpp 源文件
#
# 行为：
#   1. 创建名为 ${name} 的测试可执行文件
#   2. 添加 src/ 到 include 搜索路径（与主库一致）
#   3. 链接 hft_infra（基础设施层）
#   4. 检测源文件是否自带 main()：
#      - 无 main()：链接 GTest::gtest_main（GTest 提供入口，标准 TEST() 写法）
#      - 有 main()：链接 GTest::gtest（避免 main 符号重定义链接错误）
#   5. 将测试注册到 CTest
function(add_hft_test name)
    set(_src "${CMAKE_CURRENT_SOURCE_DIR}/unit/${name}.cpp")

    if(NOT EXISTS "${_src}")
        message(WARNING "add_hft_test: source file not found: ${_src}, skipping.")
        return()
    endif()

    # 读取源文件内容，检测是否已定义 main()
    file(READ "${_src}" _src_content)
    string(FIND "${_src_content}" "int main(" _main_pos)

    add_executable(${name} "${_src}")

    target_include_directories(${name}
        PRIVATE "${CMAKE_SOURCE_DIR}/src"
    )

    if(_main_pos GREATER -1)
        # 源文件已有 main()（如编译期纯 static_assert 测试）
        # 只链接 GTest::gtest（不提供 main），避免符号冲突
        target_link_libraries(${name}
            PRIVATE GTest::gtest
                    hft_infra
        )
    else()
        # 标准 GTest 测试文件，使用 GTest 提供的 main 入口
        target_link_libraries(${name}
            PRIVATE GTest::gtest_main
                    hft_infra
        )
    endif()

    # 注册到 CTest；单个测试超时设 30 秒
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES TIMEOUT 30)

    message(STATUS "HFT Test: registered '${name}'")
endfunction()

# ── 自动发现 unit/ 目录下所有 test_*.cpp ─────────────────────────
# CONFIGURE_DEPENDS：新增/删除测试文件时，CMake 自动重新 configure，
# 无需手动运行 cmake -B build
file(GLOB UNIT_TEST_SOURCES
    CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_*.cpp"
)

if(NOT UNIT_TEST_SOURCES)
    message(STATUS "HFT Tests: no test_*.cpp files found in tests/unit/, skipping.")
else()
    foreach(_test_src ${UNIT_TEST_SOURCES})
        # 从完整路径提取文件名（不含扩展名），例如：
        # .../unit/test_types.cpp  →  test_types
        get_filename_component(_test_name "${_test_src}" NAME_WE)
        add_hft_test("${_test_name}")
    endforeach()
endif()

# ── 行情层测试：额外链接 hft_market_data ──────────────────────────
# test_order_book.cpp 依赖 order_book.cpp / book_builder.cpp，
# 这两个文件编译进 hft_market_data（已传递依赖 hft_infra）。
# 在 auto-discovery 创建目标之后追加依赖，不重复调用 add_hft_test。
if(TARGET test_order_book)
    target_link_libraries(test_order_book PRIVATE hft_market_data)
endif()

# ── OMS 层测试：额外链接 hft_oms（含 hft_infra 传递依赖）────────────
# test_oms.cpp 依赖 order_state_machine.cpp / fill_tracker.cpp / oms.cpp，
# 这三个文件编译进 hft_oms（已传递依赖 hft_infra）。
if(TARGET test_oms)
    target_link_libraries(test_oms PRIVATE hft_oms)
endif()

# ── 风控层测试：额外链接 hft_risk（含 hft_oms + hft_infra 传递依赖）──
# test_risk.cpp 使用了 risk/ 和 oms/ 下的类型，需要链接 hft_risk 库。
# 此处在 auto-discovery 创建目标之后追加依赖，不重复调用 add_hft_test。
if(TARGET test_risk)
    target_link_libraries(test_risk PRIVATE hft_risk)
endif()

# ── 连接层测试：额外链接 hft_connectivity（含 hft_oms + hft_infra 传递依赖）──
# test_gateway.cpp 依赖 session_manager.cpp / sim_gateway.cpp，
# 这两个文件编译进 hft_connectivity。
if(TARGET test_gateway)
    target_link_libraries(test_gateway PRIVATE hft_connectivity)
endif()

# ── 持仓层测试：额外链接 hft_position（含 hft_oms + hft_infra 传递依赖）─
# test_position.cpp 依赖 position_manager.cpp / pnl_calculator.cpp，
# 这两个文件编译进 hft_position。
if(TARGET test_position)
    target_link_libraries(test_position PRIVATE hft_position)
endif()

# ── 策略层测试：额外链接 hft_strategy（含 hft_market_data + hft_oms 传递依赖）─
# test_strategy.cpp 依赖 market_maker.cpp / strategy_manager.cpp，
# 这两个文件编译进 hft_strategy。
if(TARGET test_strategy)
    target_link_libraries(test_strategy PRIVATE hft_strategy)
endif()

# ── 执行引擎层测试：额外链接 hft_execution（含 hft_connectivity + hft_infra 传递依赖）─
# test_execution.cpp 依赖 smart_router.cpp / twap.cpp / vwap.cpp / pov.cpp /
# execution_engine.cpp，这五个文件编译进 hft_execution。
if(TARGET test_execution)
    target_link_libraries(test_execution PRIVATE hft_execution)
endif()

# ── 回测层测试：额外 include 路径 + 链接 hft_backtest ──────────────
# test_backtest.cpp 依赖 backtest/ 下的头文件，include 路径需包含项目根目录
# 同时链接 hft_backtest（含 hft_position / hft_oms 传递依赖）
if(TARGET test_backtest)
    target_include_directories(test_backtest
        PRIVATE "${CMAKE_SOURCE_DIR}"       # 使 #include "backtest/..." 可解析
    )
    target_link_libraries(test_backtest
        PRIVATE hft_backtest
                hft_position
                hft_oms
    )
endif()

# ── 监控层测试：额外链接 hft_monitor（含 hft_infra 传递依赖）────────
# test_monitor.cpp 依赖 latency_tracker.cpp / metrics.cpp / alerting.cpp，
# 这三个文件编译进 hft_monitor。
if(TARGET test_monitor)
    target_link_libraries(test_monitor PRIVATE hft_monitor)
endif()

# ── 配置系统测试：额外链接 hft_config，并注入项目根路径宏 ────────────
# test_config.cpp 用 HFT_SOURCE_DIR 宏定位 config/dev.yaml
if(TARGET test_config)
    target_link_libraries(test_config PRIVATE hft_config)
    target_compile_definitions(test_config
        PRIVATE HFT_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
endif()

# ── 日志系统测试：hft_infra 已通过 PUBLIC 传递 spdlog，无需额外链接 ──
# test_logger.cpp 通过 infra/logger.hpp 使用 spdlog 类型；
# spdlog::spdlog 在 hft_infra PUBLIC 依赖中，自动传递到所有链接 hft_infra 的目标。
# 此处仅作说明，不需要额外 target_link_libraries 调用。

# ── 子目录（如有 integration/ 测试，在此扩展）────────────────────
# add_subdirectory(integration)
