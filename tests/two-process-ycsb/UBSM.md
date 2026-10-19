# UBS Memory 双节点接入分析

依据用户提供的 `ubs-mem-two-node.zip` 中 README、`ubsm_test_common.h`、
owner/remote 示例和原子竞争测试，对照当前双进程 YCSB 入口。此文是接入方案，
不是已经完成的 UBSM 后端；当前 `--memory ub` 仍调用 Memlink。

## 分配路径

附件使用 `ubs_mem.h`、`ubs_mem_def.h` 和 `libubsm_sdk.so`，不是
`MemlinkMemShmMmap`，不能把 UBSM 库直接传给现有 `--ub-library`。

1. A、B 各自初始化 SDK：`ubsmem_init_attributes` → `ubsmem_initialize`。
2. 仅 A 调用 `ubsmem_shmem_allocate_with_provider`，指定 A 的拓扑 hostname、
   实际有内存的 NUMA 节点、唯一对象名、总大小、0660 权限。
3. A、B 都调用 `ubsmem_shmem_map`，使用相同对象名和大小。B 不再 allocate。
   UBS Engine/OBMM 负责 export/import；应用无需自行交换 EID/token。
4. 两端将映射区域交给各自的小对象分配器；索引节点和 payload 放在该区域内。
   SDK 分配的是大块共享 backing，不是每次节点 malloc 都调用 SDK。

按附件接口整理的核心调用如下（省略初始化、错误处理和同步，不是独立程序）：

```cpp
// 仅 A；provider.host_name 必须是拓扑中 A 的 hostname。
ubs_mem_provider_t provider{};
// 填写 host_name；socket_id/numa_id/port_id 未指定项取 UINT32_MAX。
uint64_t flags = UBSM_FLAG_ONLY_IMPORT_NONCACHE | UBSM_FLAG_WR_DELAY_COMP;
int rc = ubsmem_shmem_allocate_with_provider(
    &provider, name, bytes, 0660, flags);
// 检查 rc == UBSM_OK，再通知 B 对象已就绪。

// A、B 都执行；api_description.md 明确列出 MAP_FIXED_NOREPLACE。
void* mapped = nullptr;
rc = ubsmem_shmem_map(requested_base, bytes, PROT_READ | PROT_WRITE,
                     MAP_SHARED | MAP_FIXED_NOREPLACE, name, 0, &mapped);
// 检查 rc、非空地址以及 mapped == requested_base。
```

现有入口预留前 8 MiB 给控制区，其余按 2 MiB 对齐后平分：

```text
同一物理共享对象（provider=A）
  [控制区 8 MiB][进程 0 分配分区][进程 1 分配分区][对齐余量]
A: 映射整个对象，memkind_create_fixed 只管理分区 0
B: 映射整个对象，memkind_create_fixed 只管理分区 1
```

这可以沿用当前 `MemoryManager` 的分区思路，但 memkind 在 NC 映射上的实际
访问/原子要求也需要验证。两进程不能用两个独立 allocator 管理同一个分区。
当前生命周期策略禁止运行期间跨进程释放对象。
**B 的分区仍然是 A 上的物理内存**，分区仅表示分配权限；如果目标是各自在本机
分配、对方远程访问，需要 A/B 各提供一个对象、双方都映射两个对象，另做双 arena
布局及地址发布，不能把单 arena 结果解释成对称 NUMA 布局。

## 现有 YCSB 还缺什么

| 接入点 | 必要修改或验证 |
| --- | --- |
| `shm-lib/shm/ub.cc` / CMake | 新增独立 UBSM 后端，链接匹配版本头文件及 SDK；保留 Memlink 的明确区别 |
| `run.py` | 新增后端选择、provider、对象创建就绪通知、退出协调；尚未实现 `--memory ubsm` |
| `main.cc` Mapper | 两端请求同一虚拟地址并检查实际结果；当前地址为 `0x400000000000` |
| 地址能力 | 后续提供的 `api_description.md` 明确支持 `MAP_FIXED_NOREPLACE`；两端可请求同一 VA，仍需检查返回地址，并确认部署 SDK 版本符合文档。仅支持整个对象映射，length 为对象 size、offset 为 0 |
| 内存访问 | 按 owner cacheable / remote NC 实现访问和内存屏障；不能直接照搬现有 x86 CLFLUSH/CLWB、SSE non-temporal 指令 |
| 原子操作 | 先验证 owner 与 remote 对同一地址的 8-byte FAA/CAS；UB 不支持 Deft 所需的 16-byte CAS，Deft 已从测试矩阵移除 |
| 分配器 | 验证 memkind/其依赖的读写及原子在该映射上受支持；不能用进程私有锁模拟跨机原子 |
| 退出 | 当前 `_Exit` 绕过析构，需显式完成远端 unmap 通知、owner unmap/deallocate、各端 finalize |

