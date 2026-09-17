# 压测驱动。构建开关由顶层 CMakeLists 定义（ELOG_BUILD_STRESS），
# 依赖 cmake_dependent_option 的缓存变量取值，这里不再重复定义。
add_executable(elog_bench_stress bench_stress.cpp)
target_link_libraries(elog_bench_stress PRIVATE elog::elog)
# 压测必须是优化构建，否则数字没有任何意义（顶层默认 Release）。
target_compile_options(elog_bench_stress PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-O2 -Wall -Wextra -Wno-comment>)
