cmake_minimum_required(VERSION 3.16)
project(turbomem LANGUAGES CXX)

include(CheckIncludeFileCXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

check_include_file_cxx(numa.h TURBOMEM_HAVE_NUMA_H)
check_include_file_cxx(numaif.h TURBOMEM_HAVE_NUMAIF_H)
find_library(TURBOMEM_NUMA_LIBRARY numa)

add_executable(test_turbomem test_turbomem.cpp)
target_include_directories(test_turbomem PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(test_turbomem PRIVATE -Wall -Wextra -Wpedantic)

if(TURBOMEM_HAVE_NUMA_H AND TURBOMEM_HAVE_NUMAIF_H AND TURBOMEM_NUMA_LIBRARY)
  target_compile_definitions(test_turbomem PRIVATE TURBOMEM_HAS_LIBNUMA=1)
  target_link_libraries(test_turbomem PRIVATE ${TURBOMEM_NUMA_LIBRARY})
else()
  target_compile_definitions(test_turbomem PRIVATE TURBOMEM_HAS_LIBNUMA=0)
endif()
