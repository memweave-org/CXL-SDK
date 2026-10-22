# UBSM 混合缓存布局

在 Sherman、RPC 和 BwTree/ClevelHash OPT、NOOPT case 中，将普通数据和共享原子目标分别放到
两个对象。此布局只在 UBSM 的 `UB_SPLIT` 构建中启用，不改变 file/CXL/Memlink 算法路径。

| 对象 | 创建 flag | 固定地址 | 内容 |
| --- | --- | --- | --- |
| data | CACHE | 0x400000000000 | BwTree 节点/增量内容、ClevelHash KV、YCSB value |
| atomic | NONCACHE \| WR_DELAY_COMP | 0x500000000000 | 控制区、CAS 探针、索引根/映射表、ClevelHash 桶槽、共享原子元数据 |

提供方由 `--provider-host` 指定，可与执行机器不同。两个 worker 映射同名的两个
对象。`results.json` 记录两者名字、请求大小、模式、创建/销毁状态及布局。
只有双方成功校验、停止后台线程并完成全部 unmap 后才销毁对象；失败则保留现场。

## 元数据分离

- Sherman 的全局锁表、根指针表以及共享复制序列计数器在 NC；树节点在 CACHE。
  节点读操作在每次快照重试时失效缓存，写操作在释放锁/序列计数器前写回。
- RPC 的队列头尾、槽位状态及请求/响应控制区在 NC，后端 BwTree 使用相同混合布局。

- BwTree 的索引对象（含映射表、根、统计/epoch 原子）、复制数组和 GC 元数据在 NC。
  Cacheable 节点中的 AllocationMeta tail/next、ElasticNode 状态位改为引用 NC 单元。
  节点 payload 仍在 Cacheable 区，节点发布前的缓存写回继续生效。
- ClevelHash 的索引对象、桶槽、level 和复制元数据在 NC；KV 和 value 在 Cacheable。
  发布 KV 时写回完整记录，读取 KV/value 前执行缓存失效。
- 含原子字段的小型元数据结构整体放在 NC，而不是把同一页映射成两种内存类型。
  没有为同一物理页同时建立 CACHE/NC 别名。
- 混合模式下 cxl_vector 等共享辅助数组从 NC 分配。这是保守的元数据放置方案，
  不承诺所有不含原子的字节都被拆入数据区。

## 分配和缓存维护

每个对象划分两个不重叠的进程分配区。分配偏移原子在本地普通 DRAM，避免 memkind
内部元数据在 Cacheable UB 页面执行原子操作。两区均为进程生命周期内只增不回收。
每次分配按至少 64 字节对齐并取整；BwTree delta chunk 也按缓存行分开，避免
不同主机发布同一缓存行中各自修改的部分。耗尽时失败，不回退到私有堆。

Cacheable 区执行 x86 clflush 或 ARM dc cvac/civac，并使用对应全局屏障；NC 区
不执行缓存维护。共享 nt/nt_pointer 原子入口带地址检查，发现目标落入 Cacheable
UB 区立即报错。它不拦截任意第三方机器指令，不能代替新增数据结构的地址审计。
UB 路径始终不增加模拟 CAS 延迟。

## 用法

```sh
UB_IMAGE=shm-pcc-sdk:ub-glibc2.39-gcc12.3.1 \
SIZE_MB=1024 ATOMIC_SIZE_MB=512 \
./tests/two-process-ycsb/run_ub_single.sh 内存提供机器的UB拓扑主机名 2 4
```

通过单机 Docker 脚本测试 Sherman/RPC：设置 `CASES="sherman rpc"`；
不设置时仍默认测试 BwTree OPT 和 ClevelHash OPT。

直接运行 run.py 时：`--ub-layout split --atomic-size-mb 512`。
默认 auto 对 UBSM Sherman、RPC、BwTree/ClevelHash OPT/NOOPT 选择 split；其他 case 保留 legacy。
构建目录带 `-ubsm-split` 后缀，与旧二进制隔离。

ClevelHash 默认预分配桶和后台扩容需要较多 NC 容量，小规模测试也不能仅按记录数
估计原子区大小。每个 worker 的 `UB_ALLOCATIONS` 打印两个区的已用字节数。

## SDK 与验证边界

按照所给 API，CACHE 要配合 ownership 且 BIOS 不能配置 snoop。本实现每个进程
map 后调用 ownership(RW)，整个测试期间持有它，结束且所有线程/进程不再访问
数据后调用 ownership(NONE)。不会在每次索引访问时使用 ownership 做串行交接。
必须在现场确认此 SDK/BIOS 支持所需的同时 RW 映射及软件缓存维护；若 SDK 拒绝，
明确失败，不会退回 NC 或自动变更服务配置。

替身验证两个对象的创建、ownership 调用、两个真实进程、读写/校验和释放，不能
模拟不一致缓存或证明真实远端 NC 的原子语义。split_contract 另验证地址放置、
拒绝在 CACHE 区执行受检查原子操作、NC 区跳过缓存维护和分配边界。

2026-10-22 回归：x86 和 ARM Docker 中，SDK 替身下 BwTree OPT、ClevelHash OPT
均通过两个进程（2/4 线程）、10,000 条记录、每进程 20,000 次操作的运行与校验。
另通过 NOOPT、非 UB file 后端回归、分配契约与 UB 不注入模拟延迟检查。
这些结果不代表已在真实 UB 硬件验证。

Sherman/RPC 扩展回归：x86 本机及 ARM Docker 的 SDK 替身测试均通过，
两个进程分别 2/4 线程，10,000 条记录、20,000 次操作（RPC 仅客户端计操作）。
另通过 Sherman/RPC 非 UB file 后端回归。真实 UB 缓存/原子语义仍需现场验证。
