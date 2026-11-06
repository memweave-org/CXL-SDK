# 原 YCSB-C 框架的 CXL/UB 多进程模式

## 八种模式：run_all

CXL 在同一台机器运行独立 A/B 进程，全部八项均使用该模型：

```sh
cd /mnt/disk1/wfn/CXL-SDK/tests/YCSB-C
bash run_all.sh --backend cxl --dry-run
bash run_all.sh --backend cxl --thread-counts 24 --cxl-node 4
```

CXL 默认 A 整个进程绑定 NUMA0、B 绑定 NUMA1，可用 `CXL_CPU_NODE_A/B` 配置；
共享内存节点仍由 `--cxl-node` / `CXL_NODE` 指定，默认 NUMA4。
包含 RPC 的矩阵默认每端 24 个客户端，RPC 每端另外启动 24 个服务线程，
分别占用所在 NUMA 节点的 48 个逻辑 CPU；仅运行非 RPC 项时默认每端 32 个客户端。
A/B 客户端和服务 CPU 集合互不重叠；显式线程数超过本 NUMA 可用容量会在构建前报错，
不会跨 NUMA 补充 CPU。默认保留 `config.sh` 的 48 GiB CACHE/每端 RPC 本地池、16 GiB 共享
通信区（两端各占约一半），Sherman 两项使用 64 GiB 的单一共享 NC 区。
CXL 的 `nc-direct` / `nc-memcpy` 使用与 UB 相同的 Sherman 节点和访问路径，但
CXL 文件映射可缓存，原子访问走 queue 模拟；这里不声称真实 UB NC 硬件语义。
ClevelHash 的层回收逻辑在两种多进程构建中都启用。

UB 两机拉取同一版本并配置 `ub/config.sh`，在 B 先执行、A 随后执行：

```sh
cd /mnt/disk1/wfn/CXL-SDK/tests/YCSB-C
bash run_all.sh --dry-run
bash run_all.sh
```

| case | 编译模式 | A（100% read） | B（90% read / 10% update） | 节点内存 |
| --- | --- | --- | --- | --- |
| bwtree | NOOPT | 直接读 | 本地索引 | CACHE |
| bwtree+opt | OPT | 直接读 | 本地索引 | CACHE |
| bwtree+rpc | 原 cc_nocc_mq | 本地 MQ / 远端 MQ 读 | 本地 MQ / 远端 MQ 读写 | 两端本地 DRAM |
| clevelhash | NOOPT | 直接读 | 本地索引 | CACHE |
| clevelhash+opt | OPT | 直接读 | 本地索引 | CACHE |
| clevelhash+rpc | 原 cc_nocc_mq | 本地 MQ / 远端 MQ 读 | 本地 MQ / 远端 MQ 读写 | 两端本地 DRAM |
| sherman+nc-direct | NOOPT | NC 字段直接 load | NC 字段直接 load/store | NC |
| sherman+nc-memcpy | NOOPT | memcpy 到本地读取 | memcpy 到本地，修改后复制回 NC | NC |

所有 case 的 A 都只读，禁止 UPDATE/INSERT；B 使用 90% read / 10% update，
update 替换已有 key 的 value。两端均使用 Zipfian 分布，每项新建共享对象，
使用相同初始记录数、每端操作数、线程数和 `SEED`，并核对各端操作类型/key
序列 hash 在相同 worker 数的八种模式间一致。B 的比例由随机抽样决定，小样本不保证恰好 90/10。
UB 默认每种模式依次测试每端 **32、64、128 个 worker**，共 24 轮；CXL 完整矩阵默认每端 **24**，共 8 轮，A/B 数量相同。
默认初始插入 10,000,000 个 key，每端每轮共 10,000,000 次操作（两端合计 20,000,000），由该端全部 worker 分摊。
A 执行 10,000,000 次查询；B 约 9,000,000 次查询、1,000,000 次更新，查询和更新均针对初始 key 集合。
可用 `--thread-counts 32 64` 或 `THREAD_COUNTS="32 64"` 选择档位；启动时检查
当前进程允许的 CPU 数，并按允许的编号绑定 worker，支持 CPU 编号不连续。
首次可在两端用相同参数执行：

```sh
RECORDS=1000 OPS=1000 bash run_all.sh --thread-counts 32
```

BwTree/ClevelHash 继续使用已有启动 CACHE 失效及发布修复，A CACHE 只读、B 可写。
普通模式不分配 RPC 队列、不启动服务线程。`+rpc` 在 A/B 各建立一个本地 DRAM
索引，按 `key % 2` 分区（0 属于 B，1 属于 A）；`RECORDS` 是两端合计的初始记录数。
两端分别装载自己的记录。本机 key 进入原 ThreadPool 的本地 MQ，远端 key 通过双向共享 MQ 请求所属进程，统一由原 ThreadPool 的服务线程执行。
BwTree RPC 的本地 memkind 固定池允许 epoch GC 释放并复用旧节点；不能开启
`retain_until_exit`，否则 `free()` 会变成空操作，加载/更新时的累计分配会耗尽池。
`SIZE_MB` 控制每端本地池容量，整机空闲内存不会自动扩展该池；默认仍为 48 GiB。
A 的 workload 仍全读，B 的 workload 仍为 90% read / 10% update；B 更新 A 所属的 key 时，
由 A 的服务线程更新 A 的本地索引。

复用 `db/thread_pool.cc` 的工作线程循环、随机桶遍历和空队列 pause/yield，以及
`shm-lib/include/msg/mpmc_queue.h` 的 MPMCQueue。每个方向默认按接收端服务线程数分桶，
按未完成 RPC 数选择队列（`RPC_QUEUE_POLICY=fixed` 恢复按 client 分桶），跨进程只传 slot ID 和 value 字节，
每个 client 最多一个在途请求。共享 NC 只保存队列、请求/回复 payload、启动描述及跨机同步；
索引控制对象、原子元数据、节点、value 都在所属机器的 DRAM，RPC 模式不创建 CACHE 映射。
读回复和更新请求传递完整 value 字节；对端不会解引用发送方的 DRAM 指针。
每端服务线程数等于该端客户端数，两组分别绑定独立 CPU；CXL 同机运行的 A/B 四组也互不重叠。
本地 lambda 和本地 MQ 的存储均留在本地 DRAM，不传送捕获的私有引用。
因此每端 N 个客户端需要 2N 个逻辑 CPU；CXL 同机双进程需要 4N 个。
所有两进程模式的 pthread 句柄、线程参数、Client DB 列表和 master 的计时/启动对象
均在本地 DRAM；用于跨机分发的 trace 暂存、索引描述和 barrier 保留在共享 NC。
结果核对本地/远端操作计数及 `A.rpc_sent == B.rpc_completed`、
`B.rpc_sent == A.rpc_completed`，也按 workload 的 key 分区核对发送次数。
`A_WRITABLE=1` 仍会在启动前报错，不能开启 A workload 更新。

