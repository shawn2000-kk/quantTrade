cmake_minimum_required(VERSION 3.20)
project(quantTrade VERSION 0.1.0 LANGUAGES CXX)

# ── 语言标准 ──────────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)   # 禁用 GNU 扩展，保持严格 C++20

# ── 输出目录 ──────────────────────────────────────────────────────
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# ── 编译选项模块 ──────────────────────────────────────────────────
include(cmake/CompilerFlags.cmake)

# ── 热路径编译选项（interface library，按需链接）────────────────────
add_library(hft_hot_flags INTERFACE)
target_compile_options(hft_hot_flags INTERFACE
    -O3
    -march=native
    -fno-exceptions
    -fno-rtti
    -funroll-loops
    -fomit-frame-pointer
    -DNDEBUG
)

# ── 通用编译告警选项 ──────────────────────────────────────────────
add_library(hft_common_flags INTERFACE)
target_compile_options(hft_common_flags INTERFACE
    -Wall
    -Wextra
    -Wpedantic
)

# ── 可选依赖：DPDK ────────────────────────────────────────────────
include(cmake/FindDPDK.cmake)

# ── 辅助宏：如果 GLOB 结果为空，生成占位 .cpp 避免空 STATIC 库报错 ──
function(ensure_sources out_var dir_glob placeholder_name)
    file(GLOB_RECURSE _srcs CONFIGURE_DEPENDS "${dir_glob}")
    if(NOT _srcs)
        set(_placeholder "${CMAKE_BINARY_DIR}/generated/${placeholder_name}_placeholder.cpp")
        if(NOT EXISTS "${_placeholder}")
            file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/generated")
            file(WRITE "${_placeholder}" "// Auto-generated placeholder — no source files found yet\n")
        endif()
        list(APPEND _srcs "${_placeholder}")
    endif()
    set(${out_var} "${_srcs}" PARENT_SCOPE)
endfunction()

# ── hft_infra 静态库 ──────────────────────────────────────────────
# 收录 src/infra/ 下所有 .cpp（现阶段可能为空，由 ensure_sources 生成占位文件）
ensure_sources(INFRA_SOURCES "${CMAKE_SOURCE_DIR}/src/infra/*.cpp" "infra")
add_library(hft_infra STATIC ${INFRA_SOURCES})
target_include_directories(hft_infra
    PUBLIC  "${CMAKE_SOURCE_DIR}/src"    # 使 #include "infra/..." 等路径均可用
)
target_link_libraries(hft_infra PUBLIC hft_common_flags)

# ── hft_market_data 静态库 ────────────────────────────────────────
ensure_sources(MARKET_DATA_SOURCES "${CMAKE_SOURCE_DIR}/src/market_data/*.cpp" "market_data")
add_library(hft_market_data STATIC ${MARKET_DATA_SOURCES})
target_include_directories(hft_market_data
    PUBLIC  "${CMAKE_SOURCE_DIR}/src"
)
target_link_libraries(hft_market_data
    PUBLIC  hft_infra
            hft_common_flags
)

# ── hft_oms 静态库 ────────────────────────────────────────────────
ensure_sources(OMS_SOURCES "${CMAKE_SOURCE_DIR}/src/oms/*.cpp" "oms")
add_library(hft_oms STATIC ${OMS_SOURCES})
target_include_directories(hft_oms
    PUBLIC  "${CMAKE_SOURCE_DIR}/src"
)
target_link_libraries(hft_oms
    PUBLIC  hft_infra
            hft_common_flags
)

# ── hft_risk 静态库 ───────────────────────────────────────────────
ensure_sources(RISK_SOURCES "${CMAKE_SOURCE_DIR}/src/risk/*.cpp" "risk")
add_library(hft_risk STATIC ${RISK_SOURCES})
target_include_directories(hft_risk
    PUBLIC  "${CMAKE_SOURCE_DIR}/src"
)
target_link_libraries(hft_risk
    PUBLIC  hft_oms
            hft_infra
            hft_common_flags
)

# ── hft_connectivity 静态库 ───────────────────────────────────────
# 收录 src/connectivity/ 下所有 .cpp（SessionManager、SimGateway 等）
ensure_sources(CONNECTIVITY_SOURCES "${CMAKE_SOURCE_DIR}/src/connectivity/*.cpp" "connectivity")
add_library(hft_connectivity STATIC ${CONNECTIVITY_SOURCES})
target_include_directories(hft_connectivity
    PUBLIC  "${CMAKE_SOURCE_DIR}/src"
)
target_link_libraries(hft_connectivity
    PUBLIC  hft_oms          # 依赖 OrderRequest / FillEvent / ModifyRequest
            hft_infra
            hft_common_flags
)

