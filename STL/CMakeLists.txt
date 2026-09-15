cmake_minimum_required(VERSION 3.10)
project(STL LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../shm-lib ${CMAKE_BINARY_DIR}/shm-lib)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../ds/BwTree ${CMAKE_BINARY_DIR}/bwtree)

add_library(STL STATIC
    src/init.cc
    src/btree.cc
    src/clevelhash.cc
)

target_include_directories(STL PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../ds/BwTree/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../ds/ClevelHash
    ${CMAKE_CURRENT_SOURCE_DIR}/../shm-lib/include
)

target_compile_definitions(STL PUBLIC USE_CXL)
target_link_libraries(STL PUBLIC shm-lib bwtree pthread numa atomic)

if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    target_compile_options(STL PRIVATE -mcx16)
endif()

add_executable(simple_demo examples/simple_demo.cc)
target_link_libraries(simple_demo PRIVATE STL)

add_executable(multi_process_demo examples/multi_process_demo.cc)
target_link_libraries(multi_process_demo PRIVATE STL)

install(TARGETS STL ARCHIVE DESTINATION lib)
install(TARGETS simple_demo multi_process_demo RUNTIME DESTINATION bin)
install(DIRECTORY include/pcc DESTINATION include)
