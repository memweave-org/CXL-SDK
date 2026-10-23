# 全局 UB API 选择与最新 UBS Memory 接入

全局构建选项为 `UB_API=none|memlink|ubsm`；环境变量 `SHM_UB_API` 是首次
CMake 配置的默认值，也供 `run.py`、`scale.py`、`build.sh` 使用。切换后端需要重新编译；
`run.py` 为不同 API 使用独立构建目录，并在 `--skip-build` 时检查缓存，禁止静默切换。
此选项选择 SDK 和访问策略，不会把旧 YCSB 配置中的 `mem_type=local` 自动改成 UB。
双机 UBSM 实验使用本目录的执行入口。

| 选择 | 映射 API | 对象生命周期 |
| --- | --- | --- |
| `none` | 普通文件/设备 mmap | 普通文件由 runner 独占创建；设备由用户提供 |
| `memlink` | `MemlinkMemShmMmap` | 用户预先创建/导入，runner 不销毁对象 |
| `ubsm` | `ubsmem_shmem_map` | runner 在 A 创建，A/B 映射，双方成功退出后 A 释放 |

`WITH_UB=ON` 只保留为旧 Memlink 构建兼容入口；不能与 `UB_API=ubsm` 混用。

## 配置

最新 UBSM（两端头文件、动态库必须为匹配版本）：

```sh
export SHM_UB_API=ubsm
export UBSM_INCLUDE_DIR=/usr/local/ubs_mem/include
export UBSM_LIBRARY=/usr/local/ubs_mem/lib/libubsm_sdk.so
export LD_LIBRARY_PATH="/usr/local/ubs_mem/lib:${LD_LIBRARY_PATH:-}"
export HCOM_CONNECTION_RETRY_TIMES=2
```

旧 Memlink：

```sh
export SHM_UB_API=memlink
export UB_MEMLINK_LIBRARY=/实际路径/libmemlink.so
```

普通文件验证：`export SHM_UB_API=none`。命令行 `--ub-api` 优先于环境变量。
直接构建任意使用 shm-lib 的项目也可传 `-DUB_API=ubsm`、`-DUBSM_INCLUDE_DIR=...`、
`-DUBSM_LIBRARY=...`；SDK 链接和相关编译宏经 shm-lib 传给消费目标。

## Docker 编译

### 单机先试 BwTree OPT 和 ClevelHash OPT

在具备厂商 SDK、UB 服务及访问权限的环境中，从仓库根目录执行：

```sh
./tests/two-process-ycsb/run_ub_single.sh 内存提供机器的UB拓扑主机名 2 4
```

脚本自动编译并依次运行这两个 OPT case；每个 case 有两个本地进程，以上线程数为
2 和 4（不填默认各 2）。默认加载 10000 条记录、每进程执行 10000 次 YCSB-B 操作，
分配 1024 MiB，使用 16-bit BwTree 映射表，适合小规模功能检查。成功后自动释放 UB
对象；失败时保留对象并提示检查日志。输出位于 `build/ub-single-*`。
这里“单机”只指两个 worker 在同一台机器运行，不限制内存所在机器。
例如在 A 运行脚本、参数填 B 的 UB 拓扑主机名：管理程序在 A 调用
`ubsmem_shmem_allocate_with_provider`，`src_loc.host_name=B`；内存由 B 提供，
A 的两个进程映射同名对象并访问 B 的内存。B 需要 UB 服务及可用资源，但不需要
启动 benchmark worker 或 SSH 登录。`UBSM_PROVIDER_NUMA` 指 B 的内存 NUMA。
两台机器上的进程各运行一个是另一种实验，需要使用下文的 `--host`。

### Cacheable 数据与 NC 原子区域

单机入口现在默认使用混合布局：数据对象以 `UBSM_FLAG_CACHE` 创建，另一个
`名字-nc` 对象以 `UBSM_FLAG_NONCACHE | UBSM_FLAG_WR_DELAY_COMP` 创建。
两端都映射这两个对象；数据可在两端缓存且依赖软件显式缓存维护，原子目标不缓存。
这与之前的 ONLY_IMPORT_NONCACHE 布局不同。

