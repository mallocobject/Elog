file(GLOB LIB_SOURCES CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

add_library(elog_lib STATIC ${LIB_SOURCES})

target_include_directories(elog_lib 
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..
)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set_target_properties(elog_lib PROPERTIES DEBUG_POSTFIX "_d")
endif()