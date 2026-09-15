#!/bin/bash

build_lsmalloc() {
    echo "Building LSMalloc ..."
    cd $1/../../allocator/lsmalloc
    cmake -B build && cmake --build build
    cd $1
}

build_cxlshm() {
    echo "Building CXLSHM ..."
    cd $1/../../allocator/cxl-shm
    cmake -B build && cmake --build build
    cd $1
}

PWD=$(pwd)

build_lsmalloc $PWD
build_cxlshm $PWD

echo "Building test ..."
cmake -B build && cmake --build build

echo "Running test ..."
./build/perf_rw -a lsmalloc -d /dev/shm/cxl -t 16
./build/perf_rw -a cxlshm -d /dev/shm/cxl -t 16
