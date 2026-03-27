cmake_minimum_required(VERSION 3.25)

project(Elog)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

message(STATUS "Cmake build mode: ${CMAKE_BUILD_TYPE}")

add_subdirectory(elog)
add_subdirectory(test)