# 双机 UB 8-byte cache visibility micro test

## 一键入口（Docker 编译，物理机运行）

`config.sh` 已配置 A=`76.53.137.81`、B=`76.53.137.82`，按本机网卡 IP 自动识别角色。
B 的 UB 拓扑名默认采用 B 的系统 hostname；如果不同，修改 `PROVIDER_B`。
将同一份测试目录同步到两台机器。默认使用已有镜像
`shm-pcc-sdk:ub-glibc2.39-gcc12.3.1` 和 `/usr/local/ubs_mem` SDK，
如现场不同修改配置；镜像架构及运行库必须兼容物理机。

两边分别执行一次，顺序不限：

```sh
cd /mnt/disk1/wfn/CXL-SDK/tests/ub-cache-visibility
bash ./run.sh
```

脚本执行 `docker build` 编译，用 `docker create` / `docker cp` 取出二进制，
在宿主机运行；容器不启动 UB 测试。自动按本机 IP 选择 A/B，选择允许的第一个
CPU，等待对端编译结束后通过 B 的 TCP 18981 端口同步，最后两边打印判定结果。
控制端口只应对 A 开放，协议用于受信任的实验网络。等待对端最多 10 分钟。

每次运行生成 `result-A-*` 或 `result-B-*`，包含编译日志、worker 日志、
资源检查日志和 `result.json`。运行前仍需确认其他整轮实验及调度任务已结束；
脚本在两次启动检查及运行期间记录全部进程，并遇到已知测试/驱动或高 CPU
进程时停止自己的测试。通用进程名检查不能识别所有现场调度任务，不能替代
对现场整轮实验结束状态的确认。受干扰结果标记无效，释放资源后重新运行。

下面是测试原理和可选的旧 SSH 控制入口；一键入口无需 SSH 或自定义 guard。

独立使用当前 `ubsmem_*` API，不依赖 YCSB 或 shm-lib。B 创建 `UBSM_FLAG_CACHE`
对象，provider 必须指定 **B 的 UB 拓扑主机名**。B 映射 RW、A 映射 READ，分别申请
相应 ownership。两边使用同一对象名、同一 offset、同一固定 VA；地址不匹配立即失败。
虚拟地址相同本身不证明物理页相同，共享物理位置由同一 UB 对象及 offset 确定。

顺序（控制消息通过 SSH 管道，不占用被测 UB cacheline）：

1. B 写 `old=0x1122334455667788` 并 flush，等待 A 映射完成。
2. A invalidate 后普通 volatile 64-bit load，确认 old，留下缓存副本。
3. B 普通 volatile 64-bit store 写 `new=0x8877665544332211`，执行屏障但不主动 flush。
4. A 普通 load，记录 `after_write`，条件 1 要求仍是 old。
5. B flush 并等待缓存维护完成，之后 A invalidate 并重新 load；条件 2 要求 new。
6. A 解除 ownership、unmap、finalize 并退出成功后，B 才解除映射并释放对象。

目标地址必须 8-byte 对齐；默认 offset=0、base=0x300000000000、对象 128 MiB。
两边各固定一个 CPU，所有读写都只针对这个 8-byte 位置。

ARM64：B `dc cvac`，A `dc civac`，前后 `dsb sy`。A 从未写入且映射只读，
clean+invalidate 用于失效其干净缓存行；不是特权 `dc ivac`。x86-64 两边使用
`clflush` + `mfence`（写回并失效）。没有调用 UBSM 模式下可能退化为屏障的封装。
CPU 指令到 UB 设备的实际可见性仍取决于硬件和 SDK 配置，测试正是用于验证这一点。

**条件 1 是观测要求，不是架构保证。** 只读映射不保证缓存一定保留 old；一致性、
替换、抢占、控制通道延迟等可能导致提前读到 new。测试不会循环挑选成功样本。
CACHE 模式的 BIOS/snoop 和 ownership 约束沿用现场 SDK 要求；若 SDK 不允许同时
A READ / B RW ownership，程序会报告失败，不改用其他映射模式冒充测试成功。

## 编译

在 A、B 各自的 SDK 环境下，从仓库根目录运行（可以指定不同输出路径）：

```sh
UBSM_INCLUDE_DIR=/usr/local/ubs_mem/include \
UBSM_LIBRARY=/usr/local/ubs_mem/lib/libubsm_sdk.so \
  tests/ub-cache-visibility/build.sh /tmp/ub-cache-worker
```

两边必须有可用的 UB 服务、设备权限、SDK 依赖，以及 SSH 非交互运行环境。
`MAP_FIXED_NOREPLACE` 不被 SDK 支持或 VA 已占用时会失败，不使用会覆盖已有映射的
`MAP_FIXED`。CPU 编号需要在各自进程允许的 affinity 集合内。

## 运行

按照仓库测试资源互斥要求，runner 要求提供现场的 `--guard` 可执行程序。
它接收 A、B 的 SSH 地址作为两个参数，必须检查**两台机器**的其他测试进程、
驱动/调度脚本及 CPU 使用，输出机器、时间、PID、资源使用和整轮测试结束依据；
确认没有其他测试且无待启动的实验调度任务才返回 0，发现冲突或无法确认则返回非零。
不能仅根据 CPU 下降或某个 worker 退出放行。不要用 `true` 跳过检查。
由于现场的调度系统及测试命令未知，此处不提供会漏检其他任务的通用进程名白名单。

```sh
python3 tests/ub-cache-visibility/run.py \
  --host-a user@machine-a --host-b user@machine-b \
  --provider B的UB拓扑主机名 \
  --binary-a /tmp/ub-cache-worker --binary-b /tmp/ub-cache-worker \
  --cpu-a 2 --cpu-b 2 \
  --guard /absolute/path/to/site-exclusivity-check \
  --output build/ub-visibility-run1
```

runner 连续两次 preflight 检查后才启动 worker；运行中每秒再次检查，冲突后停止
自己的测试、保留日志并将样本标记为无效，不干预其他任务。guard 本身应快速返回。
互斥检测属于采样检查，现场仍须避免在检查间隙启动别的任务。
`--timeout` 默认 60 秒，适用于单个控制阶段和 guard。输出目录必须不存在。

输出 `A.log`、`B.log`、`exclusivity.log`、`config.json` 和 `result.json`。
`case1_old` / `case2_new` 分别表示两个条件；只有两者通过、清理成功且样本有效才
返回 0。真实 SDK 日志里可检查 map/ownership 返回码。失败、超时或 SSH 中断时，
对象可能保留：按日志中的唯一对象名确认两边全部退出且 unmap 后再做人工清理。
终止 SSH 不能保证卡在驱动中的远程进程已退出，runner 会标记无法确认的退出。

## 本地验证范围

可以用 `tests/ub-api/mock` 的头文件做编译语法检查；该 fixture 原有 map 契约仅接受
RW，且本地共享文件提供的是 CPU 一致性，不能用于证明本测试的真实 UB 只读映射、
并发 ownership 或 stale-cache 语义。控制流程单测使用假的 Worker，不访问 UB 或 SSH：

```sh
python3 tests/ub-cache-visibility/test_control.py
```