Sherman 两项都只 mmap 一块 NC 对象，节点、value、共享根和锁在 NC，
DB 包装对象、Tree/DSM、线程句柄及启动参数在本地 DRAM，
不创建 CACHE 映射。A/B 使用相同访问机制，A 只读索引，只有 B 更新，RPC 完成数为 0。
NC 映射仍需可写以支持控制信息及 epoch 等元数据；这不允许 A 执行索引更新。
`nc-direct` 直接遍历 NC 节点字段，B 在锁和版本保护下原地修改、分裂节点；
`nc-memcpy` 将完整节点复制到本地查询，B 在本地修改后复制修改部分回 NC。
value 也分别使用直接访问或本地复制。两项都关闭本地内部节点缓存，保持相同节点布局；
每个 1024 字节节点占一个 2 KiB 对齐的分配槽并带独立 NC 版本头，避免修改/分裂时版本锁别名。
版本头按槽定位，因此同一个 4 KiB 系统页内的两个节点也使用独立版本计数器。
这也避免 memkind 1.13 将 `posix_memalign(4096,4096)` 扩成 16 KiB usable 加额外页面的空间浪费。
若仍发生分配失败，日志会报告请求大小、对齐、进程分配池大小及 allocated/active；
`memkind_create_fixed` 失败则报告实际返回码和 `memkind_error_message`，共享分配禁止回退到堆。
NC 模式不再分配旧的 4 MiB 哈希版本表；CACHE 模式仍使用它。独立版本头和哈希版本表
都是适配层机制，与上游节点/叶子记录内嵌的 front/rear 版本字段不同。
Sherman 的 NC 包含节点、value 和原框架的 trace/控制数据，默认 65536 MiB（64 GiB），
由 `SHERMAN_NC_SIZE_MB` 单独配置；耗尽即报错。
这里的 NC 是 UB SDK 的 non-cacheable 映射，不是 cacheable 内存上的 streaming 指令。

每端构建包含三种索引的 NOOPT/OPT/RPC 三份二进制，按 case 复用。镜像需包含 CityHash、
Boost coroutine/context 开发库；`Dockerfile.openEuler` 已配置这些依赖。
UB 打包构建在 Docker 内，CXL 在本机编译；二者均调用原 `tests/YCSB-C/build.sh`，worker 在物理机运行；Sherman 不需要 RDMA 或 memcached。
`--build-only --output /absolute/path/build-result` 只构建，后续用
`--bundles /absolute/path/build-result/bundles` 复用。两端握手核对 case、访问模式、
参数、workload 和二进制 SHA256，结果还检查实际 OPT 和 NC 模式，旧 bundle 不能冒充新模式。

单项也可通过同一入口选择，例如两端各执行：

```sh
YCSB_UB_CASE=bwtree+rpc bash run.sh
YCSB_UB_CASE=sherman+nc-direct bash run.sh
YCSB_UB_CASE=sherman+nc-memcpy bash run.sh
# 使用批量 workload 语义的 BwTree 非 OPT：
YCSB_UB_SUITE=1 YCSB_UB_CASE=bwtree bash run.sh
```

以上命令逐轮运行，等 A/B 均退出后再切换。未指定 case 的旧 `run.sh` 仍保留下面的默认行为。
按配置中的 IP 自动识别角色，也可设置 `YCSB_UB_ROLE=A` / `B`。不设置 `YCSB_UB_FIXTURE`。

结果位于 `ub/result-all-A-*` / `ub/result-all-B-*`，CXL 位于 `ub/result-all-local-*`，每轮使用
`<case>/threads-24`（CXL 默认）或 `threads-32/64/128`（UB 默认）子目录，包含 worker/control 日志、
配置和 `result.json`；`summary.json` / `summary.csv` 汇总所有轮次并记录两端 worker 数。
NOOPT/OPT/RPC 各构建一次，全部轮次复用；每轮新建共享对象，清理完成后再进入下一轮。
两端握手核对完整模式列表、线程数列表和当前轮次。失败立即停止，失败目录写入
`invalid.json`，失败样本不计入成功汇总。
`run_all.sh` 和 `run.sh` 的线程扫描都不检测其他进程、CPU 负载或其他测试，不等待
机器空闲，也不因外部负载中止。自身命令失败仍会停止后续轮次。

`test_run_all_control.py` 检查 24 轮调度、参数传递、构建复用、失败停止和汇总，不运行索引或计时。
`test_run_all.py --bundles /absolute/path/fixture-bundles` 用文件映射 SDK 替身执行八项
真实双进程接线检查（固定每端 1 worker、35 条记录、每端 128 次操作），结果不能用于 UB 性能或缓存一致性结论。
可选 CMake 参数 `YCSB_BUILD_SHERMAN_CONTRACT=ON` 构建 `sherman_nc_contract`，
仅供 fixture SDK 下验证双进程对称插入、分裂、更新和读取，不输出吞吐或耗时。

### 不计时的本地功能断言

使用 file-backed SDK fixture 构建时，可增加 `-DYCSB_BUILD_READONLY_RPC_CONTRACT=ON`，
同时设 `-DBWTREE_MAPPING_TABLE_BITS=10 -DYCSB_CLEVEL_HASH_POWER=10`，构建
`readonly_rpc_contract`。设置独立的 `UBSM_TEST_DIRECTORY` 后，分别传入 `queue` 或
上表中的 case；OPT 两项必须运行 OPT 构建。该测试使用两个进程、四个 A client，
验证实际操作生成器、A workload 更新/插入拒绝、B 更新可见性及 RPC 完成计数。
`bwtree+rpc` / `clevelhash+rpc` 还验证两端私有分区、双向路由、完整 value 字节传输、
远端更新的 value 独立副本，以及装载/扩容/读写期间 NC 分配量不增长。
`queue` 额外用容量为 2 的原有 MPMC 队列核对重复复用和各 client 的回复。
`bwtree+legacy` 覆盖未设置 suite 的默认入口。
这不是无 CC 硬件模拟，也不输出吞吐或延迟；测试不要求独占资源。

