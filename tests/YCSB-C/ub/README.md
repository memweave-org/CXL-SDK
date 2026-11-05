# 原 YCSB-C 框架的双机 UB 模式

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
