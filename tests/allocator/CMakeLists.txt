cmake_minimum_required(VERSION 3.20)

project(malloc_tests)

set(LSMALLOC_DIR "../../allocator/lsmalloc")
set(CXLSHM_DIR "../../allocator/cxl-shm")

set(TEST_NAMES
    perf_rw
)

# Create test executable
foreach (TEST_NAME IN LISTS TEST_NAMES)
    add_executable(${TEST_NAME} ${TEST_NAME}.cpp)
    target_include_directories(${TEST_NAME} PRIVATE ${LSMALLOC_DIR}/include ${CXLSHM_DIR}/include)
    target_link_directories(${TEST_NAME} PRIVATE ${LSMALLOC_DIR}/build ${CXLSHM_DIR}/build)
    target_link_libraries(${TEST_NAME} lsmalloc cxlshm pthread atomic)
endforeach()

# Enable testing
enable_testing()
foreach (TEST_NAME IN LISTS TEST_NAMES)
    add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
endforeach()