同一 `YCSB_BUILD_READONLY_RPC_CONTRACT=ON` 构建在启用 ClevelHash 时提供
`ub_reuse_contract`。用 fixture SDK、独立 `UBSM_TEST_DIRECTORY` 运行 OPT/NOOPT
两份产物，断言小 arena 下反复释放复用、对齐、并发、跨分区释放拒绝，以及
ClevelHash 重复插入、失败更新和扩容临时对象的内存有界性。此项也不计时。

## BwTree OPT 的进程私有 mapping cache

协调运行的 OPT 构建启用 `BWTREE_PROCESS_MAPPING_CACHE=1`。每个进程、每棵树只分配
一张 cached mapping table，同进程的 worker 共用它。表使用匿名私有 mmap，并在首次
触页前绑定到本机有 CPU 的 DRAM NUMA 节点；可用 `BWTREE_MAPPING_CACHE_NODE` 指定
其中一个节点。共享树内只保留权威 mapping table 和清理消息通道，不存私有表地址。
默认 24-bit 表每项只有一个 64-bit 原子值，共 128 MiB/进程；NOOPT 不创建该表。
B 在构造树时分配，A 在 worker 注册时分配，两者都在 READY 和吞吐计时之前完成。
启动日志 `BWTREE_MAPPING_CACHE` 包含 pid、表地址、大小、`invalidation=per-worker-batch-ack`
和 `fill=busy-slot`。NOOPT 也注册消息接收线程，但日志中的 `tables=0 bytes=0`，
不分配软件 mapping table。

GC 为每个 worker 使用独立的共享 NC 单写者环形队列，每批最多 64 个 node ID 及其
待回收内存范围描述。范围列表保存在 NC，由写端枚举完整分配块（包含变长节点尾部、
inline delta 的所有扩展 chunk，以及单独分配的 remove/abort 节点），接收端无需
读取可能过期的 CACHE 节点来确定地址和长度。
每条队列最多保留 16 批。常规发送仅用 load/store 发布，不争用全局 tail、CAS 或锁；
不同 worker 的批次没有全局顺序。首次出现更大的 worker ID 时用一次 CAS 公布队列范围，
进程注册/退出用 CAS 更新成员位图，这些均不在每批发送的竞争路径上。最多支持 512 个
GC worker ID，覆盖当前每端最多 192 worker 的双端脚本。

一次通知同时完成软件 mapping cache 清理和 CPU cacheline 失效。后台接收线程先清完
该批的 mapping 表项，再记录 epoch，等待所有可能持有清理前旧指针的读操作退出，
随后对该批内存范围执行 `clflush` 及完成屏障，最后发布 ACK。等待期间该批保留在队列中，
接收线程继续轮询其他 worker 队列；不会重复清掉已回填的新 mapping，也不再发送第二轮通知。
一次 drain 只为已完成的连续批次写一次 ACK。GC 收到所有仍注册接收端的 ACK 后才能
释放节点和 NC 范围描述，允许分配器复用地址。NOOPT 同样执行 cacheline 失效和 ACK。
一条队列满只延后该 worker 的回收，不阻塞其他 worker 发布。
普通读不等待消息。接收线程空闲时休眠 50 微秒；缓存不会因 epoch 改变而整体失效。
队列 payload 是 NC 普通读写，使用发布与 ACK 的 acquire/release（UB 的全系统屏障）
保证写完整批后才能消费、清完整批后才能复用槽位。注册先加入接收者集合，再快照各
队列头；私有空缓存只在注册结束后开放给读线程，避免注册期间队列绕回覆盖未处理数据。
本地命中只对表项做一次 acquire load。回填先用 CAS 取得表项的忙标记，再读取权威
mapping table 并发布指针；接收线程与回填互斥，等已有回填结束后才清空，之后仍须
完成读者宽限期和 cacheline 失效才能 ACK。
读线程遇到忙标记或竞争失败时直接读 NC，不等待、不回填。这样旧操作不会在清理
之后重新填入先前保存的旧指针，也不需要版本字段。叶节点仍检查权威映射：叶链头
未变时不做 CAS、不重新读节点头；变化时在取得忙标记后重新读一次 NC，保证回填顺序。
持有忙标记的线程被抢占会延后清理和 GC，其他读线程仍可继续。
A/B 的 OPT/NOOPT 都在解除共享映射前停止接收线程、注销缓存。
此协议保证回收复用时不遗留旧读者缓存；它不能阻止硬件在失效后重新预取尚未发布的地址，
因此不能据此宣称任意预取场景下都满足缓存一致性。UB 原生 NC 原子的后端保持原样。

这会改变共享树和节点分配元数据布局，A/B 必须同步源码并重新构建 OPT/NOOPT bundle，不能混用旧二进制。
本地构建可增加 `-DYCSB_BUILD_MAPPING_CACHE_CONTRACT=ON`，生成不计时的
`mapping_cache_contract`，覆盖两个进程的私有表、同进程线程复用、清理等待已有回填、
忙表项读绕过、批量 ACK 时序、四发送者独立队列、队列满不互相阻塞、异常释放忙标记、
定向清理及退出/重建，以及合并通知的宽限期、完成屏障前禁止 ACK、待处理批次不阻塞其他
worker 队列、NOOPT 接收端退出。用显式屏障
固定回填与清理的交错顺序，不依赖线程速度；普通共享 mmap 不代表 NC 硬件验证。
前述 SDK fixture 的 `readonly_rpc_contract bwtree+cache-gc` 另覆盖真实 BwTree 的
回收路径（70 个退休节点，跨越两个批次）：A 在清表后继续持有并读取旧节点时，禁止 ACK 和释放；
读端退出宽限期后，通过测试专用链接包装验证 CACHE 对象释放及旧地址复用，A 再次读取
同一地址时必须看到新 header 和变长节点尾部内容。启用 `YCSB_UB_LOCAL_NCC=ON` 时该
contract 也进行普通读写插桩，可用独立 CACHE 副本验证实际缓存行失效。
模拟构建的 `bwtree+cache-gc-no-invalidate` 是预期失败的负对照：仅在 A 完成启动失效后
屏蔽后续失效，复用地址后必须报旧读或新内容校验失败，不能算作通过。
运行这些用例前仍须完成测试资源互斥检查；性能结论还需独占环境下的 OPT/NOOPT 对比。

## BwTree 线程数扫描与单项入口

