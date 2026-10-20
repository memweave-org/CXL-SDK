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

## ARM Docker 中运行

在 arm-0 的仓库根目录执行（镜像已预先构建或导入）：

```sh
docker run --rm --user "$(id -u):$(id -g)" \
  -v "$PWD:/src" -w /src shm-pcc-sdk:ub-glibc2.39-gcc12.3.1 \
  python3 tests/two-process-ycsb/run.py --threads 1 2 \
  --records 1000 --ops 2000 --size-mb 1024 --mapping-bits 16 \
  --output build/ycsb-arm-check
```

首次运行会编译八种配置；每次选择新的输出目录。该命令使用同机共享文件，验证 ARM 上的两个进程。

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

## UB：全局切换 Memlink / 最新 UBSM

`SHM_UB_API=none|memlink|ubsm`（CMake 为 `UB_API`）统一选择后端。
新接口已接入 `ubsmem_*`；完整 Docker 编译、双机启动和对象生命周期见 [UBSM 指南](UBSM.md)。
`--memory ub --ub-api memlink` 使用旧 Memlink 和用户预先创建的对象；
`--memory ub --ub-api ubsm` 使用最新 SDK，由 runner 为各 case 创建独占对象。
不要混用两套 SDK 的头文件或库。未启用后端、SDK 缺失或映射失败均报错，不会回退到文件。

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

默认只编译和运行 8 个组合：Sherman、RPC（BwTree），以及
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
