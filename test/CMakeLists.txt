project(test)

find_package(Catch2 3 REQUIRED)

add_executable(elog_test elog.cpp)

target_link_libraries(elog_test PRIVATE elog Catch2::Catch2WithMain)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(elog_test PRIVATE -Wall -Wextra)
endif()

include(Catch)
catch_discover_tests(elog_test)