两进程框架中的 BwTree、ClevelHash（包括 OPT/NOOPT/RPC）在计时前，
由每个 worker 将自己的 workload 操作描述符复制到进程私有 DRAM。
分配优先绑定到该 worker CPU 所在 NUMA 节点；若该节点没有内存或不在当前
cpuset 允许的内存节点中，则选择允许范围内距离最近的有内存节点（也可为无 CPU 的
内存节点）。核对页面位于选定节点后才发布 READY；
计时循环读取本地数组，结果标记 `operation_storage=local-dram`。
原共享数组仅供准备阶段分发同一条 trace，仍保留到回收阶段；key、顺序和 value 指针不变，
索引 value 本身继续位于共享数据区。日志 `WORKLOAD_STORAGE` 记录各线程的位置和操作数，
其中 `node` 为选定的内存节点，`cpu_node` 为 CPU 所在节点。RPC 本地索引也采用此回退策略。

无内存 NUMA 节点的轻量回归可通过 `-DYCSB_BUILD_NUMA_CONTRACT=ON` 构建并运行
`numa_contract`。它模拟节点拓扑和 cpuset，覆盖无内存 CPU 节点、受限节点、
节点编号大于 63、距离信息缺失及无可用内存节点，不进行性能测量。

同一个 `run.sh` 用 `--backend ub|cxl` 选择运行后端（默认 `ub`）：

```sh
# UB：A、B 各执行一次，复用原 SDK 和 NC 原子路径。
bash ./run.sh --backend ub --thread-counts 32
# 本地 CXL：执行一次，自动启动 A/B 两个进程，各 32 个 worker。
bash ./run.sh --backend cxl --thread-counts 32 --cxl-node 4
# BwTree RPC：两端本地 DRAM 分区，双向 RPC 队列位于 CXL。
YCSB_UB_CASE=bwtree+rpc bash ./run.sh --backend cxl --thread-counts 24 --cxl-node 4
```

CXL 模式复用原 `CXLSystemMemoryMmapper`、memkind 和 `QUEUE_SIM_CAS`，
不加载 UB SDK。OPT/NOOPT 均显式启用 queue：`nt`/`nt_pointer` 的远端
load/store/CAS 继续走原 `AtomicRequestQueue`；显式本地访问仍沿用原绕过逻辑。
`clwb`/`clflush` 直接执行本机 cacheline 指令（x86 默认 CLWB/CLFLUSHOPT），
不会因共享地址分区而转到 UB 的地址登记分支。节点发布仍在原来的位置写回，
BwTree 不增加逐次读节点的 flush。编译时拒绝关闭 flush 或混用 UB 原子后端；
启动日志显示 `atomic_backend=queue cache_maintenance=native`。
两个进程仍走同一个 `peer.py`、原操作生成器和事务控制器：
A 100% 读，B 90% 读/10% update，默认 OPT、fresh-insert、无 RPC。
每端 1,000 万次操作、1,000 万条初始记录，线程数是每个进程的 worker 数。
本地 CPU 集合不重叠，A 限定 NUMA0、B 限定 NUMA1；可用 `CPU_A`、`CPU_B`
指定各自 NUMA 节点内的起始 CPU。CXL 普通模式默认 32 档，RPC 默认 24 档，
可通过 `--thread-counts` 指定，但客户端和服务线程必须全部放得进各自的 NUMA 节点。

CXL 在 `/dev/shm` 创建本轮独占的共享文件，将节点和元数据映射绑定到指定的
CXL NUMA 节点，并在 READY 前核对实际页面所在节点。这里的元数据用 queue 模拟
原子访问延迟，物理映射可缓存；A 的节点映射仍为只读。A 解除映射后 B 才删除文件。
原 `run_shm_ds.sh -mode=cxl_two_process --thread-counts 32` 也转到这一入口。
此 A/B CXL 入口支持 BwTree OPT/NOOPT（`OPT=1/0`）和 `YCSB_UB_CASE=bwtree+rpc`。
RPC 构建原 `cc_nocc_mq`（`USE_MSG_QUEUE;USE_NO_CC_QUEUE`，不定义 `NO_CC`），
按 `key % 2` 分区：本地操作进入本地 MQ，远端操作通过共享 MQ 传送请求和 value 字节。两端的树、原子元数据、节点及 value 均在本地
DRAM 的 memkind 池中；CXL 只存放共享控制对象、队列和通信缓冲区，不创建共享节点文件。
`SIZE_MB` 是每端本地索引池容量，`NC_SIZE_MB` 是 CXL 共享通信区域总容量；此处
沿用 NC 配置名，但 CXL 映射可缓存，原子访问使用 queue 模拟。
RPC 保持 A 100% 读、B 90% 读/10% update，更新已有 key（`replace-existing-key`），
结果校验双向请求/完成数量、分区路由和本地索引存储。可添加 `--dry-run` 检查配置。
旧 CXL 测试模式继续可用。

两种后端都会明确显示构建或复用 bundle，并将编译、等待对端和运行日志同步显示到
终端。CXL 默认原生编译，UB 默认 Docker 编译；每次扫描只构建一次。
`--bundle` 只能复用同一后端的产物；二进制会在映射前拒绝后端不匹配。

两机同步完整源码，在各自物理机执行一次：

```sh
cd /mnt/disk1/wfn/CXL-SDK/tests/YCSB-C
bash ./run.sh
```

默认 OPT，`run.sh` 顺序运行每端 **32、64、128 个事务 worker**。
每档 A/B 的 worker 数相同，A 100% read，B 90% read / 10% update，不使用 RPC。
每端只编译一次；每档使用新共享对象，等两端完成清理后才进入下一档。
`bash run.sh --dry-run` 可查看计划；`THREAD_COUNTS="32 64" bash run.sh` 可选择部分档位。
启动前检查最大档位需要的可用逻辑 CPU 数，可用 `CPU_A`、`CPU_B` 指定各端起始 CPU。
从进程允许的 CPU 列表中按编号选择，跳过编号空洞；每个 worker 绑定一个不同的 CPU。
例如允许 `0-49,96-239` 时，128 档使用 `0-49,96-173`，不会因缺少编号 50 而失败。
日志、`manifest.json` 和每档 `config.json` 记录实际 CPU 列表；总数不足时明确报出
可用数量，不自动把多个 worker 挤到同一个 CPU。更新后需要重新编译，旧 bundle 不支持此绑定方式。
两端握手核对扫描列表和当前档位；本轮执行失败即停止，
当前档保留 `invalid.json`，不进入下一档。
结果位于 `ub/result-threads-A-*` / `ub/result-threads-B-*` 的 `threads-32` 等子目录，
`summary.json` / `summary.csv` 汇总各档结果；每档的 `launcher.log` 包含完整控制与 worker 输出。
事务 worker 数不包括主线程、B 的 GC 线程、Python 控制器或 SDK 自建线程。

