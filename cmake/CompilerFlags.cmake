# cmake/CompilerFlags.cmake
# 全局编译器检测与配置
# 此文件在顶层 CMakeLists.txt 中通过 include() 加载

# ── 默认 Build Type（必须在其他所有 if(CMAKE_BUILD_TYPE ...) 之前设置）──
# 防止 cmake -B build 无 -DCMAKE_BUILD_TYPE= 时各条件块全部不命中
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING
        "Build type: Debug | Release | RelWithDebInfo | MinSizeRel" FORCE)
    message(STATUS "CMAKE_BUILD_TYPE not set, defaulting to Release")
endif()

# ── 编译器识别 ────────────────────────────────────────────────────
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
    set(COMPILER_IS_GCC TRUE)
    message(STATUS "Compiler: GCC ${CMAKE_CXX_COMPILER_VERSION}")
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|AppleClang")
    set(COMPILER_IS_CLANG TRUE)
    message(STATUS "Compiler: Clang/AppleClang ${CMAKE_CXX_COMPILER_VERSION}")
else()
    message(WARNING "Unsupported compiler: ${CMAKE_CXX_COMPILER_ID}. Build may fail.")
endif()

# ── C++20 强化（backup，顶层 CMakeLists 已设 CMAKE_CXX_STANDARD）──
# 对于不支持 CMAKE_CXX_STANDARD 的旧 CMake 或 generator，直接加 flag
add_compile_options(-std=c++20)

# ── Linux 特定：链接 pthreads ──────────────────────────────────────
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_compile_options(-pthread)
    add_link_options(-pthread)
    message(STATUS "Platform: Linux — enabling -pthread")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    message(STATUS "Platform: macOS — pthreads built-in, no -pthread flag needed")
endif()

# ── Debug 配置：Address Sanitizer + Undefined Behavior Sanitizer ──
# 以 INTERFACE library 形式暴露，让需要 ASan/UBSan 的 target 按需链接，
# 而不是全局污染所有 target（避免影响 Release 和热路径库）
add_library(hft_sanitizers INTERFACE)

if(COMPILER_IS_GCC OR COMPILER_IS_CLANG)
    target_compile_options(hft_sanitizers INTERFACE
        -fsanitize=address,undefined
        -fno-omit-frame-pointer   # ASan 需要帧指针以获得准确的堆栈跟踪
        -g                         # 调试符号，使 ASan 报告有行号
    )
    target_link_options(hft_sanitizers INTERFACE
        -fsanitize=address,undefined
    )
endif()

# Debug build 全局加调试符号（不含 sanitizer，避免强制所有 target 链接 ASan）
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_compile_options(-g)
    message(STATUS "Build type: Debug — adding -g")
endif()

# Release build 全局优化（顶层 hft_hot_flags interface library 也有此设置，
# 这里作为全局 fallback 确保 Release 模式下所有 target 都能受益）
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    add_compile_options(-O3 -march=native)
    message(STATUS "Build type: Release — adding -O3 -march=native")
endif()
