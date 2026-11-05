# 原 YCSB-C 框架的双机 UB 模式

## 八项顺序对比：run_all

两机拉取同一版本并配置 `ub/config.sh`，在 B 先执行、A 随后执行：

```sh
cd /mnt/disk1/wfn/CXL-SDK/tests/YCSB-C
bash run_all.sh --dry-run
bash run_all.sh
```

| case | 编译模式 | A 的读 / 更新 | B 的读 / 更新 | 节点内存 |
| --- | --- | --- | --- | --- |
| bwtree | NOOPT | 直接读 / NC RPC | 本地索引 | CACHE |
| bwtree+opt | OPT | 直接读 / NC RPC | 本地索引 | CACHE |
| bwtree+rpc | NOOPT | 全部 NC RPC | 本地索引 | CACHE |
| clevelhash | NOOPT | 直接读 / NC RPC | 本地索引 | CACHE |
| clevelhash+opt | OPT | 直接读 / NC RPC | 本地索引 | CACHE |
| clevelhash+rpc | NOOPT | 全部 NC RPC | 本地索引 | CACHE |
| sherman+nc-direct | NOOPT | 原地 load/store | 原地 load/store | NC |
| sherman+nc-memcpy | NOOPT | memcpy 到本地，修改后 memcpy 回去 | 同左 | NC |

两端均为 90% read / 10% update、Zipfian；update 替换已有 key 的 value。
每项新建共享对象，使用相同初始记录数、每端操作数、线程数和 `SEED`，并核对
各端操作类型/key 序列 hash 在八项间一致。比例由随机抽样决定，小样本不保证恰好 90/10。
默认初始 100,000 条，每端 1 线程、1,000,000 次操作（合计 2,000,000）。
首次可在两端用相同参数执行：

```sh
RECORDS=1000 OPS=1000 bash run_all.sh
```

BwTree/ClevelHash 继续使用已有启动 CACHE 失效及发布修复，默认 A CACHE 只读、B 可写。
普通模式仅 A 的更新交给 B；RPC 模式 A 的全部操作交给 B。RPC 使用 NC mailbox，
每个 client 一个同步请求，B 一个服务线程；默认服务线程和 B client 共用 `CPU_B`。
`A_WRITABLE=1` 可显式允许普通 BwTree/ClevelHash 的 A 直接更新 CACHE。

Sherman 两项都只 mmap 一块 NC 对象，节点、value、根、锁和控制信息全部在 NC，
不创建 CACHE 映射。A/B 分配区互不重叠，双方对称执行读写，RPC 完成数必须为 0。
`nc-direct` 直接遍历 NC 节点字段，在锁和版本保护下原地修改、分裂节点；
`nc-memcpy` 将完整节点复制到本地做查询或修改，然后复制修改部分回 NC。
value 也分别使用直接访问或本地复制。两项都关闭本地内部节点缓存，保持相同节点布局；
每个 1024 字节节点占一个 4 KiB 分配槽并带独立 NC 版本头，避免修改/分裂时版本锁别名。
`NC_SIZE_MB` 包含节点、value 和原框架的 trace/控制数据，默认 1024 MiB，耗尽即报错。
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

结果位于 `ub/result-all-A-*` / `ub/result-all-B-*`：每项一个子目录，包含 worker/control 日志、
配置和 `result.json`；`summary.json` / `summary.csv` 汇总八项。失败立即停止后续项，
失败目录写入 `invalid.json`，失败样本不计入成功汇总。
`exclusivity.jsonl` 记录驱动 PID、CPU、时间、退出情况；启动前连续确认空闲，运行中
发现其他测试则只停止自身进程组并标记样本无效。识别器无法覆盖任意命名的调度程序，
运行者仍须确认两台机器的其他整轮测试已经结束；受干扰项须待资源释放后重跑。

`test_run_all_control.py` 只检查脚本选项和计划，不运行索引或计时。
`test_run_all.py --bundles /absolute/path/fixture-bundles` 用文件映射 SDK 替身执行八项
真实双进程接线检查（35 条记录、每端 128 次操作），结果不能用于 UB 性能或缓存一致性结论。
可选 CMake 参数 `YCSB_BUILD_SHERMAN_CONTRACT=ON` 构建 `sherman_nc_contract`，
仅供 fixture SDK 下验证双进程对称插入、分裂、更新和读取，不输出吞吐或耗时。

## 旧单项 BwTree 入口

两机同步完整源码，在各自物理机执行一次：

```sh
cd /mnt/disk1/wfn/CXL-SDK/tests/YCSB-C
bash ./run.sh
```

默认 OPT；测试 NOOPT 时，两边都用 `OPT=0 bash ./run.sh`。
也可使用原入口 `bash ./run_shm_ds.sh -mode=ub_two_host`。
旧目录 `tests/ub-bwtree-ycsb/run.sh` 现在也转到此入口。
配置统一在 **`tests/YCSB-C/ub/config.sh`**，旧目录的 config.sh 不再生效。

- A：`76.53.137.81`，100% read，原框架 follower（machine 1）。
- B：`76.53.137.82`，90% read / 10% update，原框架 master（machine 0）。
- B 先用原 `LoadManager` 加载，再共享同一个 `BwTreeDB`。
- 两边运行原 `TransactionThreadController → Client → BwTreeDB`。
- 使用原 `CoreWorkload`、`OpGeneratorIntKeyAddr` 和 Zipfian 分布；只调整各主机的操作比例与分组。
- 保留原 `UpdateInternal` 的随机新 key 插入语义，没有改成覆盖旧 key。
- 默认各 1 线程、各 1,000,000 次操作、100,000 条初始数据；支持 A/B 不同线程数。

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

默认 CACHE 4096 MiB、NC 1024 MiB；NC 包含原框架的共享 trace 数组，
调大操作数或线程数时可能需要增加 `NC_SIZE_MB`。耗尽即报错，不回退到私有堆。
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
运行脚本不扫描其他任务；实验互斥由运行者确认。

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
