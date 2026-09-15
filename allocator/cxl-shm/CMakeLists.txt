cmake_minimum_required(VERSION 3.10)
project(cxl_shm C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 编译选项
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra")

# 头文件目录
include_directories(${PROJECT_SOURCE_DIR}/include)

# 源文件
file(GLOB SOURCES src/*.cc src/*.cpp)

# 生成动态库
add_library(cxlshm SHARED ${SOURCES})

# 生成静态库
add_library(cxlshm_static STATIC ${SOURCES})
set_target_properties(cxlshm_static PROPERTIES OUTPUT_NAME cxlshm)

# 安装规则
install(TARGETS cxlshm cxlshm_static
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
)
install(DIRECTORY include/ DESTINATION include) 