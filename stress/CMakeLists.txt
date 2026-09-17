# 压测可执行文件：默认随构建一起产出，可用 -DELOG_BUILD_STRESS=OFF 关掉。
option(ELOG_BUILD_STRESS "构建 elog 压测驱动" ON)

if(ELOG_BUILD_STRESS)
    add_executable(elog_bench_stress bench_stress.cpp)
    target_link_libraries(elog_bench_stress PRIVATE elog)
    # 压测必须是优化构建，否则数字没有任何意义（根 CMakeLists 默认 Release）。
    target_compile_options(elog_bench_stress PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-O2 -Wall -Wextra -Wno-comment>)
endif()