默认数据区 1024 MiB（`SIZE_MB`），NC 区 512 MiB（`ATOMIC_SIZE_MB`）。每个对象
划分两个进程各自的分配区。BwTree 节点、ClevelHash KV 和 value 在数据区；共享
索引头、映射表、桶槽、同步变量、复制元数据，以及节点内拆出的原子字段在 NC 区。
详细放置、ownership、验证边界见 [UB_SPLIT.md](UB_SPLIT.md)。

CACHE 模式仍需遵守厂商的 BIOS/ownership 条件（API 要求 BIOS 不配置 snoop）。
每个进程 map 后申请本地 RW ownership，所有 worker 和扩容线程停止访问后释放
ownership 再 unmap。实际 SDK 是否允许目标部署中各进程同时取得 RW 映射，以及
跨机缓存维护是否有效，必须在真实 UB 上验证；替身不能证明这些硬件语义。

UBSM 构建不模拟 CAS 延迟。其他后端和 `--ub-layout legacy` 保留原布局。
`run.py` 默认 `--ub-layout auto`：Sherman、RPC 和 BwTree/ClevelHash OPT/NOOPT 使用混合布局；
CC case 继续使用旧路径。可显式 `--ub-layout split`，不支持的 case
会报错，不会偷偷改成 legacy。单机脚本显式指定 split。

也可以直接指定镜像，脚本自动创建临时容器完成编译并删除容器，然后在宿主机运行大测试：

```sh
UB_IMAGE=shm-pcc-sdk:ub-glibc2.39-gcc12.3.1 \
./tests/two-process-ycsb/run_ub_single.sh 内存提供机器的UB拓扑主机名 2 4
```

大测试的镜像入口只挂载仓库、SDK 和编译依赖，使用宿主机 UID/GID 与附加组；
不需要 UB 设备或服务 socket。编译完成后，`ub_admin` 和两个 worker 全部在
宿主机运行，使用宿主机用户和 SDK 环境，不会在容器中初始化 UB。
宿主机需要安装 SDK、启动 UB 服务并具备相应设备和服务访问权限。

脚本自动解析宿主机 `libubsm_sdk.so` 的动态依赖链，将 SDK 及配套依赖复制到
`build/ub-runtime/run-*/lib` 供容器链接，清单保存在 `manifest.json`。
镜像使用自己的 glibc、动态加载器、libstdc++ 和 libgcc。
宿主机必须能运行该镜像编译的产物；例如宿主机 glibc 太旧，使用 glibc 2.39
镜像编译并不能解决宿主机运行兼容性。脚本在创建任何 UB 对象前，对两组
`bench` 和 `ub_admin` 执行宿主机 `ldd -r`，缺库、符号缺失或版本不兼容时
立即停止。应选择与宿主机兼容的镜像或补齐宿主机依赖。

运行日志位于 `OUTPUT`（默认 `build/ub-single-*`），编译日志位于其同级
`OUTPUT-build` 目录；两者必须不存在。`UB_EXTRA_LIBRARY_DIRS` 同时用于
宿主机依赖搜索。不要把容器的 glibc 或动态加载器复制到宿主机覆盖系统库。

独立 `run_sdk_smoke.sh` 的 `UB_IMAGE` 模式仍在容器内运行，用于诊断容器 SDK
环境；若要与大测试的宿主机运行环境对照，应不设置 `UB_IMAGE`、`UB_CONTAINER`。

SDK 默认路径为 `/usr/local/ubs_mem`，同时支持 `lib`、`lib64`，以及只有一个实际
版本库文件、没有无版本 `.so` 链接的情况。CMake 使用完整文件路径链接，并配置
SDK 与依赖目录的链接时和运行时搜索路径。需要额外库搜索目录时，可设置：

