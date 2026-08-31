cmake_minimum_required(VERSION 3.21)

project(silifork_bridge VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(SILIFORK_BRIDGE_HOST_TESTS
       "Build only format/protocol tests without the Geode SDK" OFF)

if(SILIFORK_BRIDGE_HOST_TESTS)
    include(CTest)
    add_executable(silifork_bridge_tests
        tests/bridge_tests.cpp
        src/binary_api.cpp
        src/replay_reader.cpp
    )
    target_include_directories(silifork_bridge_tests PRIVATE src)
    if(WIN32)
        target_link_libraries(silifork_bridge_tests PRIVATE bcrypt)
    endif()
    if(MSVC)
        target_compile_options(silifork_bridge_tests PRIVATE /W4 /EHsc)
    else()
        target_compile_options(silifork_bridge_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME silifork_bridge_formats COMMAND silifork_bridge_tests)
    return()
endif()

if(NOT WIN32 AND NOT CMAKE_CROSSCOMPILING)
    message(FATAL_ERROR "SiliFork Bridge targets Geometry Dash 2.2081 on Windows x64")
endif()

if(NOT DEFINED ENV{GEODE_SDK})
    message(FATAL_ERROR "GEODE_SDK is not set")
endif()

add_library(${PROJECT_NAME} SHARED
    src/main.cpp
    src/binary_api.cpp
    src/live_bridge.cpp
    src/replay_reader.cpp
)

target_include_directories(${PROJECT_NAME} PRIVATE src)
target_compile_definitions(${PROJECT_NAME} PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE /W4 /EHsc)
else()
    target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra -Wpedantic)
endif()

target_link_libraries(${PROJECT_NAME} ws2_32 bcrypt)

add_subdirectory($ENV{GEODE_SDK} ${CMAKE_CURRENT_BINARY_DIR}/geode)
setup_geode_mod(${PROJECT_NAME}
    EXTERNALS "peony.silicate:1.0.9"
)
