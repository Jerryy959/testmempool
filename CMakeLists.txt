cmake_minimum_required(VERSION 3.16)
project(turbomem LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(test_turbomem test_turbomem.cpp)
target_include_directories(test_turbomem PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(test_turbomem PRIVATE -Wall -Wextra -Wpedantic)
