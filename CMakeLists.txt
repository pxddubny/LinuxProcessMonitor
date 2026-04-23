cmake_minimum_required(VERSION 3.16)
project(linux_process_monitor VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(linux_process_monitor
    src/main.cpp
    src/proc_reader.cpp
    src/controller.cpp
)

target_include_directories(linux_process_monitor PRIVATE src)

if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(linux_process_monitor PRIVATE -Wall -Wextra -Wpedantic)
endif()
