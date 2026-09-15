#!/bin/bash

# 设置颜色输出
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# 打印带颜色的信息
message() {
    echo -e "${BLUE}[MESSAGE]${NC} $1"
}

warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

error() {
    echo -e "${RED}[ERROR]${NC} $1"
    exit 1
}

info() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

# 检查命令是否存在
check_command() {
    if ! command -v $1 &> /dev/null; then
        error "$1 is required but not installed"
    fi
}

# 检查必要的命令
check_command cmake
check_command make

# 获取脚本所在目录的绝对路径
SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
STM_DIR="$(dirname "$SCRIPT_DIR")/../TXMem"

# 默认STM实现
STM_IMPL=${1:-"tl2"}
CLEAN=0
STM_BUILD_DIR="$STM_DIR/$STM_IMPL"

if [ "$2" = "clean" ]; then
    CLEAN=1
    rm -rf build
fi

# 编译STM
info "Building STM lib $STM_IMPL ..."
cd "$STM_BUILD_DIR" || error "Failed to change directory to $STM_BUILD_DIR"

# 根据不同的STM实现使用不同的编译命令
case $STM_IMPL in
    "tl2")
        if [ $CLEAN -eq 1 ]; then
            info "Cleaning TL2..."
            rm -rf build
        fi
        cmake -B build || error "CMake configuration failed"
        cmake --build build || error "Make failed"
        ;;
    "tinystm")
        # 编译TinySTM
        if [ $CLEAN -eq 1 ]; then
            info "Cleaning TinySTM..."
            make clean || error "Make clean failed"
        fi
        make || error "Make failed"
        ;;
    "swisstm")
        # 编译SwissTM
        if [ $CLEAN -eq 1 ]; then
            info "Cleaning SwissTM..."
            make clean || error "Make clean failed"
        fi
        make || error "Make failed"
        ;;
    *)
        error "Unsupported STM: $STM_IMPL support tl2, tinystm, swisstm"
        ;;
esac

info "Completed building STM lib $STM_IMPL"

# 配置和编译STAMP
info "Building STAMP benchmarks with $STM_IMPL"
cd "$SCRIPT_DIR" || error "Failed to change directory to $SCRIPT_DIR"
cmake -DSTM_IMPL=$STM_IMPL -B build || error "CMake configuration failed"
cmake --build build || error "Make failed"

info "STAMP build completed successfully; Run with: ./run.sh -a <app> ..."
