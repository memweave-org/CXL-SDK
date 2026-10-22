# UBSM 最小初始化、映射及读写测试

`sdk_smoke.cc` 是独立 C++ 程序，只调用厂商 SDK，不依赖 shm-lib、memkind、YCSB、
索引或模拟延迟。每个 SDK 调用都打印返回码；任何一步失败立即进入清理并非零退出。

## 在真实 UB 机器运行

从仓库根目录，复用编译镜像自动创建容器及 SDK 依赖收集：

```sh
export UB_IMAGE=shm-pcc-sdk:ub-glibc2.39-gcc12.3.1

# 先只初始化，不创建任何对象，不需要 provider 参数。
./tests/ub-api/run_sdk_smoke.sh init

# 当前机器运行，另一台机器提供内存；默认 128 MiB。
./tests/ub-api/run_sdk_smoke.sh noncache 另一台机器的UB拓扑主机名

# 在满足 CACHE 模式的 BIOS/服务配置后，单独测试 CACHE。
./tests/ub-api/run_sdk_smoke.sh cache 另一台机器的UB拓扑主机名
```

两个内存模式共用完全相同的初始化：logger(debug) → init_attributes → initialize。
模式只影响之后 allocate 的 flags。若 `init` 就返回 6015，切换 cache/noncache
不会绕过该错误。

| 模式 | 创建 flags | 后续步骤 |
| --- | --- | --- |
| init | 不分配 | initialize → finalize |
| noncache | NONCACHE \| WR_DELAY_COMP | allocate → map → 写入/读回 → unmap → deallocate → finalize |
| cache | CACHE | allocate → map → ownership(RW) → 写入 → ownership(READ) → 读回 → ownership(NONE) → unmap → deallocate → finalize |

按所给 API，CACHE 模式要求配合 ownership，且 BIOS 不能配置 snoop；NONCACHE
模式配合 WR_DELAY_COMP，不能调用 ownership。本测试不改变 BIOS、驱动或服务配置，
不要假设同一现场配置必然能同时通过两种模式。这里的 noncache 是纯 NC，并非之前
YCSB 使用的 ONLY_IMPORT_NONCACHE（提供方 cacheable、导入方 NC）。

每次只映射一个完整对象，用普通 64-bit 标量写入 4096 字节后读回比对，不测试 CAS。
ARM 使用 `dmb sy` 屏障。使用同一个进程进行读写，不能证明两个主机的缓存一致性。
内存 NUMA/socket/port 使用 UINT32_MAX，由 SDK 选择；第三个参数可改变 MiB 大小
（至少 4，4 的倍数）。Cache 和 NC 默认都只使用一个进程。

日志与独立二进制放在 `build/ub-single-*/{build.log,run.log,ub_sdk_smoke}`。
也可以设置 `OUTPUT=build/my-sdk-test`。出错时会尝试解除映射和释放对象；若解除映射
失败，保留对象并打印 `OBJECT_MAY_REMAIN`，不能在仍有映射时强行销毁。

`UB_IMAGE`、`UB_CONTAINER`、`UB_SDK_ROOT`、`UBSM_INCLUDE_DIR`、`UBSM_LIBRARY`、
`UB_EXTRA_LIBRARY_DIRS`、`UB_EXTRA_LIBRARIES` 与单机 YCSB 入口相同。
使用现有容器时，其 SDK/设备/socket 需要预先配置；不设镜像或容器则在当前环境运行。
脚本只复用 Docker/SDK 准备部分，不构建或运行任何 YCSB case。

## 手工编译（已配好 SDK 开发环境）

```sh
c++ -std=c++17 -O0 -g -I/usr/local/ubs_mem/include \
  tests/ub-api/sdk_smoke.cc /usr/local/ubs_mem/lib/libubsm_sdk.so \
  -Wl,-rpath,/usr/local/ubs_mem/lib -o /tmp/ub_sdk_smoke

/tmp/ub_sdk_smoke init
/tmp/ub_sdk_smoke noncache 另一台机器的UB拓扑主机名 128
/tmp/ub_sdk_smoke cache 另一台机器的UB拓扑主机名 128
```

## 本地验证范围

使用明确打印 `UBSM_TEST_STUB` 的文件映射替身验证三个模式、写入读回、ownership
调用和成功清理；替身的 flag 数值不是厂商 ABI。不代表已经通过真实 UB 的
初始化、缓存属性或 ownership 硬件测试。