保留单档入口：`YCSB_UB_SINGLE=1 THREADS_A=32 THREADS_B=32 bash run.sh`。
测试 NOOPT 时，两边都用 `OPT=0 bash ./run.sh`。
也可使用原入口 `bash ./run_shm_ds.sh -mode=ub_two_host`。
旧目录 `tests/ub-bwtree-ycsb/run.sh` 现在也转到此入口。
配置统一在 **`tests/YCSB-C/ub/config.sh`**，旧目录的 config.sh 不再生效。

- A：`76.53.137.81`，100% read，原框架 follower（machine 1）。
- B：`76.53.137.82`，90% read / 10% update，原框架 master（machine 0）。
- B 先用原 `LoadManager` 加载，再共享同一个 `BwTreeDB`。
- 两边运行原 `TransactionThreadController → Client → BwTreeDB`。
- 使用原 `CoreWorkload`、`OpGeneratorIntKeyAddr` 和 Zipfian 分布；只调整各主机的操作比例与分组。
- 保留原 `UpdateInternal` 的随机新 key 插入语义，没有改成覆盖旧 key。
- 每档各 10,000,000 次操作、10,000,000 条初始数据；操作数是每台总数，不是每线程总数。
  单档模式仍支持 A/B 不同线程数，默认各 1 个 worker。

## 内存和编译

| 对象 | 相同 VA | A | B | 内容 |
| --- | --- | --- | --- | --- |
| CACHE | `0x300000000000` | READ | RW | BwTree 节点、delta、value；ClevelHash OPT 的不可变 `level_meta` 和 helper 描述符 |
| NONCACHE + WR_DELAY_COMP | `0x400000000000` | RW | RW | DB/树对象、映射表、epoch、原子根指针及副本槽、线程控制及原框架共享数组 |

使用 `SHM_UB_SPLIT` 和独立的 UB NT backend：`UB_API=ubsm`（或 Memlink）
在编译时为 `nt`、`aligned_nt`、`nt_pointer` 选择 `atomic_ub_*`，直接对对象所在的
mmap 地址执行 CPU 原子 load/store/CAS/fetch 操作。共享原子对象由 NC arena 分配；
栈上/进程内临时对象仍可使用。对象本身不保存模拟副本、queue 或计时状态。
NC 的 `flush()` 只执行内存屏障，CACHE 节点仍走原有显式 cache maintenance。

ClevelHash OPT 的 `level_meta` 直接分配在共享 CACHE 区，初始化、扩容及迁移产生的
每个版本在发布前完整写回；查询直接读取该共享对象，不创建进程私有 DRAM 快照。
`meta` 和 `meta_replicas` 的原子指针字、层描述符及 bucket 槽仍在 NC。
NOOPT 的 `level_meta` 保持原 NC 分配。失败发布的元数据从对应分配器释放，
已发布版本继续保留到共享对象退出；两端需同步重新构建后运行。
BwTree 读侧仅在映射启动时整区失效一次，后续节点和 value 直接读取，不执行
逐节点/逐对象的首次读取 flush；写端发布新节点前仍执行原有写回。
UB 构建不编译 queue 模拟源文件；`UB_API=none` 的 NO_CC 模式继续使用原 queue backend。
兼容参数 `nt=false` 不会绕过 UB 原子操作或 CACHE 地址检查。
`run.sh` 自动设置 `UB_API=ubsm` 并重新编译，更新后仍用相同入口，无需额外 backend 参数。
A 正常登记/注销 epoch，可写 NC 中的 OPT 元数据；没有额外改写 `GetType()`。
value 从 CACHE 分配并在发布前写回，避免把 B 的进程私有 malloc 指针传给 A。
NC 的分配空间分成互不重叠的两半，分配偏移位于各进程本地。

`docker build` 内调用原 **`tests/YCSB-C/build.sh` + CMake**，构建
`ycsbc_nocc` 或 `ycsbc_nocc_no_opt`，提取为结果目录内的 `ycsbc`。
容器只编译；Python 控制器与 `ycsbc` 都在物理机运行。
默认镜像 `shm-pcc-sdk:ub-glibc2.39-gcc12.3.1`、SDK `/usr/local/ubs_mem`。
编译为非 PIE，两端校验二进制及 workload SHA256。
CACHE 和 NC 的 SDK provider 默认都使用 B（82）的系统 hostname。
`PROVIDER_CACHE` / `PROVIDER_NC` 可分别指定 SDK 拓扑主机名；不是 TCP 连接 IP。
兼容原 `PROVIDER_B`：显式设置时作为两块内存的共同默认 provider，分项配置优先。
仍由 B 创建两个对象并在双方解除映射后释放，内存所在机器不改变 A/B 分配区的划分。
TCP 控制端口默认 18982。

默认 CACHE 49152 MiB（48 GiB）、NC 16384 MiB（16 GiB），均由 B（82）提供。
Sherman 两项不创建 CACHE 对象，默认分配一块 NC 65536 MiB（64 GiB）共享对象；
其节点和 value 均在 NC，不能沿用其他索引的元数据预算。
NC 分配区由 A/B 各占一半，B 预生成两端全部操作序列；默认每端 1000 万次操作的 trace 数组约 1.8 GiB，
还需容纳索引元数据，以及 Sherman 模式的节点和 value。
`SIZE_MB` 覆盖 CACHE 容量（RPC 中为每端本地索引的 DRAM 容量）；
显式设置 `NC_SIZE_MB` 会覆盖所有 case，`SHERMAN_NC_SIZE_MB` 可进一步单独覆盖 Sherman。
例如 `SIZE_MB=49152 NC_SIZE_MB=16384 SHERMAN_NC_SIZE_MB=65536 bash run_all.sh`。
`--dry-run` 的 `memory_by_case` 和每轮结果记录实际共享容量；RPC 的 CACHE 为 0。
更大负载可能仍需增加容量。耗尽即报错，不回退到私有堆。
UB arena 按进程分区，与 CXL 使用同样的 `memkind_create_fixed` 分配器，
`MemoryManager::free()` 将安全释放的块交给 memkind 复用。UB 不再额外维护全局加锁的
空闲区间表和分配记录表；保留 CACHE/NC 分区和禁止堆回退。
普通 malloc/free 直接调用 `memkind_malloc` / `memkind_free`，不强制 64B 取整；
显式 `posix_memalign` / `clalign` 保留调用方要求的对齐。
跨分区、跨分配器释放仍会拒绝；内部指针和重复释放遵循 CXL/memkind 的未定义行为约定。
生产路径不维护 live/high-water 统计，也不查询 usable size。GC contract 通过仅链接进
测试程序的 memkind 包装观测对象生命周期，不向实际 benchmark 添加统计操作。
ClevelHash 的未发布 KV、失败的扩容数组和元数据会回收；扩容分配串行化以限制临时数组峰值，
普通查询和更新不经过此扩容锁。已发布 KV 尚无跨机回收宽限期，继续保留到对象退出，
不能在替换指针后立即复用。UB 和 CXL 的 BwTree CACHE 节点均允许在原 epoch/GC
确认安全后回收复用；OPT 还等待 mapping-cache 清理 ACK 及之后的读端宽限期。
CXL 保留严格的共享分配要求，但不再强制保留每个已退休的分配。已发布 value 仍保留至退出。
整个 UB 对象在退出时释放。NodeID 的空闲栈没有入栈路径，UB 模式跳过其无用的 128-bit CAS。
所有节点由 B 分配/回收，不启动不需要的跨机 GC 消息队列 dispatcher。

