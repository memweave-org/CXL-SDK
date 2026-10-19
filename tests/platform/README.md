# ARM / x86 平台验证

YCSB 默认构建 BwTree、ClevelHash。`YCSB_INDEXES` 可显式选择索引；
CLHT、HOT、Masstree、ART、BTreeOLC 尚未完成 ARM 移植，ARM 上选择它们会在配置阶段报错。
不要通过删除 flush 检测或空实现缓存指令来绕过错误。旧构建目录缓存了索引列表时，
请显式传入 `-DYCSB_INDEXES='bwtree;clevel-hash'`。

平台层区分 pause/yield、屏障、缓存维护、计时和 128 位 CAS。
AArch64 使用 LDAXP/STLXP 重试实现 128 位 CAS，包括失败比较时的成对读取验证，
要求地址 16 字节对齐；不依赖进程私有的 libatomic 软件锁。
ARM 拷贝采用普通拷贝加缓存维护，不是 x86 streaming 指令的性能等价物。
计时器返回硬件 ticks，不能跨架构直接比较为 CPU cycles 或纳秒。

从仓库根目录运行平台冒烟测试：

```sh
arch_flags=
case "$(uname -m)" in x86_64) arch_flags=-mcx16 ;; esac
c++ -std=c++17 -O2 -pthread -march=native $arch_flags \
  -Ishm-lib/include tests/platform/smoke.cc -o /tmp/cxl-platform-smoke
/tmp/cxl-platform-smoke
```

该测试创建两个进程、每进程两个线程，竞争更新同一 128 位数据，验证 40000 次累加、
双字互补不变量、比较失败保持原值，以及拷贝和屏障路径。

`smoke.cc` 的 128 位 CAS 测试仅用于普通 CPU 共享内存，不可在 UB 映射上使用。
UB 不支持该原语，因此 Deft 已退出当前测试矩阵。

两进程 YCSB-B 验证：

```sh
python3 tests/two-process-ycsb/run.py --threads 1 2 \
  --records 1000 --ops 2000 --size-mb 1024 --mapping-bits 16 \
  --output build/platform-ycsb-smoke
```

已在 x86-64 本机以及 arm-0 的 `shm-pcc-sdk:ub-glibc2.39-gcc12.3.1`
容器中原生运行通过：Deft、Sherman、RPC、BwTree/ClevelHash OPT OFF/ON 和 CC，
此前共九种配置。当前默认矩阵已移除 Deft，仅保留八种配置，且禁止 128 位 CAS。ARM GCC 12.3.1、glibc 2.39。测试使用普通文件的共享映射。
旧 YCSB 入口的 RPC 完成标志也改为原子变量，避免轮询数据竞争导致提前返回和悬空引用；
异步结果改为完成后发布，修复排队前释放结果缓冲区的问题，并取消少于八个 worker 时可能除零的队列编号公式。
旧入口的 BwTree/ClevelHash RPC 模式在 ARM 与 x86 上均完成 1000 条加载、2000 次操作。

这些测试不证明真实双机 UB 映射的缓存属性、远端原子支持或跨机一致性。
特别是 UB NC 映射上的独占指令、cache maintenance 指令是否适用，必须按厂商映射契约
单独验证。双进程入口仍采用进程生命周期回收策略，不能据此评估后台 GC 收益。
