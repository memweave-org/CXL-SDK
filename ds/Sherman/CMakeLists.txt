cmake_minimum_required(VERSION 2.8.5 FATAL_ERROR)
project(Sherman)

# disable boost warning
add_definitions(-DBOOST_COROUTINES_NO_DEPRECATION_WARNING)

#Compiler options
set(CMAKE_C_FLAGS "-Wall -Wno-deprecated-declarations -Wsign-compare -g")
set(CMAKE_CXX_FLAGS "${CMAKE_C_FLAGS} -std=c++11")

# Link Options
set(LINKS_FLAGS "-lcityhash -lboost_system -lboost_coroutine -lpthread")

set(INCLUDE_BASE ${PROJECT_SOURCE_DIR}/include)
include_directories(${INCLUDE_BASE})

#Source file define
set(COMMON_SRC ${PROJECT_SOURCE_DIR}/src)

# #Used by both server and clients
file(GLOB_RECURSE COMMON_FILE ${COMMON_SRC}/*.cpp)
add_library(sherman STATIC ${COMMON_FILE})
target_link_libraries(sherman shm-lib ${LINKS_FLAGS})
target_include_directories(sherman PUBLIC ${INCLUDE_BASE})

# link_libraries(sherman)
# file(GLOB TEST_SRC ${PROJECT_SOURCE_DIR}/test/*.cpp)
# foreach (TEST ${TEST_SRC})
#     get_filename_component(TEST_NAME ${TEST} NAME_WE)
#     add_executable(${TEST_NAME} ${TEST})
#     target_link_libraries(${TEST_NAME} ${LINKS_FLAGS})
# endforeach()

# # 添加src目录下的源文件并生成静态库
# add_library(bwtree STATIC src/bwtree.cpp src/bwtree.h src/test_suite.cpp)
# # add_subdirectory(../shm ${CMAKE_BINARY_DIR}/shm)
# target_link_libraries(bwtree shm-lib)
# # target_include_directories(bwtree PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# target_include_directories(bwtree PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
