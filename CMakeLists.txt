cmake_minimum_required(VERSION 3.16)

project(fm350gl_at_dialer
    VERSION 0.1.0
    DESCRIPTION "Cross-platform FM350GL AT dialer and network configurator"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(fm350gl-at-dialer src/main.cpp)

if (WIN32)
    target_link_libraries(fm350gl-at-dialer PRIVATE iphlpapi ws2_32)
endif()

target_compile_features(fm350gl-at-dialer PRIVATE cxx_std_17)

if (MSVC)
    target_compile_options(fm350gl-at-dialer PRIVATE /W4)
else()
    target_compile_options(fm350gl-at-dialer PRIVATE -Wall -Wextra -pedantic)
endif()
