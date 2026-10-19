# 两进程 YCSB-B（当前仓库移植版）

入口为 `python3 tests/two-process-ycsb/run.py`，从仓库根目录执行。
Deft 已从测试矩阵移除：其 128 位 CAS 依赖不受 UB 支持；命令行与 CMake 均拒绝选择 Deft。
所有本入口的目标都定义 `SHM_DISABLE_CAS128`，若意外进入 128 位原子路径会明确报错。
默认依次运行 Sherman、RPC、BwTree OPT OFF/ON、ClevelHash OPT OFF/ON，以及两个索引的 CC 对照。
每项都创建两个独立的 OS 进程，映射同一个索引，而不是启动两个独立数据库。

## 本地实际运行

```sh
python3 tests/two-process-ycsb/run.py \
  --threads 2 --records 10000 --ops 10000 \
  --size-mb 1024 --mapping-bits 16 \
  --output build/ycsb-two-process-smoke
```

- `--threads N`：每个进程 N 个工作线程；`--threads N0 N1`：分别指定。
- `--ops`：每进程的测量操作数。RPC 为进程 0 提供服务、进程 1 发请求，因此只计算客户端的 `--ops`，不能与直访模式的两倍总操作数混淆。
- `--records`：整个共享索引的初始键数，不是每进程各加载一份。
- `--cases sherman rpc bwtree-off bwtree-on clevelhash-off clevelhash-on` 可选子集。
- `--size-mb`：整个共享区域大小，两进程各占独立分配分区，另留 8 MiB 控制空间。
- `--mapping-bits 16` 仅用于小规模启动验证；默认 24 是原 BwTree 表大小，大规模运行应增大共享区。内存不足报错，不会回退到私有堆。
- 输出目录必须是新目录。保留每个进程的日志、PID、完成数、错误数、验证数、耗时、二进制 SHA256 和退出码；任一进程失败、超时、结果缺失或验证失败都会使脚本失败。
- 本地共享文件由脚本独占创建，结束后只删除本次创建的文件。不会清零或删除用户提供的整块 CXL/UB 区域；运行时会初始化并覆盖该区域中的控制信息和数据，所以目标区域必须专用于本次实验，不可同时运行另一组测试。

## 两台机器

在两个同架构、兼容 CPU/依赖环境的主机上，准备同路径的仓库、相同二进制、相同 workload 文件。
脚本会检查两端二进制 SHA256。SSH 需免交互认证。普通文件、NFS 文件不能作为双机共享内存替代品。

```sh
python3 tests/two-process-ycsb/run.py \
  --host user@host2 --memory cxl --shared-path /dev/dax0.0 \
  --threads 24 24 --records 1000000 --ops 1000000 \
  --size-mb 65536 --output build/ycsb-two-hosts
```

两端的 `--shared-path` 必须指向同一块真实共享内存，容量至少为 `--size-mb`。
当前支持两端使用同一设备路径字符串；本地执行端是进程 0，SSH 端是进程 1。
脚本不自动配置 CXL 交换机、UB 网络或复制设备数据。远端命令带独立超时，避免本地控制端断开后无限运行。

## UB / Memlink

用户提供的 `ubs-mem-two-node.zip` 使用另一套 **UBS Memory (`ubsmem_*`)** API。
当前 `--memory ub` 仅表示下面的 Memlink 后端，不能直接链接 `libubsm_sdk.so`。
双机分配流程、接入缺口和附件通路验证命令见 [UBSM 接入分析](UBSM.md)。

检查时 GitHub `origin` HEAD 与本地 HEAD 均为 `72757a92ca3847e11bf10abd5cd1c961b7e01034`。
仓库原已有 `shm-lib/shm/matrix_wrapper.cc`、Memlink 头文件和
`tests/YCSB-C/init_ub_env.sh`，但原 YCSB 初始化只接受 `mem_type=local/cxl`，
原 CMake 没有 UB 选项，也没有链接 Memlink。当时 Sherman 移植代码另存在于本地工作树中，本次已纳入仓库。

新增运行入口使用：

