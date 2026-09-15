cmake_minimum_required(VERSION 3.10)
project(tl2 C)

# 设置C标准
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# 设置编译选项
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O3 -Wall -pthread")

# 检测平台
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|i686")
    set(PLATFORM_SOURCE platform_x86.h)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "sparc")
    set(PLATFORM_SOURCE platform_sparc.h)
else()
    message(FATAL_ERROR "Unsupported platform: ${CMAKE_SYSTEM_PROCESSOR}")
endif()

# 源文件
set(TL2_SOURCES
    tl2.c
    tmalloc.c
)

# 创建静态库
add_library(stm STATIC ${TL2_SOURCES})

# 设置包含目录
target_include_directories(stm
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}
)

# 设置输出目录
set_target_properties(stm PROPERTIES
    OUTPUT_NAME "stm"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/lib"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/lib"
)

# 安装规则
install(TARGETS stm
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
)

# 安装头文件
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/
    DESTINATION include
    FILES_MATCHING PATTERN "*.h"
)
