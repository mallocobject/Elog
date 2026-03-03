cmake_minimum_required(VERSION 3.25)

project(Elog)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(LOG_LEVEL "TRACE" CACHE STRING "elog level")
set_property(CACHE LOG_LEVEL PROPERTY STRINGS 
    TRACE
    DEBUG
    INFO
    WARN
    ERROR
    FATAL
)

message(STATUS "Current logger level: ${LOG_LEVEL}")
add_definitions(-DELOG_LEVEL_SETTING=LYNX_${LOG_LEVEL})
message(STATUS "cmake build mode: ${CMAKE_BUILD_TYPE}")

add_subdirectory(elog)

add_executable(Elog main.cpp)

target_link_libraries(Elog PRIVATE elog_lib)

target_compile_definitions(Elog PRIVATE 
    LYNX_WEB_SRC_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)