## 启停与统计

Python 只协调两边已启动的进程，替代 SSH 拉起 follower，不执行数据操作。
B 发布原线程控制器，A 完成映射和线程初始化后，两边通过原 NC start/finish 变量运行。
最终吞吐使用原 `MasterTransactionManager` 的 **成功操作总数 / duration**，
`result.json` 中 `common_seconds` 为此原生计时；`controller_seconds` 单独记录 TCP 控制窗口。
原生计时使用 `steady_clock`，避免系统校时影响耗时。启动清缓存、加载和 READY 等待不在
此计时内；计时覆盖 B 放行事务到 A/B 均完成，包含等待较慢一端。Python 以单调时钟
记录更外层的 RUN→双方 RESULT 窗口。若 B 原生耗时超过该窗口加上
`max(50ms, 1% × controller_seconds)` 的容差，结果判为计时异常，不发布有效吞吐。
固定操作数下，较快的一端结束后会等待另一端，因此不是固定时长的稳态测量。
任何操作失败都使汇总失败，不能把失败操作计入成功吞吐。

日志在 `ub/result-A-*` / `ub/result-B-*`，包括 build.log、worker.log、control.log、
config.json、result.json。A 退出并解除两块映射后，B 才销毁 DB、停止 GC、解除映射并释放对象。
异常时报告可能残留的对象，不释放可能还在被另一端访问的共享对象。
`run.sh` 和 `run_all.sh` 均不检测其他任务或 CPU 负载，也不据此等待或中止。

## 小规模验证

`test_framework.py` 用文件映射 SDK 替身和真实 `ycsbc` 验证：35 条加载数据（覆盖加载线程除不尽的余数）、
每端 128 次操作、A 1/B 2 线程、A 的真实 `PROT_READ`、NC RW、原框架输出与双对象释放。
计时丢弃，不作性能结论。它不能验证真实 UB 的缓存可见性和远端原子语义。

```sh
python3 tests/YCSB-C/ub/test_framework.py --binary /absolute/path/ycsbc_nocc --opt 1
python3 tests/YCSB-C/ub/test_framework.py --binary /absolute/path/ycsbc_nocc_no_opt --opt 0
```

上述二进制必须链接文件映射 SDK 替身；不要把这项本机检查指向真实 SDK 二进制。
用于编译的替身头文件/实现见 `tests/ub-bwtree-ycsb/mock/`。

从仓库根目录可独立检查 NT backend 选择及映射内存原子操作：

```sh
python3 tests/ub-api/latency_check.py
out=$(mktemp -d /tmp/ub-nt-check-XXXXXX)
c++ -std=c++17 -shared -fPIC -Itests/ub-bwtree-ycsb/mock \
  tests/ub-bwtree-ycsb/mock/sdk.cc -o "$out/libubsm_sdk.so"
c++ -std=c++17 -O2 -pthread -DSHM_LIB_WITH_UBSM -DSHM_UB_SPLIT \
  -Itests/ub-bwtree-ycsb/mock -Ishm-lib/include \
  tests/ub-api/nt_contract.cc shm-lib/shm/ub.cc \
  -L"$out" -Wl,-rpath,"$out" -lubsm_sdk -lmemkind -o "$out/nt_contract"
UBSM_TEST_DIRECTORY="$out/objects" "$out/nt_contract"
```

本地 worker 的 READY、RESULT、CLOSED 通过专用继承 pipe
（`YCSB_UB_CONTROL_FD`）传给控制器；stdin 只接收控制命令，stdout/stderr 只作日志。
控制器不从日志解析协议，SDK 并发打印或不换行不会吞掉 READY。
A/B 之间仍走独立 TCP 连接。worker 必须通过 run.sh/peer.py 启动。

控制器持续接收对端状态；等待本地 READY、RESULT 或 CLOSED 时也检查对端失败和断连。
任一端失败，另一端中止本轮 worker；SIGTERM 后 2 秒仍未退出则发送 SIGKILL 并等待确认。
仅操作控制器自己启动的子进程，失败结果标记 pass=false，UB 对象仍保留供诊断。

## 本地真实双副本：检查 CoW 发布与启动残留缓存

`YCSB_UB_LOCAL_NCC=ON` 是额外的本地调试构建，不加载真实 UB SDK。
运行的仍是原 `LoadManager / Client` 和 `BwTreeDB / CLevelHashDB`。CACHE 使用 A、B 各自独立的
memfd，同一 VA 对应不同物理存储；公共后备文件只由模拟后端访问。
NC 元数据仍共享，代表现有 UB 的非缓存原子区域。A 的应用映射保持 `PROT_READ`。

这个模型按 **64 字节 cacheline** 为 A、B 分别维护有效位和脏位，保留原有
节点分配布局。默认**两端的每一行都已经缓存了旧零值**，即使公共内存非零也不会
自动更新。公共 UB 映射代码默认在每端映射完成、访问数据之前各执行一次整区失效；
运行器默认不设置启动失效环境变量，以直接验证生产代码的默认行为。
`--no-startup-invalidate` 保留初始残留缓存，`--initial-cache cold` 则选择初始无效行。

这里无效行就是 **EMPTY**，与“有效且内容为零”严格区分：

