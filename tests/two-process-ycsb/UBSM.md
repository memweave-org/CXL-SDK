# 全局 UB API 选择与最新 UBS Memory 接入

全局构建选项为 `UB_API=none|memlink|ubsm`；环境变量 `SHM_UB_API` 是首次
CMake 配置的默认值，也供 `run.py`、`scale.py`、`build.sh` 使用。切换后端需要重新编译；
`run.py` 为不同 API 使用独立构建目录，并在 `--skip-build` 时检查缓存，禁止静默切换。
此选项选择 SDK 和访问策略，不会把旧 YCSB 配置中的 `mem_type=local` 自动改成 UB。
双机 UBSM 实验使用本目录的执行入口。

| 选择 | 映射 API | 对象生命周期 |
| --- | --- | --- |
| `none` | 普通文件/设备 mmap | 普通文件由 runner 独占创建；设备由用户提供 |
| `memlink` | `MemlinkMemShmMmap` | 用户预先创建/导入，runner 不销毁对象 |
| `ubsm` | `ubsmem_shmem_map` | runner 在 A 创建，A/B 映射，双方成功退出后 A 释放 |

`WITH_UB=ON` 只保留为旧 Memlink 构建兼容入口；不能与 `UB_API=ubsm` 混用。

## 配置

最新 UBSM（两端头文件、动态库必须为匹配版本）：

```sh
export SHM_UB_API=ubsm
export UBSM_INCLUDE_DIR=/usr/local/ubs_mem/include
export UBSM_LIBRARY=/usr/local/ubs_mem/lib/libubsm_sdk.so
export LD_LIBRARY_PATH="/usr/local/ubs_mem/lib:${LD_LIBRARY_PATH:-}"
export HCOM_CONNECTION_RETRY_TIMES=2
```

旧 Memlink：

```sh
export SHM_UB_API=memlink
export UB_MEMLINK_LIBRARY=/实际路径/libmemlink.so
```

普通文件验证：`export SHM_UB_API=none`。命令行 `--ub-api` 优先于环境变量。
直接构建任意使用 shm-lib 的项目也可传 `-DUB_API=ubsm`、`-DUBSM_INCLUDE_DIR=...`、
`-DUBSM_LIBRARY=...`；SDK 链接和相关编译宏经 shm-lib 传给消费目标。

## Docker 编译

在机器 A 的仓库根目录执行，仓库在两机应使用相同绝对路径。下面要求厂商 SDK 安装
于 `/usr/local/ubs_mem`，其传递依赖也已经在镜像中可用；若出现缺库，先根据 SDK 的
`ldd` 输出补齐对应厂商库，不要把宿主机整个 `/usr/lib64` 覆盖到容器中。

```sh
docker run --rm --user "$(id -u):$(id -g)" \
  -v "$PWD:$PWD" -w "$PWD" \
  -v /usr/local/ubs_mem:/usr/local/ubs_mem:ro \
  -e SHM_UB_API -e UBSM_INCLUDE_DIR -e UBSM_LIBRARY \
  -e LD_LIBRARY_PATH -e HCOM_CONNECTION_RETRY_TIMES \
  shm-pcc-sdk:ub-glibc2.39-gcc12.3.1 \
  python3 tests/two-process-ycsb/run.py --build-only \
  --mapping-bits 16 --output "build/ubsm-build-$(date +%Y%m%d-%H%M%S)"
```

八种配置均不包含 Deft；UBSM 构建禁止 128 位 CAS。`--build-only` 不分配 UB 对象，
无需传 provider 或名字。将相同代码、`build/two-ycsb/*-ubsm` 和 workload 文件部署到 B
的相同路径。两机需兼容 CPU/运行库；默认 `-march=native`，不能任意跨 CPU 使用。
runner 会核对两端 worker 二进制与 workload SHA256，相同源码分别构建不保证哈希相同。

旧 YCSB 的 Docker 构建脚本也会转发上述 API 环境变量；`UB_SDK_ROOT` 可指定额外
只读 SDK 挂载目录。但旧入口的配置和跨机启动方式不由这个开关自动改变。