```sh
python3 tests/two-process-ycsb/run.py \
  --host user@host2 --memory ub --shared-path pcc-shm-250 \
  --ub-library /absolute/path/to/vendor/libmemlink.so \
  --threads 24 24 --records 1000000 --ops 1000000 \
  --size-mb 65536 --output build/ycsb-ub
```

`--memory ub` 自动设置 `-DWITH_UB=ON`，必须同时提供真实的厂商库路径。
两端需事先通过厂商工具创建/导入同名、同一物理 backing 的共享对象，并配置好访问权限。
仓库原有创建命令示例为 `memlink mem_create_shm_test pcc-shm-250 65536 0`；
远端导入及部署按实际厂商环境配置。这里不会猜测厂商库文件名或静默替换成文件映射。

具体路径是 **MemlinkMemShmMmap → UB 共享映射 → 每进程 memkind 固定分区 → cacheable 分配接口**。
这保证该入口的共享分配落在 UB 返回的区域，但不是把每次小对象分配改为
`MemlinkMemMalloc`，也不是启用现有 `cxlalloc` Rust 分配器。
要求映射精确返回 `0x400000000000`；失败会退出，绝不回退到普通文件或私有内存。
此开关接入本目录新入口，不会自动改写旧 `tests/YCSB-C/config.ini` 的运行路径。

当前移植版支持 x86-64 和 AArch64；ARM 普通共享映射已通过两进程测试。平台实现与验证范围见 [ARM 支持](../platform/README.md)。ARM 非临时拷贝采用普通拷贝加缓存维护，不能视为与 x86 streaming 指令性能相同。
因此 **UB 不仅需要 flag，还需要厂商运行库、正确的共享对象、可用硬件及兼容的指令/原子语义**。
本地验证不证明真实 UB 上的原子性、缓存一致性或性能。

## 工作负载和结果边界

复用 `tests/YCSB-C/core/core_workload.cc` 和 `workloadb_zipfian.spec` 的 95% 读 / 5% update、Zipfian 键生成器；键整体加 1。值为共享区中的 8 字节独立 payload。
主进程加载一次，两个进程准备完毕后同步开始。测量结束后，两边验证全部初始键及对方单独插入的校验键，也检查对方工作线程记录的写入键。
校验键在计时结束后插入，不计入吞吐操作数。

默认 `--updates original` 保留原 BwTree 适配器的特殊处理：BwTree/RPC 将 update 改为 fresh insert，其余索引更新已有键。
为保证进程间不碰撞，本入口的 fresh key 用确定性不重叠区间代替原 `Random64()`，因此不是原随机 trace 的逐位复现。
`--updates fresh-insert` 可使所有系统均执行 95% 读 / 5% fresh insert，对齐当前旧单进程比较程序的语义。
每行 RESULT 标记实际 `update_semantics`。默认不同系统的 update 语义不同，不能把数值直接称为严格同义的标准 YCSB-B 排名。

OPT 宏沿用原 YCSB 组合：`OPT_GC;OPT_ROOT_READ;OPT_IN_USE_FLAG;OPT_REPLICATE_META;OPT_BATCH_FLUSH;OPT_CLEVEL_ROOT_READ`；OFF 不定义这些宏，NO_CC 和 queued atomic 模型保留。
Sherman 为当前移植版 1024 字节节点，BwTree 为当前代码的可变长节点。
RPC 复用 SDK `MPMCQueue<uint32_t>` 的共享请求队列，以及当前 RPC 比较程序的独立请求数据/响应序号协议；服务端操作同一 BwTree，客户端每线程最多一个在途请求。进程本地缓存、scratch buffer 和线程上下文留在各进程内。

**这是新的双进程执行/验证入口，不是原论文可直接用于性能结论的完整复现。**
为了避免 memkind 跨进程释放和错误回收，共享对象使用进程生命周期策略，不启动 BwTree 后台回收器；
`cacheable.free` 在该入口中延迟至进程退出/下次重新初始化。
因此 OPT GC 的吞吐收益尚不能通过本入口评估，大规模写入需准备足够内存。
原旧入口默认分配/释放行为不改变。这里未添加 CPU 绑核、三次重复、论文六进程等实验控制；小规模通过不等于完整线性一致性证明。

## 本地验证 UB 接线（测试替身，不是真 UB）