| 本端行状态 | 下一次读的行为 |
| --- | --- |
| EMPTY（`valid=false`） | 从公共内存取整行，再标记有效 |
| 有效、干净，包括旧零值 | 直接读本端副本，不自动更新 |
| 有效、脏 | 读本端写入值，等待显式写回 |

失效只清有效位，不在失效时复制公共内存，也不清零底层数据。默认路径是：
两端初始有效零缓存 → 各自整区失效为 EMPTY → B 构造、写回并发布 CoW 对象 →
A 首次读取 EMPTY 行时获取已写回的内容 → 后续复用缓存。
默认不开启按对象首次使用失效，也不注入 next-line 预取。

无效行首次读或写时从公共内存填充，部分写入也会先取整行，保留其他字段。
写入把行标为脏；`clwb` 只写回脏行，不刷新或失效任何对端副本；
`clflush` 先写回本端脏行，再将本端行置为无效。干净的旧零值不会被错误写回。
单独 fence 不复制数据。已完整写回并发布的新 CoW 节点，在地址未提前进入缓存时
可以直接读取；如果地址被预取或曾被读取，仍需相应失效协议。

使用 Clang/LLVM 15 的编译插桩，覆盖优化后的普通 load/store、内存复制/清零、
`memcmp/bcmp` 和显式 `__builtin_prefetch`。CACHE 中的原子操作会被拒绝，
原子元数据必须在共享 NC 区。BwTree 不增加按对象首次使用刷新；ClevelHash 原有的
KV pair 读取失效仍然执行，BwTree 则保留其 CoW 读路径。
模拟器本身不插桩，非 CACHE 地址快速跳过。

`--read-ahead-lines N` 在每次读取或写入取行时，额外预取后续最多 N 行（0..64，
默认 0），可以把尚未分配或发布的地址提前缓存。预取不会覆盖已经有效的行。
启动失效之后仍可能再次预取旧数据，因此“一开始刷过”不等于以后不会有旧缓存。
这是确定性的 next-line 策略，不声称重现某款 CPU 的全部预取行为。
它是额外的预取压力场景，和启动时提前失效 cacheline 是两种操作。

BwTree 保留原来的 CoW 读路径：每端映射 CACHE 后，在开始访问前整区失效一次；
之后从 NC 元数据取得已发布节点指针，节点和 value 都直接读取并复用缓存。
`GetType()`、high/low key、depth、item count 等访问器不执行读前 flush，
也不查询首次访问地址集合。写端发布新节点前原有的 `FlushNode()` 写回仍保留。
GC 复用节点地址前则执行上面的批量通知、读者宽限期及各端 cacheline 失效协议。
已移除 BwTree 的 `PrepareRead()` 和 `--cow-acquire` 实验入口；设置
`SHM_UB_COW_ACQUIRE=1` 也不会改变 BwTree 的节点或 value 读取路径。

默认还会对 A 实际读取的字节执行只观察的后备内存比较：不一致就报
`LOCAL_NCC_STALE_READ` 并失败，不修复数据。它能捕获错误非零值和 payload，
而不仅是 NULL 指针或操作返回码；这项检查适用于本测试的不可变 CoW 数据。
`--no-check-reads` 可关闭这个额外诊断，保留原程序的 NULL 指针/失败表现。
需要 Linux、`clang++-15`、LLVM 15 开发文件及原 YCSB 的编译依赖：

```sh
cmake -S tests/YCSB-C -B build/local-ncc-opt \
  -DCMAKE_C_COMPILER=clang-15 -DCMAKE_CXX_COMPILER=clang++-15 \
  -DYCSB_UB_LOCAL_NCC=ON -DVARIANT=nocc '-DYCSB_INDEXES=bwtree;clevel-hash' \
  -DBWTREE_MAPPING_TABLE_BITS=10 -DYCSB_CLEVEL_HASH_POWER=5 \
  -DLLVM_DIR=/usr/lib/llvm-15/lib/cmake/llvm
cmake --build build/local-ncc-opt --target ycsbc_nocc local_ncc_contract -j2

# 默认两端缓存旧零值，映射完成后各自失效一次。
python3 tests/YCSB-C/ub/run_local_ncc.py --build build/local-ncc-opt \
  --output build/ncc-stale-both --records 1000 --ops 2000

# ClevelHash：小初始容量让 1000 条预加载触发扩容，跨行 value 同时覆盖 payload。
python3 tests/YCSB-C/ub/run_local_ncc.py --build build/local-ncc-opt \
  --output build/ncc-clevelhash --db clevelhash --records 1000 --ops 20000 --value-size 192

# 负对照：保留旧零缓存，不做启动失效，预期失败。
python3 tests/YCSB-C/ub/run_local_ncc.py --build build/local-ncc-opt \
  --output build/ncc-no-startup --no-startup-invalidate

# 额外负对照：启动失效后再预取未来地址，旧读路径可能失败。
python3 tests/YCSB-C/ub/run_local_ncc.py --build build/local-ncc-opt \
  --output build/ncc-prefetch --read-ahead-lines 64 --ops 20000

# 初始冷缓存对照，额外保留逐 cacheline 记录。
python3 tests/YCSB-C/ub/run_local_ncc.py --build build/local-ncc-opt \
  --output build/ncc-cold --initial-cache cold --no-startup-invalidate --trace
```

NOOPT 使用独立构建目录，改为 `-DVARIANT=nocc_no_opt`，构建目标
`ycsbc_nocc_no_opt local_ncc_contract`，运行时增加 `--opt 0`。
所有输出目录必须是新目录。`--drop writeback` 或 `--drop invalidate` 用于负对照，
不会把被注入的失败当作通过。`--trace` 保存每条写回、失效、首次填充记录；
整块失效汇总为一条记录，逐行填充/写回仍可能产生较大日志。默认仍记录计数和
故障前最近 32 次缓存维护。`--value-size 192` 可额外覆盖跨 cacheline 的 value。

