# 原 YCSB-C 框架的双机 UB 模式

## 八种模式 × 三档 worker：run_all

两机拉取同一版本并配置 `ub/config.sh`，在 B 先执行、A 随后执行：

```sh
cd /mnt/disk1/wfn/CXL-SDK/tests/YCSB-C
bash run_all.sh --dry-run
bash run_all.sh
```

| case | 编译模式 | A（100% read） | B（90% read / 10% update） | 节点内存 |
| --- | --- | --- | --- | --- |
| bwtree | NOOPT | 直接读 | 本地索引 | CACHE |
| bwtree+opt | OPT | 直接读 | 本地索引 | CACHE |
| bwtree+rpc | NOOPT | 共享内存队列 RPC 读 | 本地索引 | CACHE |
| clevelhash | NOOPT | 直接读 | 本地索引 | CACHE |
| clevelhash+opt | OPT | 直接读 | 本地索引 | CACHE |
| clevelhash+rpc | NOOPT | 共享内存队列 RPC 读 | 本地索引 | CACHE |
| sherman+nc-direct | NOOPT | NC 字段直接 load | NC 字段直接 load/store | NC |
| sherman+nc-memcpy | NOOPT | memcpy 到本地读取 | memcpy 到本地，修改后复制回 NC | NC |

所有 case 的 A 都只读，禁止 UPDATE/INSERT；B 使用 90% read / 10% update，
update 替换已有 key 的 value。两端均使用 Zipfian 分布，每项新建共享对象，
使用相同初始记录数、每端操作数、线程数和 `SEED`，并核对各端操作类型/key
序列 hash 在相同 worker 数的八种模式间一致。B 的比例由随机抽样决定，小样本不保证恰好 90/10。
每种模式依次测试每端 **32、64、128 个 worker**，共 24 轮，A/B 数量相同。
默认初始插入 10,000,000 个 key，每端每轮共 10,000,000 次操作（两端合计 20,000,000），由该端全部 worker 分摊。
A 执行 10,000,000 次查询；B 约 9,000,000 次查询、1,000,000 次更新，查询和更新均针对初始 key 集合。
可用 `--thread-counts 32 64` 或 `THREAD_COUNTS="32 64"` 选择档位；启动时检查
当前进程允许的 CPU 数，并按允许的编号绑定 worker，支持 CPU 编号不连续。
首次可在两端用相同参数执行：

```sh
RECORDS=1000 OPS=1000 bash run_all.sh --thread-counts 32
```

BwTree/ClevelHash 继续使用已有启动 CACHE 失效及发布修复，A CACHE 只读、B 可写。
普通模式不分配 RPC 队列、不启动服务线程。`+rpc` 仅把 A 的读请求发给 B：
复用 `shm-lib/include/msg/mpmc_queue.h` 的 `MPMCQueue<uint32_t>`（`USE_NO_CC_QUEUE`），
队列、请求和回复都位于共享 NC。队列传递 client 编号，每个 client 只保留一个
在途读请求，B 出队执行读取后通过 `response_sequence` 发布结果。
B 的自身 workload 直接访问索引；B 有一个额外的 RPC 服务线程，默认与 B client
共用 `CPU_B`。普通模式 RPC 完成数必须为 0，`+rpc` 必须等于 A 的全部读次数。
`A_WRITABLE=1` 会在启动前报错，不能开启 A 更新。

Sherman 两项都只 mmap 一块 NC 对象，节点、value、根、锁和控制信息全部在 NC，
不创建 CACHE 映射。A/B 使用相同访问机制，A 只读索引，只有 B 更新，RPC 完成数为 0。
NC 映射仍需可写以支持控制信息及 epoch 等元数据；这不允许 A 执行索引更新。
`nc-direct` 直接遍历 NC 节点字段，B 在锁和版本保护下原地修改、分裂节点；
`nc-memcpy` 将完整节点复制到本地查询，B 在本地修改后复制修改部分回 NC。
value 也分别使用直接访问或本地复制。两项都关闭本地内部节点缓存，保持相同节点布局；
每个 1024 字节节点占一个 4 KiB 分配槽并带独立 NC 版本头，避免修改/分裂时版本锁别名。
`NC_SIZE_MB` 包含节点、value 和原框架的 trace/控制数据，默认 16384 MiB，耗尽即报错。
这里的 NC 是 UB SDK 的 non-cacheable 映射，不是 cacheable 内存上的 streaming 指令。

每端构建包含三种索引的 NOOPT/OPT 两份二进制，按 case 复用。镜像需包含 CityHash、
Boost coroutine/context 开发库；`Dockerfile.openEuler` 已配置这些依赖。
构建仍在 Docker 内，worker 在物理机运行；Sherman 不需要 RDMA 或 memcached。
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

结果位于 `ub/result-all-A-*` / `ub/result-all-B-*`，每轮使用
`<case>/threads-32`、`threads-64` 或 `threads-128` 子目录，包含 worker/control 日志、
配置和 `result.json`；`summary.json` / `summary.csv` 汇总 24 轮并记录两端 worker 数。
NOOPT/OPT 各构建一次，全部轮次复用；每轮新建共享对象，清理完成后再进入下一轮。
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
验证实际操作生成器、A 更新/插入拒绝、B 更新可见性及 RPC 完成计数。
`queue` 额外用容量为 2 的原有 MPMC 队列核对重复复用和各 client 的回复。
`bwtree+legacy` 覆盖未设置 suite 的默认入口。
这不是无 CC 硬件模拟，也不输出吞吐或延迟；正式性能测试仍须等待独占资源。