```sh
UB_EXTRA_LIBRARY_DIRS=/实际HCOM目录/lib:/实际安全C库目录/lib \
UB_IMAGE=shm-pcc-sdk:ub-glibc2.39-gcc12.3.1 \
./tests/two-process-ycsb/run_ub_single.sh 内存提供机器的UB拓扑主机名 2 4
```

通过 `dlopen` 加载、没有声明在 ELF 依赖中的插件不能自动推断，可用
`UB_EXTRA_LIBRARIES=/完整插件路径.so:/另一个插件.so` 显式收集其依赖。
这些选项不强行预加载库，不会掩盖 SDK 未正确链接依赖或版本不匹配的问题。
宿主机无法解析的依赖会在创建容器前报错。

如果现场服务 socket 位于其他目录，或 SDK 还有额外依赖，可设置
`UB_DOCKER_ARGS_FILE=/绝对路径/docker-args.txt`；文件每行是一个完整 Docker 参数，
不加 shell 引号、不做变量展开，例如：

```text
--volume
/实际服务目录:/实际服务目录:ro
```

日志保存在宿主机仓库 `build/ub-single-*`，UB 对象失败保留策略不变。
`UB_IMAGE` 与 `UB_CONTAINER` 不能同时设置；不指定两者则直接在当前环境运行。

若已启动具备 UB 设备、服务 socket、组权限和 SDK 依赖的容器 `ub-ycsb`，且仓库
挂载在相同绝对路径，宿主机一条命令即可在容器里完成编译和运行：

```sh
UB_CONTAINER=ub-ycsb ./tests/two-process-ycsb/run_ub_single.sh 内存提供机器的UB拓扑主机名 2 4
```

已有容器应使用前述 GCC 12.3.1/glibc 2.39 镜像；`UB_CONTAINER` 入口复用其 SDK 环境。可用 `UBSM_PROVIDER_NUMA=0` 指定实际内存 NUMA，
`RECORDS`、`OPS`、`SIZE_MB`、`JOBS`、`TIMEOUT`、`OUTPUT` 覆盖默认参数。
`--help` 查看说明。必须提供真实 SDK 路径；不会自动退回普通文件或测试替身。

### 双机实验的构建

在机器 A 的仓库根目录执行，仓库在两机应使用相同绝对路径。下面要求厂商 SDK 安装
于 `/usr/local/ubs_mem`，其传递依赖也已经在镜像中可用；若出现缺库，先根据 SDK 的
`ldd` 输出补齐对应厂商库，不要把宿主机整个 `/usr/lib64` 覆盖到容器中。

```sh
docker run --rm --user "$(id -u):$(id -g)" \
  -v "$PWD:$PWD" -w "$PWD" \
  -v /usr/local/ubs_mem:/usr/local/ubs_mem:ro \
  -e SHM_UB_API -e UBSM_INCLUDE_DIR -e UBSM_LIBRARY \
  -e LD_LIBRARY_PATH -e HCOM_CONNECTION_RETRY_TIMES \
  shm-pcc-sdk:ub-glibc2.39-gcc12.3.1 \
  python3 tests/two-process-ycsb/run.py --build-only \
  --mapping-bits 16 --output "build/ubsm-build-$(date +%Y%m%d-%H%M%S)"
```

八种配置均不包含 Deft；UBSM 构建禁止 128 位 CAS。`--build-only` 不分配 UB 对象，
无需传 provider 或名字。将相同代码、`build/two-ycsb/*-ubsm` 和 workload 文件部署到 B
的相同路径。两机需兼容 CPU/运行库；默认 `-march=native`，不能任意跨 CPU 使用。
runner 会核对两端 worker 二进制与 workload SHA256，相同源码分别构建不保证哈希相同。

旧 YCSB 的 Docker 构建脚本也会转发上述 API 环境变量；`UB_SDK_ROOT` 可指定额外
只读 SDK 挂载目录。但旧入口的配置和跨机启动方式不由这个开关自动改变。

## 双机运行

