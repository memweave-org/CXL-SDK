cmake_minimum_required(VERSION 3.20)
project(log_structure_alloc C CXX)

set(CMAKE_BUILD_TYPE DEBUG)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_subdirectory(3rdparty)

# 设置编译选项
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra")

option(FAULT_INJECTION "fault injection option" ON)
if(FAULT_INJECTION)
  add_definitions(-DFAULT_INJECTION)
endif()

# 添加头文件目录
include_directories(${PROJECT_SOURCE_DIR}/include)

add_subdirectory(src)

# 链接必要的库
target_link_libraries(lsmalloc pthread clht)

# C 示例程序
# add_executable(c_example examples/c_example.c)
# target_link_libraries(c_example lsmalloc)

# C++ 测试程序（如果需要）
# add_executable(test1 examples/test.cc)
# target_link_libraries(test1 lsmalloc)
# add_executable(test2 examples/test2.cc)
# target_link_libraries(test2 lsmalloc)

# 测试
enable_testing()
add_subdirectory(tests)