每轮先跑 `local_ncc_contract`：检查独立副本、未写回不可见、冷 CoW 首次填充、
写回不刷新旧缓存、fence/clean 不等于 invalidate、跨行维护、NC 可见性、A 写保护，
以及两端都缓存旧零值、干净行不能覆盖公共数据、部分写取行、脏行写回后失效、
提前失效后保持 EMPTY、B 写回后无需再次失效便可读到新值，以及显式预取、
next-line 预取、提前失效后又预取旧值。另有独立的可选 acquisition 协议契约，
以及不设置任何启动开关、两端均从旧零缓存正确读取非零后备内存的默认启动契约。
契约测试自行设置这些条件，
不继承本轮索引测试的故障注入或只读比较开关。
然后运行 A/B 各一个事务线程，A 100% 读，B 90% 读、10% 更新已有 key。
A 直接读取索引，不转发写请求；B 是唯一 CACHE 写进程。`--no-suite` 保留旧工作负载。
默认 suite 在事务结束后逐个核对所有访问过的 key，要求 A/B 返回相同的最终 value
地址；再对同一个 key 连续发布 32 个内容各异的新 value，每轮都检查 A 返回的地址
和每个 payload 字节，确认 update 实际生效。验证器不执行额外缓存失效。
`verified_keys / verified_versions` 记录在结果里，这些验证代码只编译进模拟构建。
脚本持续检查同机其他测试和驱动，启动前连续确认
三个空闲快照；运行中发现重叠则终止自己的 worker、标记 `interfered=true`，该轮
必须重跑。`exclusivity.jsonl` 保存机器、UTC 时间、PID、CPU 使用及退出观察。
这是基于可见进程的互斥检查，不能代替对不可见容器/远端调度器的外部协调。

`result.json` 记录正确性结果、二进制 SHA256 和退出码，明确 `performance_valid=false`。
发生 NULL `high_key_p` 时，仅在这个调试构建里打印节点/字段地址、同一缓存行的
本地与后备内存值，随后失败退出；诊断不修复或刷新数据。失败轮保留对象文件和日志。
普通 SDK 构建不包含模拟插桩。**正常 UBSM Cacheable 映射现在默认执行启动整区失效**，
与模拟构建使用同一段代码和缓存维护接口，重新编译后运行 `run.sh / run_all.sh` 即生效，
无需额外设置环境变量。只有显式 `SHM_UB_STARTUP_INVALIDATE=0` 才关闭，供负对照使用。
旧调试环境变量
`LOCAL_NCC_STARTUP_INVALIDATE` 仅作为兼容别名。失效发生在 map/ownership 之后、
分配和读取之前，操作本端 cacheline，不清零数据，也不假设可以远程失效对端。

边界：当前支持 BwTree 和 ClevelHash 的 A CACHE 只读、B 单写进程，每端一个事务线程；不验证多写方
缓存所有权、真实设备原子语义、指令乱序完成或所有硬件预取策略。外部未插桩库、内联汇编
的隐式 CACHE 读取不在普通 load 插桩覆盖范围内，不能直接扩展为任意程序验证器。
`stale` 是显式注入的初始条件，不代表已证明真实 SDK 会留下旧缓存。初始零值
被建模为干净缓存行；跨运行遗留的脏行及其写回所有权不在此模型内。
当前 bump allocator 不重用已释放地址；测试通过不证明地址复用、物理内存重新分配
或大规模并发下的正确性。真实 UB 是否需要启动失效，还取决于 SDK 的 map/ownership
缓存保证，以及真实机器的冷启动与失效对照结果。

### 原 build.sh 入口和多进程 MQ 验证

在 `tests/YCSB-C` 执行：

```bash
# CXL 多进程，生成 build_cc_nocc_mq_cxl/ycsbc_cc_nocc_mq
YCSB_BACKEND=cxl YCSB_INDEXES=bwtree bash ./build.sh rpc

# UB 多进程；使用目标平台的实际 SDK 头文件和动态库
YCSB_BACKEND=ub YCSB_INDEXES=bwtree \
  UBSM_INCLUDE_DIR=/usr/local/ubs_mem/include \
  UBSM_LIBRARY=/usr/local/ubs_mem/lib/libubsm_sdk.so bash ./build.sh rpc
```

`ub/build.sh` 只负责打包和目标平台 Docker 构建，内部仍调用这个入口。`+rpc` 打包必须
选择 `cc_nocc_mq`；批量脚本分别构建 `opt/`、`noopt/`、`rpc/` 三个 bundle。
旧 NOOPT RPC bundle 会被构建模式或结果校验拒绝。未设置后端时，原有单进程构建保持兼容。

`test_multiprocess_mq.py --backend cxl --bundle DIR --output DIR` 验证 1/1、2/2、3/2
客户端组合。UB 本地测试加 `--backend ub --fixture`，要求 bundle 链接文件映射 SDK fixture。
`YCSB_BUILD_READONLY_RPC_CONTRACT=1` 可通过原 build.sh 构建 `readonly_rpc_contract`；
RPC contract 使用 `rpc` variant，验证完整 64 字节 value、远端更新后的独立副本、路由和
索引活动不额外分配 NC。fixture 不代表真实 UB 硬件或缓存一致性验证。

多进程 RPC 默认使用 `RPC_QUEUE_POLICY=least-outstanding`：每个接收端按服务线程数
建立远端请求队列，发送端选择未完成请求最少的队列，随机起点打散相同负载的选择；
队列满或竞争失败时尝试同一接收端的其他队列。响应槽仍按客户端独占，路由不会改变
key 的所属分区。`RPC_QUEUE_POLICY=random` 保留随机选队列及失败重试，便于比较；
`RPC_QUEUE_POLICY=fixed` 恢复原先每客户端一个队列的固定路由。由 B 在创建共享队列时
确定策略，A 按共享配置执行；`RPC_PARTITION` 日志记录实际策略和发送目标队列数。

Outstanding 统计的是远端队列中已提交但尚未消费响应的 RPC，覆盖排队、执行及返回等待。
每组队列只有一个对端发送进程，因此计数由该发送进程的所有客户端线程共享，放在本地
DRAM，避免新增 UB/NC 原子访问；它不包括接收端线程池的本地任务，也不是服务端总负载。
目前仍是每客户端线程最多一条在途请求，尚未接入协程窗口或自适应限流。
同一选项构建的 `rpc_queue_router_contract` 验证慢队列避让、失败提交回滚和并发计数；
`readonly_rpc_contract queue` 验证多客户端共享较少队列时的跨进程响应隔离。

CXL 全矩阵每项保存 `A/`、`B/` 的 worker/control 日志和 `cxl.json` 映射信息。
`--cases bwtree clevelhash+rpc` 可选择失败项重测，默认仍为全部八项。
`--backend ub` 与 `--backend cxl` 共用 case 路由、索引实现、workload 生成、RPC
ThreadPool/MPMCQueue、结果核验和清理握手。必要差异是映射/分配器、原子和 cacheline
操作后端；两种模式不是相同二进制，CXL 测试不能替代真实 UB 硬件测试。
