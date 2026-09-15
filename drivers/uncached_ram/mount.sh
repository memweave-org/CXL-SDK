#!/bin/bash

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)" || exit 1

# 设置设备路径，如果未提供参数则使用默认值
if [ -z "$1" ]; then
  DEVICE_PATH="/dev/uncached_mem_dev"
  echo "未提供设备路径，使用默认值: $DEVICE_PATH"
else
  DEVICE_PATH="$1"
  echo "使用提供的设备路径: $DEVICE_PATH"
fi

# 编译内核模块
echo "正在编译内核模块..."
make -C "$SCRIPT_DIR"
if [ $? -ne 0 ]; then
  echo "错误：编译内核模块失败。" >&2
  exit 1
fi
echo "编译成功。"

# 检查模块是否已加载
if lsmod | grep -q "^uncached_ram\s"; then
  echo "发现已加载的 uncached_ram 模块，正在尝试卸载..."
  sudo rmmod uncached_ram
  if [ $? -ne 0 ]; then
    echo "错误：卸载 uncached_ram 模块失败。" >&2
    # 即使卸载失败，也可能需要继续尝试安装
  else
    echo "模块卸载成功。"
  fi
fi

# 安装（加载）内核模块
echo "正在安装（加载）内核模块..."
sudo make -C "$SCRIPT_DIR" install
if [ $? -ne 0 ]; then
  echo "错误：安装（加载）内核模块失败。" >&2
  exit 1
fi
echo "模块安装（加载）成功。"

echo "正在尝试查找 shm-ds 的主设备号..."

# 从 dmesg 获取主设备号 (在模块加载后执行)
# 查找包含 "Created char device, major:" 的最新行，并提取数字
MAJOR=$(sudo dmesg | grep 'Created char device, major:' | tail -n 1 | sed 's/.*major: \([0-9]*\).*/\1/')

# 检查是否找到了主设备号
if [ -z "$MAJOR" ]; then
  echo "错误：无法在 dmesg 中找到主设备号。" >&2
  echo "请确保内核模块已加载。" >&2
  exit 1
fi

echo "找到主设备号: $MAJOR"

# 如果设备节点已存在，则先删除
if [ -e "$DEVICE_PATH" ]; then
  echo "正在删除已存在的设备节点 $DEVICE_PATH"
  sudo rm -f "$DEVICE_PATH"
  if [ $? -ne 0 ]; then
    echo "错误：删除已存在的设备节点 $DEVICE_PATH 失败。" >&2
    exit 1
  fi
fi

# 创建字符设备节点
echo "正在创建字符设备节点 $DEVICE_PATH (主设备号 $MAJOR, 次设备号 0)"
sudo mknod "$DEVICE_PATH" c "$MAJOR" 0

# 检查 mknod 是否成功
if [ $? -ne 0 ]; then
  echo "错误：创建设备节点 $DEVICE_PATH 失败。" >&2
  echo "请确保您拥有必要的权限（可能需要以 root 身份运行）。" >&2
  exit 1
fi

# 可选：设置设备节点权限
sudo chmod 666 "$DEVICE_PATH"
echo "设置权限为 666: $DEVICE_PATH"

echo "成功创建设备节点 $DEVICE_PATH"

exit 0
