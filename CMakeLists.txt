cmake_minimum_required(VERSION 3.22)
project(elog)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
                 Debug Release RelWithDebInfo MinSizeRel)
endif()


set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

message(STATUS "Cmake build mode: ${CMAKE_BUILD_TYPE}")

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 14)
    message(FATAL_ERROR
        "NetX needs GCC 14 or newer (found ${CMAKE_CXX_COMPILER_VERSION}); "
        "C++23 <print> is not available before 14. "
        "Configure with -DCMAKE_CXX_COMPILER=g++-14")
endif()


include(CTest)

add_subdirectory(elog)
if(BUILD_TESTING)
    add_subdirectory(test)
endif()

