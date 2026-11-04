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

复用现有 `SHM_UB_SPLIT` 和 `nt` 实现；UB 原子路径不注入模拟延迟。
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
