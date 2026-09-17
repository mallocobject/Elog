# 单元测试。不调用 project()：作为子目录时不该重置 PROJECT_NAME/PROJECT_VERSION，
# 也不该悄悄换掉父作用域的项目名（这里原来写的是 project(test)，会影响整个构建）。
find_package(Catch2 3 REQUIRED)

add_executable(elog_test elog.cpp)
target_link_libraries(elog_test PRIVATE elog::elog Catch2::Catch2WithMain)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(elog_test PRIVATE -Wall -Wextra)
endif()

include(Catch)
catch_discover_tests(elog_test)
