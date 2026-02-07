cmake_minimum_required(VERSION 3.25)

project(logger)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(logger)

add_executable(logger_app main.cpp)

target_link_libraries(logger_app PRIVATE logger_lib)

target_compile_definitions(logger_app PRIVATE 
    LYNX_WEB_SRC_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)