`memlink_mock.cc` 仅供接口回归验证，明确打印 `MEMLINK_TEST_STUB`，不要用于性能结果。

```sh
c++ -shared -fPIC tests/two-process-ycsb/memlink_mock.cc -o /tmp/libmemlink_test_stub.so
truncate -s 1024M /tmp/ycsb-memlink-test.arena
python3 tests/two-process-ycsb/run.py --cases bwtree-on \
  --memory ub --shared-path /tmp/ycsb-memlink-test.arena \
  --ub-library /tmp/libmemlink_test_stub.so \
  --threads 1 3 --records 1000 --ops 2000 --size-mb 1024 --mapping-bits 16 \
  --output build/ycsb-memlink-stub-test
```

## 一键线程扩展曲线（本地模拟）

```sh
python3 tests/two-process-ycsb/scale.py \
  --threads 24 48 72 96 120 144 168 192 --repeats 3 \
  --records 100000 --ops 200000 --size-mb 1024 --mapping-bits 16 \
  --output build/two-ycsb-scaling
```

默认只编译和运行 9 个组合：Sherman、RPC（BwTree），以及
BwTree / ClevelHash 各自的 OPT、NOOPT、CC。`on` 对应 OPT，`off` 对应
NOOPT；`bwtree-cc`、`clevelhash-cc` 使用硬件一致性，不定义 NO_CC、
QUEUE_SIM_CAS 或 OPT 宏。每个组合独立构建，后续线程数/重复次数复用二进制。
旧的 `run.py --threads N0 N1` 仍表示单次运行的两进程线程配置；
新 `scale.py --threads 24 48 72 96 120 144 168 192` 表示扫描序列，每个进程使用相同线程数。
默认每进程递增 24，最大每进程 192（两进程合计 384）；
递增 48 可用 `--threads 48 96 144 192`。Sherman scratch 容量按线程数配置，
包含主线程，避免原 2 GiB 缓冲区在 192 线程时越界。

输出 `ycsb-thread-scaling.png` / `.svg`、逐次 `measurements.csv`、
中位数及范围 `summary.csv`，以及每次运行的原始日志、验证结果和二进制摘要。
任何运行失败立即停止，不为失败点填入推测数据。`--skip-build` 要求已有构建
配置完全匹配；通常首次运行不指定它。

横轴是**每个进程线程数**：直访为 N+N 工作线程，RPC 为 N 服务线程 + N 客户端。
直访吞吐 = 两进程完成数之和 / 两进程较长测量时间；RPC 吞吐仅计算客户端
完成数 / 客户端测量时间。曲线为重复测试中位数，阴影为最小/最大值。
使用同一 YCSB-B Zipfian 生成器，并统一 `fresh-insert`，因此准确名称是
**YCSB-B 派生负载（95% 读 / 5% 新键插入）**，不是标准原地更新 YCSB-B。
这是本地共享文件模拟，不等于真实非一致性硬件；未绑核、无独立预热，短运行
可能受调度和线程同步成本影响，不能用于论文性能结论。GC 仍按进程生命周期处理。

旧 `tests/YCSB-C/build.sh` 也默认仅编译 BwTree、ClevelHash，默认模式为
`opt noopt cc`，可指定 `rpc`。需要额外索引时直接配置 CMake
`-DBUILD_EXTRA_INDEXES=ON`。Sherman 和这张双进程图统一使用本目录入口。


双机扫描可使用相同入口（两端必须先准备完全相同的二进制和真正共享设备）：

```sh
python3 tests/two-process-ycsb/scale.py \
  --host user@host2 --memory cxl --shared-path /dev/dax0.0 \
  --threads 24 48 72 96 120 144 168 192 --repeats 3 \
  --records 1000000 --ops 10000000 --size-mb 65536 --mapping-bits 24 \
  --output build/two-host-ycsb-scaling
```

`--memory ub` 还需 `--ub-library`，与 `run.py` 相同。RPC 的 192+192 是
192 个服务线程和 192 个客户端线程，吞吐只计客户端，不能当成 384 个客户端。
单台 192 逻辑 CPU 上模拟 384 个工作线程存在超额调度，不能代表两台机器扩展性。