# ── hft_position 静态库 ───────────────────────────────────────────
# 持仓管理 & 盈亏计算层：依赖 hft_oms（FillEvent 类型）和 hft_infra
ensure_sources(POSITION_SOURCES "${CMAKE_SOURCE_DIR}/src/position/*.cpp" "position")
add_library(hft_position STATIC ${POSITION_SOURCES})
target_include_directories(hft_position
    PUBLIC  "${CMAKE_SOURCE_DIR}/src"
)
target_link_libraries(hft_position
    PUBLIC  hft_oms
            hft_infra
            hft_common_flags
)

# ── hft_strategy 静态库 ───────────────────────────────────────────────
# 依赖 hft_market_data（BBOEvent / DepthEvent）和 hft_oms（OrderRequest / FillEvent）
ensure_sources(STRATEGY_SOURCES "${CMAKE_SOURCE_DIR}/src/strategy/*.cpp" "strategy")
add_library(hft_strategy STATIC ${STRATEGY_SOURCES})
target_include_directories(hft_strategy
    PUBLIC  "${CMAKE_SOURCE_DIR}/src"
)
target_link_libraries(hft_strategy
    PUBLIC  hft_market_data
            hft_oms
            hft_common_flags
)

# ── hft_execution 静态库 ──────────────────────────────────────────────
# 执行引擎层：SmartRouter / Twap / Vwap / Pov / ExecutionEngine
# 依赖 hft_connectivity（IGateway 接口 + OrderRequest）和 hft_infra（rdtsc_ns）
ensure_sources(EXECUTION_SOURCES "${CMAKE_SOURCE_DIR}/src/execution/*.cpp" "execution")
add_library(hft_execution STATIC ${EXECUTION_SOURCES})
target_include_directories(hft_execution
    PUBLIC  "${CMAKE_SOURCE_DIR}/src"
)
target_link_libraries(hft_execution
    PUBLIC  hft_connectivity
            hft_infra
            hft_common_flags
)

# ── hft_monitor 静态库 ───────────────────────────────────────────
# 延迟统计 / Prometheus 指标 / Webhook 告警
# 依赖 hft_infra（rdtsc_clock）；Linux 下 alerting.cpp 额外使用 POSIX socket
ensure_sources(MONITOR_SOURCES "${CMAKE_SOURCE_DIR}/src/monitor/*.cpp" "monitor")
add_library(hft_monitor STATIC ${MONITOR_SOURCES})
target_include_directories(hft_monitor
    PUBLIC  "${CMAKE_SOURCE_DIR}/src"
)
target_link_libraries(hft_monitor
    PUBLIC  hft_infra
            hft_common_flags
)
# Linux 下 alerting.cpp 需要 pthread（std::thread detach）
if(UNIX AND NOT APPLE)
    target_link_libraries(hft_monitor PUBLIC pthread)
endif()

# ── GoogleTest 查找（先本地 submodule，再 FetchContent 网络拉取）──
if(EXISTS "${CMAKE_SOURCE_DIR}/third_party/googletest/CMakeLists.txt")
    message(STATUS "GoogleTest: using local third_party/googletest")
    add_subdirectory(
        "${CMAKE_SOURCE_DIR}/third_party/googletest"
        "${CMAKE_BINARY_DIR}/googletest"
        EXCLUDE_FROM_ALL
    )
else()
    message(STATUS "GoogleTest: third_party/googletest not found, fetching via FetchContent")
    include(FetchContent)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG        v1.14.0
    )
    # 在 Windows 上防止覆盖父项目的编译器/链接选项
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
endif()

# ── hft_backtest 静态库 ───────────────────────────────────────────
# 回测框架：SimExchange + Backtester + MetricsReport
# 头文件通过 "${CMAKE_SOURCE_DIR}" 以 backtest/xxx.hpp 路径引用
add_library(hft_backtest STATIC
    "${CMAKE_SOURCE_DIR}/backtest/sim_exchange.cpp"
    "${CMAKE_SOURCE_DIR}/backtest/backtester.cpp"
    "${CMAKE_SOURCE_DIR}/backtest/metrics_report.cpp"
)
target_include_directories(hft_backtest
    PUBLIC  "${CMAKE_SOURCE_DIR}"       # 使 #include "backtest/..." 可解析
            "${CMAKE_SOURCE_DIR}/src"   # 使 #include "connectivity/..." 等可解析
)
target_link_libraries(hft_backtest
    PUBLIC  hft_position        # PositionManager / PnlCalculator
            hft_market_data     # BBOEvent / DepthEvent
            hft_oms             # OrderRequest / FillEvent / OrderAck
            hft_common_flags
)

# ── backtest_main 可执行文件 ───────────────────────────────────────
add_executable(backtest_main
    "${CMAKE_SOURCE_DIR}/backtest/main_backtest.cpp"
)
target_link_libraries(backtest_main
    PRIVATE hft_backtest
            hft_position
            hft_oms
)

# ── 测试 ──────────────────────────────────────────────────────────
enable_testing()
add_subdirectory(tests)
