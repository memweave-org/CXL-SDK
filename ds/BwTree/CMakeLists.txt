# 设置CMake的最低版本
cmake_minimum_required(VERSION 3.10)

# 设置项目名称
project(bwtree)

# 设置C++标准
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)
# set(CMAKE_CXX_COMPILER /usr/bin/g++)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -pthread -g -Wall -Wno-invalid-offsetof")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -lpapi -latomic -Wl,-rpath,/usr/local/lib")
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O2")

# file(GLOB_RECURSE BENCHMARK_SOURCES "benchmark/*.{cpp,c}")
# file(GLOB TEST_SOURCES "test/*.cpp")
include_directories(${CMAKE_CURRENT_SOURCE_DIR})
# ${TEST_SOURCES} ${BENCHMARK_SOURCES}

set(SRC_FILE src/bwtree.cpp src/bwtree.h src/test_suite.cpp benchmark/art/art.c)

# 添加src目录下的源文件并生成静态库
add_library(bwtree STATIC src/bwtree.cpp src/bwtree.h src/test_suite.cpp)
# add_subdirectory(../shm ${CMAKE_BINARY_DIR}/shm)
target_link_libraries(bwtree shm-lib)
# target_include_directories(bwtree PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(bwtree PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)

# # 查找benchmark和test目录下的所有cpp文件


# # 添加1.main.cpp和所有benchmark及test目录下的源文件并生成可执行文件main
# add_executable(bwtree-test main.cc ${BENCHMARK_SOURCES} ${TEST_SOURCES})

# # 链接静态库到可执行文件
# target_link_libraries(bwtree-test bwtree)
