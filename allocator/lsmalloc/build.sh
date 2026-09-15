#!/bin/bash

# 创建构建目录
mkdir -p build
cd build

# 配置 CMake
cmake ..

# 构建
make -j$(nproc)

# 运行测试
if [ "$1" == "test" ]; then
    ctest --output-on-failure
fi