## 双机运行

先通过附件的 UBSM 双向读写、owner/remote 竞争 8-byte CAS/FAA 测试。
需要 ubse/ubsmd 正常、两端访问权限和 UID/GID 匹配，以及符合附件 flag 的硬件 snoop 配置。

仅在 A 启动协调脚本；B 通过 SSH 启动一个 worker。请替换实际 hostname、NUMA 和 SSH 地址：

```sh
export UBSM_PROVIDER_HOST=实际UB拓扑中的A主机名
export UBSM_PROVIDER_NUMA=0   # 必须是实际有内存且可提供 UB 资源的节点

python3 tests/two-process-ycsb/run.py \
  --memory ub --ub-api ubsm --host user@machine-b \
  --shared-path ycsb-ub --skip-build \
  --threads 2 4 --records 10000 --ops 10000 \
  --size-mb 1024 --mapping-bits 16 \
  --output "build/ubsm-run-$(date +%Y%m%d-%H%M%S)"
```

`--shared-path` 在 UBSM 模式是名称前缀，不是已有对象名。每个 case 会追加随机后缀，
准确名字实时打印并写入 results.json。A 是 provider，B 不分配第二个对象。两端映射
相同物理对象到 `0x400000000000`；前 8 MiB 是控制区，剩余内存划为两个独立 memkind
分区。B 的分配分区仍物理位于 A，不能解释为对称 NUMA 实验。

宿主机运行必须具备与 Docker 产物兼容的 glibc/依赖。若需要也在 Docker 内运行，
在两机预先启动同名且具备厂商规定设备、UDS、组权限和 SDK 依赖的容器，例如 `ub-ycsb`，
并挂载相同绝对路径的仓库，然后追加：

```sh
--worker-prefix 'docker exec ub-ycsb'
```

此参数同样用于 A 的对象管理程序及两端 worker。远端容器内应设置 LD_LIBRARY_PATH、
HCOM_CONNECTION_RETRY_TIMES；普通 SSH 不保证传递本地环境变量。超时命令在容器内部
执行。脚本不猜测 UBSM 服务 socket/设备路径，也不自动授予 privileged 权限。

## 实现和退出约定

- 最新 API 路径：init_attributes → initialize → allocate_with_provider（仅管理进程）/
  map（两个 worker）→ unmap → finalize；双方成功且确认 unmap 后，管理进程 deallocate。
- map 请求整个对象、offset=0、MAP_SHARED | MAP_FIXED_NOREPLACE；返回错误地址直接失败。
- 固定创建 flag 为 ONLY_IMPORT_NONCACHE | WR_DELAY_COMP：owner cacheable、remote NC。
  此构建关闭软件缓存维护和模拟 CXL 延迟，保留屏障；ARM 屏障使用 dmb sy。
  禁止 128 位 CAS/快照，不能切换 flag 绕过硬件要求。
- ClevelHash 创建方先停止并 join 后台扩容线程，再 unmap。测试保持进程生命周期分配策略，
  不启用跨进程 memkind free/BwTree 后台 GC；这不是完整论文性能复现。
- 任一创建、worker、映射、解除映射或 finalize 失败，均不自动释放/复用对象。
  若容器或 SSH 被中断，应先确认两端 worker 全部停止，再用厂商工具按精确名字检查清理。
  也可在确认无人使用后，使用对应构建的 `ub_admin destroy 精确对象名`。

## 验证边界

当前没有厂商 SDK 和真实双节点 UB；回归通过明确标记 `UBSM_TEST_STUB` 的测试替身
验证接口调用、生命周期及双进程索引操作，不能证明 SDK ABI、NC 映射上的 memkind/
硬件原子支持或真实跨节点一致性。生产构建必须指定厂商头文件和动态库，不能使用
`tests/ub-api/mock`。结果会标记 `ubsm_test_stub=true`，绘图脚本拒绝把替身结果当作性能数据。
