# GNUInstallDirs 必须在定义 target 之前 include：下面 $<INSTALL_INTERFACE:...>
# 里的 ${CMAKE_INSTALL_INCLUDEDIR} 是在这条命令执行时就展开的，晚一步就会展开成
# 空字符串，导出的 target 会丢掉 include 目录（消费者报 "elog/logger.hpp: No such file"）。
include(GNUInstallDirs)

add_library(elog INTERFACE)
add_library(elog::elog ALIAS elog)

# 头文件按 "elog/logger.hpp" 自包含引入，所以基准目录是本目录的上一级
# （仓库里是 checkout 根，安装后是 <prefix>/include）。
target_include_directories(elog INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

# C++23 挂在 target 上，不用 set(CMAKE_CXX_STANDARD)：
# 作为依赖被 FetchContent 拉进来时，全局设置会改掉宿主项目其它 target 的标准。
# 需要别的标准（例如宿主统一用 26）时用 -DELOG_CXX_STANDARD=26 覆盖。
set(ELOG_CXX_STANDARD 23 CACHE STRING "elog 要求的 C++ 标准")
target_compile_features(elog INTERFACE cxx_std_${ELOG_CXX_STANDARD})

# elog 用了 C++23 的 <print>，GCC 要到 14 才提供。这里报错比让用户看到一句
# 没头没尾的 "print: No such file or directory" 清楚。
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU"
   AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 14)
    message(FATAL_ERROR
        "elog needs GCC 14 or newer (found ${CMAKE_CXX_COMPILER_VERSION}); "
        "C++23 <print> is not available before 14. "
        "Configure with -DCMAKE_CXX_COMPILER=g++-14")
endif()

# ---- 安装与导出（可选）----
#
# 打开后宿主可以 find_package(elog) 直接链接 elog::elog，也可以让
# FetchContent_Declare(... FIND_PACKAGE_ARGS ...) 优先命中已安装的版本。
if(ELOG_INSTALL)
    include(CMakePackageConfigHelpers)

    # 头文件平铺在源码目录下，安装到 <prefix>/include/elog/
    file(GLOB ELOG_HEADERS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp")
    install(FILES ${ELOG_HEADERS}
            DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/elog)

    install(TARGETS elog EXPORT elogTargets)

    install(EXPORT elogTargets
            FILE elogTargets.cmake
            NAMESPACE elog::
            DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/elog)

    configure_package_config_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/elogConfig.cmake.in"
        "${CMAKE_CURRENT_BINARY_DIR}/elogConfig.cmake"
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/elog)

    write_basic_package_version_file(
        "${CMAKE_CURRENT_BINARY_DIR}/elogConfigVersion.cmake"
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion)

    install(FILES
            "${CMAKE_CURRENT_BINARY_DIR}/elogConfig.cmake"
            "${CMAKE_CURRENT_BINARY_DIR}/elogConfigVersion.cmake"
            DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/elog)
endif()
