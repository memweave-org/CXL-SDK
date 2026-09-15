# Specify the minimum version of CMake
cmake_minimum_required(VERSION 3.10)

# Set the project name
project(YCSBC)

# Specify the C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)

set(BWTREE_MAPPING_TABLE_BITS 24 CACHE STRING
    "BwTree mapping-table size as a power of two (24 for paper-scale runs)")
add_compile_definitions(BWTREE_MAPPING_TABLE_BITS=${BWTREE_MAPPING_TABLE_BITS})

find_package(Libssh QUIET)
find_path(LIBSSH_INCLUDE_DIR libssh/libssh.h)
find_library(LIBSSH_LIBRARY NAMES ssh libssh libssh.so.4)
if(NOT TARGET Libssh::Libssh)
  if(LIBSSH_INCLUDE_DIR AND LIBSSH_LIBRARY)
    add_library(Libssh::Libssh UNKNOWN IMPORTED)
    set_target_properties(Libssh::Libssh PROPERTIES
      IMPORTED_LOCATION "${LIBSSH_LIBRARY}"
      INTERFACE_INCLUDE_DIRECTORIES "${LIBSSH_INCLUDE_DIR}")
  elseif(NOT Libssh_FOUND)
    message(FATAL_ERROR "libssh not found (install libssh).")
  endif()
endif()

if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|i386|i686")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -g -Wall -latomic -mcx16 -march=native -msse4.1")
else()
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -g -Wall -latomic -march=native")
endif()
#  -fsanitize=address -fno-omit-frame-pointer

# set(CMAKE_BUILD_TYPE Debug)
set(CMAKE_BUILD_TYPE Release)

# Include directories
include_directories(include)

# Specify the source files
file(GLOB SUBSRCS "core/*.cc" "ycsbc.cc" "db/db_factory.cc" "db/thread_pool.cc" "follower/*.cc")

set(public_link_libs "")
set(interface_link_libs "")

macro(add_db db_file db_dir lib_name program_macro link_type)
    list(APPEND SUBSRCS db/${db_file})
    if(${link_type} STREQUAL "PUBLIC")
        list(APPEND public_link_libs ${lib_name})
    elseif(${link_type} STREQUAL "INTERFACE")
        list(APPEND interface_link_libs ${lib_name})
    elseif(${link_type} STREQUAL "HEADER")
        list(APPEND header_link_libs ${lib_name})
    endif()
    if(${link_type} STREQUAL "PUBLIC" OR ${link_type} STREQUAL "INTERFACE")
        add_subdirectory(../../ds/${db_dir} ${CMAKE_BINARY_DIR}/${db_dir})
    else()
        include_directories(../../ds/${db_dir})
    endif()
    add_compile_definitions(${program_macro})
endmacro()

add_subdirectory(../../shm-lib ${CMAKE_BINARY_DIR}/shm-lib)
list(APPEND public_link_libs shm-lib)

if(VARIANT STREQUAL "limit_atomic")
    set(ATOMIC_IMPL_TYPE "PageFault" CACHE STRING "Atomic implementation type" FORCE)
    add_subdirectory(../../atomic atomic)
    list(APPEND public_link_libs atomic-lib)
endif()

add_db("bwtree_db.cc" "BwTree" "bwtree" "ENABLE_BWTREE_DB" "PUBLIC")
add_db("clevel_hash_db.cc" "ClevelHash" "clevel-hash" "ENABLE_CLEVEL_HASH_DB" "INTERFACE")
add_db("clht_db.cc" "CLHT" "clht" "ENABLE_CLHT_DB" "PUBLIC")
add_db("hot_db.cc" "HOT" "hot" "ENABLE_HOT_DB" "INTERFACE")
add_db("masstree_db.cc" "Masstree" "masstree" "ENABLE_MASSTREE_DB" "PUBLIC")
add_db("radix_art_olc_db.cc" "RadixART" "ARTSynchronized" "ENABLE_RADIX_ART_OLC_DB" "PUBLIC")
# add_db("btree_db.cc" "btree" "USE_BTREE")
add_db("btree_olc_db.cc" "BTreeOLC" "btreeolc" "ENABLE_BTREE_OLC_DB" "HEADER")

# Link libraries

option(VARIANT "Build variant" "cc")
message(STATUS "VARIANT: ${VARIANT}")
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# add_compile_definitions(TRX_LATENCY)