附件 flag 的含义是 **owner cacheable、remote non-cacheable，并开启写延迟补偿**。
它不自动改写本仓库的分配器、指令或同步协议。附件约定正常 load/store 无需软件
flush/invalidate，但这不表示可以删除所有内存屏障。若目标为 ARM，还需移植当前
x86 专用代码和构建选项。BwTree/ClevelHash 的本地多进程验证不等于 UBSM 双机验证。

正常生命周期应为：A 创建并映射 → B 映射 → 双方运行及验证 → B 停止访问并 unmap
→ TCP 通知 A → A unmap、deallocate → 各端 finalize。异常退出时不应猜测远端已经
释放；保留精确对象名供双方停止后检查清理，不能直接复用或强删残留对象。

## 先在两台 UB 机器验证附件通路

下面从附件解压目录执行，两台机器均安装匹配版本 SDK。`OWNER_IP`、`OWNER_HOST`
及 `OWNER_NUMA` 为实际环境值；两端设置相同且本轮独占的 `ARENA_NAME`。

```sh
cmake -S . -B build-ubsm \
  -DUBSM_INCLUDE_DIR=/usr/local/ubs_mem/include \
  -DUBSM_LIBRARY=/usr/local/ubs_mem/lib/libubsm_sdk.so
cmake --build build-ubsm -j
```

A 先启动（外层 timeout 同时覆盖示例中未设截止时间的 accept 等待）：

```sh
timeout 180s ./build-ubsm/ubsm_one_sided_owner \
  --bind-ip "$OWNER_IP" --provider-host "$OWNER_HOST" \
  --provider-numa "$OWNER_NUMA" --port 18525 --name "$ARENA_NAME" \
  --region-mb 128 --test-bytes 4096 --iterations 100000 --timeout-sec 120
```

B 启动另一个真正独立的进程：

```sh
timeout 180s ./build-ubsm/ubsm_one_sided_remote \
  --owner-ip "$OWNER_IP" --port 18525 --name "$ARENA_NAME" \
  --region-mb 128 --test-bytes 4096 --iterations 100000 --timeout-sec 120
```

通过双向数据校验和正常清理后，再按附件 `benchmarks/README.md` 的 4.3 节运行
`ubsm_owner_remote_atomic_contention_test`。它检查双方竞争同一个 8-byte word
的 FAA/CAS ticket 是否无重复、无缺失，并检查最终计数；只测 remote 侧原子不足以
验证 owner/remote 竞争。之后另加固定 VA、分区分配和 16-byte CAS 验证，才进入索引测试。

环境要求以附件为准：ubse/ubsmd 可用、用户可访问服务 UDS、两端 UID/GID 及对象
权限匹配、provider 有实际内存和 OBMM 资源池、对应 snoop 能力开启。
申请大小至少 4 MiB 且为其倍数；附件提示实际 block 可能为 128 MiB，以上使用 128 MiB。
这里的 128 MiB 是通路测试大小，不能直接套作所有 YCSB case 的容量配置。

## 本次验证边界

本次完成附件源码与仓库分配/映射/退出路径的静态核对。当前环境为 x86_64，标准
安装路径未发现 UBSM 头文件或 `libubsm_sdk.so`，也未提供两台目标机器的连接信息。
因此未编译或运行真实 SDK，未确认固定 VA、硬件一致性及跨机原子能力。
此前 Memlink stub 和本地双进程结果不作为 UBSM 硬件验证证据。
