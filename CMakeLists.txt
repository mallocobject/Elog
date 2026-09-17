cmake_minimum_required(VERSION 3.22)
project(elog VERSION 0.1.0 LANGUAGES CXX)

include(CMakeDependentOption)

# ---- 只在自己是顶层项目时才动全局设置 ----
#
# 被 FetchContent 拉进宿主项目时 PROJECT_IS_TOP_LEVEL 为假：这时再
# set(... CACHE ... FORCE) 会直接覆盖宿主选好的 CMAKE_BUILD_TYPE，属于越界；
# set(CMAKE_CXX_STANDARD) 也会连带改掉宿主其它 target 的标准。
# C++23 的要求挂在 elog 这个 target 上（见 elog/CMakeLists.txt）。
if(PROJECT_IS_TOP_LEVEL)
    if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
        set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
        set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
                     Debug Release RelWithDebInfo MinSizeRel)
    endif()
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
    message(STATUS "Cmake build mode: ${CMAKE_BUILD_TYPE}")
endif()

# ---- 测试与 CTest：只在顶层注册 ----
#
# include(CTest) 会调用 enable_testing()，而 enable_testing() 的作用域是
# “当前目录及其子目录”，一旦在子目录里调用，宿主的根 CTestTestfile.cmake 就会
# 多出一条 subdirs("_deps/elog-build") —— elog 的测试会被塞进宿主的 ctest 列表
# （即便 FetchContent 把它们排除在 ALL 之外，也会显示成一堆 *_NOT_BUILT）。
# 所以作为依赖时：不注册 CTest，只保留一个“要不要建 elog 自己的测试”的开关，
# 默认关闭。
if(PROJECT_IS_TOP_LEVEL)
    include(CTest)
    set(ELOG_BUILD_TESTS_DEFAULT ${BUILD_TESTING})
else()
    option(BUILD_TESTING "Enable testing (host project's switch)" ON)
    set(ELOG_BUILD_TESTS_DEFAULT OFF)
endif()

option(ELOG_BUILD_TESTS "Build elog's own unit tests" ${ELOG_BUILD_TESTS_DEFAULT})

# 压测依赖单元测试的依赖项（Catch2）已经装好，所以跟着 ELOG_BUILD_TESTS 走。
cmake_dependent_option(ELOG_BUILD_STRESS "Build elog's stress benchmark"
                       ${PROJECT_IS_TOP_LEVEL} "ELOG_BUILD_TESTS" OFF)

# 依赖方只要链接 elog::elog 即可，不需要安装这一步。
option(ELOG_INSTALL "Generate install/export rules for find_package(elog)" OFF)

add_subdirectory(elog)

if(ELOG_BUILD_TESTS)
    add_subdirectory(test)
endif()

if(ELOG_BUILD_STRESS)
    add_subdirectory(stress)
endif()