# Define the executable
macro(add_ycsbc_executable target_name compile_definitions)
    string(REPLACE "ycsbc_" "" short_target_name ${target_name})
    string(TOUPPER ${short_target_name} upper_target_name)
    if(VARIANT STREQUAL ${short_target_name})
        message(STATUS "DEF: ${compile_definitions}")
        add_compile_definitions(${compile_definitions})
        add_executable(${target_name} ${SUBSRCS})
        target_include_directories(${target_name} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
        # Create unique library targets for each executable
        foreach(lib ${public_link_libs})
            target_link_libraries(${target_name} ${lib})
            get_target_property(lib_type ${lib} TYPE)
            if(lib_type STREQUAL "INTERFACE_LIBRARY")
                target_compile_definitions(${lib} INTERFACE ${compile_definitions})
            else()
                target_compile_definitions(${lib} PUBLIC ${compile_definitions})
            endif()
        endforeach()
        foreach(lib ${interface_link_libs})
            target_link_libraries(${target_name} ${lib})
            target_compile_definitions(${lib} INTERFACE ${compile_definitions})
        endforeach()
        # 查找并链接系统库，避免依赖 RadixART 才带上它们
        find_library(JemallocLib jemalloc)
        if(JemallocLib)
            target_link_libraries(${target_name} ${JemallocLib})
        endif()
        target_link_libraries(${target_name} pthread tbb atomic ssh numa memkind)
        set_target_properties(${target_name} PROPERTIES ENABLE_EXPORTS ON)
        set_target_properties(${target_name} PROPERTIES POSITION_INDEPENDENT_CODE TRUE)
    endif()
endmacro()

# Set different build options
set(OPT_FLAGS "OPT_GC;OPT_ROOT_READ;OPT_IN_USE_FLAG;OPT_REPLICATE_META;OPT_BATCH_FLUSH;OPT_CLEVEL_ROOT_READ")

add_ycsbc_executable(ycsbc_cc "")
add_ycsbc_executable(ycsbc_limit_atomic "LIMIT_ATOMIC;NOCC_FLUSH_NODE")
add_ycsbc_executable(ycsbc_cc_mq "USE_MSG_QUEUE")
add_ycsbc_executable(ycsbc_cc_gc_flush "BWTREE_GC_FLUSH_THREAD_DEFAULT=true;TRX_TYPE_STAT")
add_ycsbc_executable(ycsbc_cc_mq_gc_flush "USE_MSG_QUEUE;BWTREE_GC_FLUSH_THREAD_DEFAULT=true;TRX_TYPE_STAT")
add_ycsbc_executable(ycsbc_cc_nocc_mq "USE_MSG_QUEUE;USE_NO_CC_QUEUE")
add_ycsbc_executable(ycsbc_nocc "NO_CC;${OPT_FLAGS};QUEUE_SIM_CAS")
add_ycsbc_executable(ycsbc_nocc_sim "NO_CC;${OPT_FLAGS};NT_SIM")
add_ycsbc_executable(ycsbc_nocc_dup_flag "NO_CC;${OPT_FLAGS};OPT_DUP_IN_USE_FLAG=4")
add_ycsbc_executable(ycsbc_nocc_no_opt "NO_CC")
add_ycsbc_executable(ycsbc_nocc_part_opt "NO_CC;OPT_GC;OPT_ROOT_READ")
add_ycsbc_executable(ycsbc_nocc_clevel_part1 "NO_CC")
add_ycsbc_executable(ycsbc_nocc_clevel_part2 "NO_CC;OPT_BATCH_FLUSH")
add_ycsbc_executable(ycsbc_nocc_bwtree_part1 "NO_CC;OPT_GC")
add_ycsbc_executable(ycsbc_nocc_bwtree_part2 "NO_CC;OPT_GC;OPT_ROOT_READ")
# Latency test
add_ycsbc_executable(ycsbc_lat_cc "TRX_LATENCY")
add_ycsbc_executable(ycsbc_lat_nocc "TRX_LATENCY;NO_CC;${OPT_FLAGS};QUEUE_SIM_CAS")
add_ycsbc_executable(ycsbc_lat_nocc_no_clflush "TRX_LATENCY;NO_CC;${OPT_FLAGS};QUEUE_SIM_CAS;NO_CLFLUSH")
add_ycsbc_executable(ycsbc_lat_nocc_no_opt "TRX_LATENCY;NO_CC")
add_ycsbc_executable(ycsbc_lat_nocc_no_opt_no_clflush "TRX_LATENCY;NO_CC;NO_CLFLUSH")
# add_executable(test test.cc)
