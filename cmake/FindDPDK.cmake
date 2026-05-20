# cmake/FindDPDK.cmake
# 查找 DPDK（Data Plane Development Kit）
#
# 输出变量：
#   DPDK_FOUND         — TRUE 表示找到 DPDK
#   DPDK_INCLUDE_DIRS  — 头文件搜索路径
#   DPDK_LIBRARIES     — 链接库列表
#   DPDK_VERSION       — 检测到的版本号
#
# DPDK 是可选依赖；找不到时输出 WARNING，不中止构建。
# 使用方：
#   include(cmake/FindDPDK.cmake)
#   if(DPDK_FOUND)
#       target_include_directories(my_target PRIVATE ${DPDK_INCLUDE_DIRS})
#       target_link_libraries(my_target PRIVATE ${DPDK_LIBRARIES})
#   endif()

# 优先使用 pkg-config（DPDK >= 20.11 官方支持 libdpdk.pc）
find_package(PkgConfig QUIET)

if(PKG_CONFIG_FOUND)
    pkg_check_modules(DPDK QUIET libdpdk)

    if(DPDK_FOUND)
        message(STATUS "DPDK: found via pkg-config, version ${DPDK_VERSION}")
        message(STATUS "  DPDK_INCLUDE_DIRS = ${DPDK_INCLUDE_DIRS}")
        message(STATUS "  DPDK_LIBRARIES    = ${DPDK_LIBRARIES}")

        # 创建 IMPORTED target 方便使用
        if(NOT TARGET DPDK::dpdk)
            add_library(DPDK::dpdk INTERFACE IMPORTED)
            target_include_directories(DPDK::dpdk INTERFACE ${DPDK_INCLUDE_DIRS})
            target_link_libraries(DPDK::dpdk INTERFACE ${DPDK_LIBRARIES})
            # DPDK 通常需要 -mssse3 或更高 SIMD 指令集
            target_compile_options(DPDK::dpdk INTERFACE ${DPDK_CFLAGS_OTHER})
        endif()

        return()
    endif()
endif()

# pkg-config 未找到时，尝试手动查找头文件和库
find_path(DPDK_INCLUDE_DIR
    NAMES rte_eal.h
    PATHS
        /usr/include/dpdk
        /usr/local/include/dpdk
        /opt/dpdk/include
        $ENV{RTE_SDK}/build/include
        $ENV{DPDK_ROOT}/include
    DOC "DPDK include directory"
)

find_library(DPDK_LIBRARY
    NAMES dpdk rte_eal
    PATHS
        /usr/lib
        /usr/lib/x86_64-linux-gnu
        /usr/local/lib
        /opt/dpdk/lib
        $ENV{RTE_SDK}/build/lib
        $ENV{DPDK_ROOT}/lib
    DOC "DPDK main library"
)

if(DPDK_INCLUDE_DIR AND DPDK_LIBRARY)
    set(DPDK_FOUND TRUE)
    set(DPDK_INCLUDE_DIRS "${DPDK_INCLUDE_DIR}")
    set(DPDK_LIBRARIES    "${DPDK_LIBRARY}")

    # 尝试提取版本号
    if(EXISTS "${DPDK_INCLUDE_DIR}/rte_version.h")
        file(STRINGS "${DPDK_INCLUDE_DIR}/rte_version.h" _ver_line
             REGEX "^#define RTE_VER_RELEASE ")
        string(REGEX REPLACE ".*RTE_VER_RELEASE ([0-9]+).*" "\\1" DPDK_VERSION "${_ver_line}")
    endif()

    message(STATUS "DPDK: found via manual search, version ${DPDK_VERSION}")

    if(NOT TARGET DPDK::dpdk)
        add_library(DPDK::dpdk INTERFACE IMPORTED)
        target_include_directories(DPDK::dpdk INTERFACE ${DPDK_INCLUDE_DIRS})
        target_link_libraries(DPDK::dpdk INTERFACE ${DPDK_LIBRARIES})
    endif()
else()
    # DPDK 是可选依赖，找不到只发 WARNING，不中止构建
    set(DPDK_FOUND FALSE)
    message(WARNING
        "DPDK not found. Kernel-bypass networking (DPDK/OpenOnload) will be disabled. "
        "Set DPDK_ROOT or RTE_SDK env var, or install libdpdk-dev to enable."
    )
endif()
