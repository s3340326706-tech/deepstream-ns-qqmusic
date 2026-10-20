cmake_minimum_required(VERSION 3.16)
project(source_engine_cpp CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(CURL REQUIRED)
find_package(Threads REQUIRED)

add_executable(probe
    tools/probe.cpp
    src/util.cpp
    src/http.cpp
    src/source_engine.cpp
)

target_include_directories(probe PRIVATE include)
target_link_libraries(probe PRIVATE CURL::libcurl Threads::Threads)

if (MSVC)
    target_compile_options(probe PRIVATE /W3)
else()
    target_compile_options(probe PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()
