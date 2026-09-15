cmake_minimum_required(VERSION 3.10)
project(stamp C)

# 设置C标准
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# 设置编译选项
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O3 -Wall -pthread")

# 设置STM库路径
set(STM_DIR "${CMAKE_SOURCE_DIR}/../../TXMem" CACHE PATH "Path to STM directory")

# 添加STM包含路径
include_directories(
    ${CMAKE_SOURCE_DIR}/backends/${STM_IMPL}
    ${STM_DIR}/${STM_IMPL}/include
)

# 添加STM库路径
link_directories(
    ${STM_DIR}/${STM_IMPL}/lib
)

# 添加STM定义
add_definitions(-DSTM)

# 添加子目录
add_subdirectory(bayes)
add_subdirectory(genome)
add_subdirectory(intruder)
add_subdirectory(kmeans)
add_subdirectory(labyrinth)
add_subdirectory(ssca2)
add_subdirectory(vacation)
add_subdirectory(yada)

# 安装规则
install(TARGETS 
    bayes
    genome
    intruder
    kmeans
    labyrinth
    ssca2
    vacation
    yada
    RUNTIME DESTINATION bin
)
