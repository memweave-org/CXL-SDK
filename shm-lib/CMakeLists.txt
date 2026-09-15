cmake_minimum_required(VERSION 3.10)
project(shm-lib)

# 设置C++标准
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 查找numa库
find_library(NUMA_LIB numa)

find_package(Libssh QUIET)
find_path(LIBSSH_INCLUDE_DIR libssh/libssh.h)
find_library(LIBSSH_LIBRARY NAMES ssh)
if(NOT TARGET Libssh::Libssh AND LIBSSH_INCLUDE_DIR AND LIBSSH_LIBRARY)
  add_library(Libssh::Libssh UNKNOWN IMPORTED)
  set_target_properties(Libssh::Libssh PROPERTIES
    IMPORTED_LOCATION "${LIBSSH_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${LIBSSH_INCLUDE_DIR}")
endif()
set(SHM_LIB_FULL_SSH FALSE)
if(Libssh_FOUND)
  set(SHM_LIB_FULL_SSH TRUE)
elseif(LIBSSH_INCLUDE_DIR AND LIBSSH_LIBRARY)
  set(SHM_LIB_FULL_SSH TRUE)
endif()

# 根据架构设置编译器标志
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|i386|i686")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -march=native -msse4.1 -g -Wall")
else()
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -g -Wall")
endif()

# 包含头文件目录
include_directories(include)

# 添加源文件
file(GLOB_RECURSE SOURCES
    "clstale/*.cc"
    "msg/*.cc"
    "shm/*.cc"
    "utils/*.cc"
    "connection/*.cc"
)

list(FILTER SOURCES EXCLUDE REGEX ".*/connection/establish.*\\.cc$")
if(SHM_LIB_FULL_SSH)
  list(APPEND SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/connection/establish.cc")
else()
  list(APPEND SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/connection/establish_stub.cc")
  message(WARNING "libssh CMake package or dev headers not found: using establish_stub.cc (FollowerManager SSH disabled). Install libssh-devel for full support.")
endif()

# 添加静态库
add_library(shm-lib STATIC ${SOURCES})

target_compile_definitions(shm-lib PUBLIC USE_CXL)

option(WITH_CXLALLOC "Link cxlalloc-static; enables INI allocator_backend=cxlalloc" OFF)
set(CXLALLOC_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../allocator/cxlalloc" CACHE PATH "cxlalloc Rust workspace root")

# 链接 shm-lib 依赖的系统库
find_library(MEMKIND_LIB memkind)
if(NUMA_LIB)
    target_link_libraries(shm-lib PRIVATE ${NUMA_LIB})
else()
    message(WARNING "NUMA library not found; shm-lib may fail to link/run on NUMA systems")
endif()
if(MEMKIND_LIB)
    target_link_libraries(shm-lib PRIVATE ${MEMKIND_LIB})
else()
    message(WARNING "memkind library not found; shm-lib uses memkind APIs")
endif()

if(SHM_LIB_FULL_SSH)
  if(Libssh_FOUND)
    target_link_libraries(shm-lib PRIVATE Libssh::Libssh)
  else()
    target_include_directories(shm-lib PUBLIC ${LIBSSH_INCLUDE_DIR})
    target_link_libraries(shm-lib PRIVATE ${LIBSSH_LIBRARY})
  endif()
endif()

if(WITH_CXLALLOC)
  set(CXLALLOC_STATIC_LIB "${CXLALLOC_ROOT}/target/release/libcxlalloc_static.a")
  find_program(CARGO_EXECUTABLE cargo HINTS "$ENV{HOME}/.cargo/bin" "$ENV{USERPROFILE}/.cargo/bin")
  if(NOT EXISTS "${CXLALLOC_STATIC_LIB}" AND CARGO_EXECUTABLE)
    message(STATUS "WITH_CXLALLOC: building cxlalloc-static via cargo")
    execute_process(
      COMMAND ${CARGO_EXECUTABLE} build --release -p cxlalloc-static
      WORKING_DIRECTORY "${CXLALLOC_ROOT}"
      RESULT_VARIABLE _cxl_cargo_rc)
    if(NOT _cxl_cargo_rc EQUAL 0)
      message(FATAL_ERROR "cargo build for cxlalloc-static failed (rc=${_cxl_cargo_rc})")
    endif()
  endif()
  if(NOT EXISTS "${CXLALLOC_STATIC_LIB}")
    message(FATAL_ERROR "WITH_CXLALLOC requires '${CXLALLOC_STATIC_LIB}'. Install Rust/cargo and run: (cd \"${CXLALLOC_ROOT}\" && cargo build --release -p cxlalloc-static)")
  endif()
  target_compile_definitions(shm-lib PUBLIC SHM_LIB_WITH_CXLALLOC)
  target_include_directories(shm-lib PUBLIC "${CXLALLOC_ROOT}/cxlalloc-static/include")
  target_link_libraries(shm-lib PUBLIC
    "${CXLALLOC_STATIC_LIB}"
    dl pthread m stdc++ gcc_s)
endif()

# 公开头文件目录
target_include_directories(shm-lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