## BwTree 线程数扫描与单项入口

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
| CACHE | `0x300000000000` | READ | RW | BwTree 节点、delta、value |
| NONCACHE + WR_DELAY_COMP | `0x400000000000` | RW | RW | DB/树对象、映射表、epoch、OPT 元数据、线程控制及原框架共享数组 |

使用 `SHM_UB_SPLIT` 和独立的 UB NT backend：`UB_API=ubsm`（或 Memlink）
在编译时为 `nt`、`aligned_nt`、`nt_pointer` 选择 `atomic_ub_*`，直接对对象所在的
mmap 地址执行 CPU 原子 load/store/CAS/fetch 操作。共享原子对象由 NC arena 分配；
栈上/进程内临时对象仍可使用。对象本身不保存模拟副本、queue 或计时状态。
NC 的 `flush()` 只执行内存屏障，CACHE 节点仍走原有显式 cache maintenance。
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
B 的 SDK provider 默认使用 B 系统 hostname；可在配置中填写 `PROVIDER_B`。
TCP 控制端口默认 18982。

默认 CACHE 32768 MiB、NC 16384 MiB，为千万级初始数据及每端千万次操作预留空间。
NC 分配区由 A/B 各占一半，B 预生成两端全部操作序列；仅 trace 数组就超过 1 GiB，
还需容纳索引元数据，以及 Sherman 模式的节点和 value。
可通过 `SIZE_MB` / `NC_SIZE_MB` 覆盖容量；更大负载可能仍需增加。耗尽即报错，不回退到私有堆。
保留原 epoch/GC 逻辑及 B 的后台 collector；UB bump allocator 不复用已释放地址，
整个对象在退出时释放。NodeID 的空闲栈没有入栈路径，UB 模式跳过其无用的 128-bit CAS。
所有节点由 B 分配/回收，不启动不需要的跨机 GC 消息队列 dispatcher。

## 启停与统计

Python 只协调两边已启动的进程，替代 SSH 拉起 follower，不执行数据操作。
B 发布原线程控制器，A 完成映射和线程初始化后，两边通过原 NC start/finish 变量运行。
最终吞吐使用原 `MasterTransactionManager` 的 **成功操作总数 / duration**，
`result.json` 中 `common_seconds` 为此原生计时；`controller_seconds` 单独记录 TCP 控制窗口。
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
  -L"$out" -Wl,-rpath,"$out" -lubsm_sdk -o "$out/nt_contract"
UBSM_TEST_DIRECTORY="$out/objects" "$out/nt_contract"
```

本地 worker 的 READY、RESULT、CLOSED 通过专用继承 pipe
（`YCSB_UB_CONTROL_FD`）传给控制器；stdin 只接收控制命令，stdout/stderr 只作日志。
控制器不从日志解析协议，SDK 并发打印或不换行不会吞掉 READY。
A/B 之间仍走独立 TCP 连接。worker 必须通过 run.sh/peer.py 启动。

控制器持续接收对端状态；等待本地 READY、RESULT 或 CLOSED 时也检查对端失败和断连。
任一端失败，另一端中止本轮 worker；SIGTERM 后 2 秒仍未退出则发送 SIGKILL 并等待确认。
仅操作控制器自己启动的子进程，失败结果标记 pass=false，UB 对象仍保留供诊断。


## 正常 UB 运行的缓存修复

重新编译后，`run.sh` 默认在两端 CACHE 映射取得 ownership 后、分配和读取之前，
各自执行一次整区缓存失效。它不清零共享内存，也不依赖对端 cache coherence。
日志中的 `UB_STARTUP_INVALIDATE` 可确认执行；仅显式设置
`SHM_UB_STARTUP_INVALIDATE=0` 才关闭，供故障对照使用。

`YCSB_UB_CASE=clevelhash bash ./run.sh` 选择 ClevelHash NOOPT，
`YCSB_UB_CASE=clevelhash+opt bash ./run.sh` 选择 OPT；默认仍是 BwTree，
由 `OPT=0/1` 选择变体。ClevelHash 的可变 bucket、metadata、root 和线程状态放在 NC，
不可变 KV pair 放在 CACHE，并在发布指针前完整写回；读取 pair 时执行原有缓存失效。
两端 ClevelHash 均执行 90% 读、10% 更新已有 key，A 直接读取、写请求转交 B，
默认 A 的 CACHE 映射保持只读。`clevelhash+rpc` 和 `bwtree+rpc` 可将 A 的全部操作交给 B。

默认 `run.sh` 每次从当前源码构建。若显式设置 `YCSB_UB_BUNDLE`，必须使用修复后
重新构建的 bundle；旧二进制不会因为脚本更新自动获得修复。

这些修复用于真实 SDK 构建，不启用独立内存副本模拟。启动失效不能阻止随后对未来
地址的预取；CoW 地址不复用且未提前重新缓存，是仅靠启动失效读取新对象的前提。