先通过附件的 UBSM 双向读写、owner/remote 竞争 8-byte CAS/FAA 测试。
需要 ubse/ubsmd 正常、两端访问权限和 UID/GID 匹配，以及符合附件 flag 的硬件 snoop 配置。

仅在 A 启动协调脚本；B 通过 SSH 启动一个 worker。请替换实际 hostname、NUMA 和 SSH 地址：

```sh
export UBSM_PROVIDER_HOST=实际UB拓扑中的A主机名
export UBSM_PROVIDER_NUMA=0   # 必须是实际有内存且可提供 UB 资源的节点

python3 tests/two-process-ycsb/run.py \
  --memory ub --ub-api ubsm --host user@machine-b \
  --shared-path ycsb-ub --skip-build \
  --threads 2 4 --records 10000 --ops 10000 \
  --size-mb 1024 --mapping-bits 16 \
  --output "build/ubsm-run-$(date +%Y%m%d-%H%M%S)"
```

`--shared-path` 在 UBSM 模式是名称前缀，不是已有对象名。每个 case 会追加随机后缀，
准确名字实时打印并写入 results.json。A 是 provider，B 不分配第二个对象。两端映射
相同物理对象到 `0x400000000000`；混合布局另有固定地址 `0x500000000000` 的 NC 对象，其中前 8 MiB 是控制区。
两个对象均由指定 provider 提供，不是对称 NUMA 实验；混合布局使用按进程划分的
只增分配区，legacy 使用原 memkind 分区。

宿主机运行必须具备与 Docker 产物兼容的 glibc/依赖。若需要也在 Docker 内运行，
在两机预先启动同名且具备厂商规定设备、UDS、组权限和 SDK 依赖的容器，例如 `ub-ycsb`，
并挂载相同绝对路径的仓库，然后追加：

```sh
--worker-prefix 'docker exec ub-ycsb'
```

此参数同样用于 A 的对象管理程序及两端 worker。远端容器内应设置 LD_LIBRARY_PATH、
HCOM_CONNECTION_RETRY_TIMES；普通 SSH 不保证传递本地环境变量。超时命令在容器内部
执行。脚本不猜测 UBSM 服务 socket/设备路径，也不自动授予 privileged 权限。

## 实现和退出约定

- 最新 API 路径：init_attributes → initialize → allocate_with_provider（仅管理进程）/
  map（两个 worker）→ unmap → finalize；双方成功且确认 unmap 后，管理进程 deallocate。
- map 请求整个对象、offset=0、MAP_SHARED | MAP_FIXED_NOREPLACE；返回错误地址直接失败。
- 混合布局：CACHE 数据 + NONCACHE/WR_DELAY_COMP 原子；数据区执行缓存维护，NC 区跳过。
  legacy 布局继续 ONLY_IMPORT_NONCACHE/WR_DELAY_COMP。两种布局都关闭模拟延迟，
  保留屏障并禁止 128 位 CAS/快照。
- ClevelHash 创建方先停止并 join 后台扩容线程，再 unmap。测试保持进程生命周期分配策略，
  不启用跨进程 memkind free/BwTree 后台 GC；这不是完整论文性能复现。
- 任一创建、worker、映射、解除映射或 finalize 失败，均不自动释放/复用对象。
  若容器或 SSH 被中断，应先确认两端 worker 全部停止，再用厂商工具按精确名字检查清理。
  也可在确认无人使用后，使用对应构建的 `ub_admin destroy 精确对象名`。

## 验证边界

当前没有厂商 SDK 和真实双节点 UB；回归通过明确标记 `UBSM_TEST_STUB` 的测试替身
验证接口调用、生命周期及双进程索引操作，不能证明 SDK ABI、NC 映射上的 memkind/
硬件原子支持或真实跨节点一致性。生产构建必须指定厂商头文件和动态库，不能使用
`tests/ub-api/mock`。结果会标记 `ubsm_test_stub=true`，绘图脚本拒绝把替身结果当作性能数据。
