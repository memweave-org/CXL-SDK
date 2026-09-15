cmake_minimum_required(VERSION 3.6)
project(P-CLHT)

if( NOT CMAKE_BUILD_TYPE )
    message(STATUS "No build type selected, default to Release")
    set( CMAKE_BUILD_TYPE Release)
else()
    message(STATUS "Build type is set to ${CMAKE_BUILD_TYPE}")
endif()

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++17 -march=native -mrtm -mcx16 -mavx -mavx2 -mbmi2 -mlzcnt -Wno-deprecated-declarations -Wall -Wextra -fno-builtin-malloc -fno-builtin-calloc -fno-builtin-realloc -fno-builtin-free -faligned-new=64 -DNDEBUG")

execute_process(COMMAND cat /proc/cpuinfo COMMAND grep clflush OUTPUT_VARIABLE ENABLE_CLFLUSH)
execute_process(COMMAND cat /proc/cpuinfo COMMAND grep clflushopt OUTPUT_VARIABLE ENABLE_CLFLUSHOPT)
execute_process(COMMAND cat /proc/cpuinfo COMMAND grep clwb OUTPUT_VARIABLE ENABLE_CLWB)

if(ENABLE_CLWB)
    add_definitions(-DCLWB)
    message(STATUS "Looking for clwb instruction - found")
elseif(ENABLE_CLFLUSHOPT)
    add_definitions(-DCLFLUSH_OPT)
    message(STATUS "Looking for clwb instruction - not found")
    message(STATUS "Looking for clflushopt instruction - found")
elseif(ENABLE_CLFLUSH)
    add_definitions(-DCLFLUSH)
    message(STATUS "Looking for clwb instruction - not found")
    message(STATUS "Looking for clflushopt instruction - not found")
    message(STATUS "Looking for clflush instruction - found")
else()
    message(FATAL_ERROR "Cannot find any flush instructions (clflush, clflushopt, clwb)")
endif()

add_definitions(-DADD_PADDING)

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/external/include)

find_library(JEMALLOC_LIB NAMES jemalloc)
if(NOT JEMALLOC_LIB)
  message(FATAL_ERROR "jemalloc library not found")
endif()
find_library(TBB_LIB NAMES tbb)
if(NOT TBB_LIB)
  message(FATAL_ERROR "tbb library not found")
endif()
find_library(NUMA_LIB NAMES numa)
if(NOT NUMA_LIB)
  message(FATAL_ERROR "libnuma not found")
endif()
find_library(MEMKIND_LIB NAMES memkind)
if(NOT MEMKIND_LIB)
  message(FATAL_ERROR "memkind not found")
endif()
find_package(Threads)

set(SRC_FILE
    src/clht_db.cc
    src/clht_lb_res.cc
    src/clht_gc.cc
    external/sspfd/sspfd.cc
    external/ssmem/src/ssmem.cc
    src/utils/rlock.cc
    src/utils/sim_id.cc
    src/mm/mm.cc
)

add_library(clht STATIC ${SRC_FILE})
# target_link_libraries(clht shm-lib)
target_include_directories(clht PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/external/include)
target_compile_definitions(clht PUBLIC USE_CXL)

target_link_libraries(clht ${TBB_LIB} ${JEMALLOC_LIB} ${NUMA_LIB} ${MEMKIND_LIB} Threads::Threads)

# set(P_CLHT_TEST example.cpp ${SRC_FILE})
# add_executable(clht-example ${P_CLHT_TEST})

# target_link_libraries(clht-example ${TbbLib} ${JemallocLib} boost_system
#     boost_thread pthread)
