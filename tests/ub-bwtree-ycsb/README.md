# 双机 UB BwTree YCSB

A=`76.53.137.81`：`PROT_READ`，100% read。
B=`76.53.137.82`：`PROT_READ|PROT_WRITE`，90% read / 10% update。
采用当前 YCSB-B 的 Zipfian 键分布和 BwTree 适配器语义：**update 转成 fresh insert**，
不是替换已有 key 的值。负载比例按概率生成，结果保存实际读写数。

两边同步相同代码，在物理机各执行一次，顺序不限：

```sh
cd /mnt/disk1/wfn/CXL-SDK/tests/ub-bwtree-ycsb
bash ./run.sh
```

`config.sh` 已填两台机器 IP。默认 B 的 UB 拓扑名取 B 的系统 hostname；若不同，
修改 `PROVIDER_B`。两台机器需要允许 A 连接 B 的 TCP 18982 端口。
同一配置文件中的 `THREADS_A`、`THREADS_B` 分别控制线程数，默认各 1；可指定
`CPU_A`、`CPU_B` 作为连续绑核的起点，默认是各自允许的第一个 CPU。
默认加载 100,000 条，两边各运行 1,000,000 次操作，UB 对象 4096 MiB。
改变比例请修改 `workloadb_90read.spec`，read + update 必须等于 1。

## 编译与执行

`docker build` 在 `shm-pcc-sdk:ub-glibc2.39-gcc12.3.1` 内编译。
SDK 默认 `/usr/local/ubs_mem`，支持 `lib` / `lib64`；版本化库可在配置里设置
`SDK_LIBRARY`。脚本提取二进制，检查宿主机动态库依赖，然后在**物理机**上执行。
容器不执行 UBSM 初始化或 workload，不需要挂设备。
两边校验二进制及 workload SHA256；不要混用不同架构或不同源码版本。
为保证共享节点的虚函数地址跨进程相同，二进制采用固定地址的非 PIE 链接。

B 独占创建 `UBSM_FLAG_CACHE` 对象、加载一次共享 BwTree 和 8-byte payload；A 映射
同一对象，固定 VA 为 `0x300000000000`。SDK map/ownership 权限按 A READ、B RW
设置。所有控制消息、操作 trace、结果和同步都在进程本地内存及 TCP 中。
两边完成准备后开始；A 在测量结束后验证 B 每个线程最后插入的 key。
只有 A 已完成 unmap/finalize 并退出，B 才释放对象。异常时保留可能仍在用的对象。

## 只读映射所需的 BwTree 调整

普通 `GetValue` 会写 epoch，部分 OPT 路径也会写共享缓存元数据，因此此入口采用
**无 OPT 的 BwTree 读优化遍历**（`TraverseReadOptimized`），不登记 A 的共享 epoch。
B 仍通过真实 `BwTree::Insert` 修改同一索引，支持分裂与 delta consolidation。

编译时 `prepare_source.py` 对复制的 BwTree 头文件作测试专用适配；不修改原索引文件。
停用 GC 和节点回收，所有节点保留到两端退出；NodeID 单调分配，移除空闲链表的
128-bit atomic。共享 bump allocator 容量耗尽即报错，不回退到私有堆。
映射表容量为 `2^20` 个 NodeID；内存大小和运行操作数需考虑保留节点的空间开销。

使用现有 native `nt` 标量原子路径，无 queued latency 模拟。B 在节点、payload 和
指针发布时写回，A 在读取 root、mapping entry、节点和 payload 时失效缓存。
ARM 使用 `dc cvac` / `dc civac` + `dsb sy`；x86 使用 `clflush` + `mfence`。
A 的代码不对 UB 区域执行 store/CAS。此配置的结果不能当作原 OPT/GC 配置的吞吐。

## 吞吐口径

每次运行写入 `result-A-*` / `result-B-*`，包括 `build.log`、`worker.log`、
`config.json`、`control.log` 和 `result.json`。两端获得相同汇总结果。

```
total_ops_per_second = (A 完成操作数 + B 完成操作数) / common_seconds
```

`common_seconds` 由 A 的单一 monotonic clock 从发出启动指令到收到两端完成结果计时，
包含线程启动和控制消息开销；不需要同步两台机器的时钟，也不把两个不同时间窗口
的局部吞吐直接相加。初始化、加载、trace 生成、最终 witness 验证、释放不计入。
局部 `seconds` 为各进程工作线程执行窗口。若一边先结束，其后的另一边运行时间仍
计入共同窗口；这是固定操作数的双进程总吞吐，不是固定时长的稳态吞吐。

任何操作错误、数量不足、权限/映射失败、验证失败或清理失败都返回非零。
脚本不扫描其他进程，不检查 CPU 占用；结果是否受其他实验干扰由实验运行者确认。

## 本地验证

```sh
python3 tests/ub-bwtree-ycsb/test_control.py
python3 tests/ub-bwtree-ycsb/test_readonly.py
```

第一项使用假 worker 验证双端协议和吞吐计算。第二项编译文件映射 SDK fixture，
用 32 条初始数据和每进程 128 次操作验证真正的 A `PROT_READ`、B 插入、分裂、
A 跨进程查询全部 key 和成功释放；丢弃计时，不作为性能样本。
这些检查不能证明真实双机 UB 的 CACHE/ownership 支持或硬件可见性。

## 定位等待阶段

终端会区分 B 监听、A 连接重试、TCP 已连接、握手完成、UB 初始化、加载和执行。
控制阶段保存到 `control.log`；worker 的 SDK 调用和加载进度同时输出到终端及
`worker.log`。最后一个没有对应 END 的 `BEGIN` 标记表示尚未返回的 SDK 调用。
A 显示等待 B load 时，看 B 的日志；这不表示 TCP 还未连接。

旧版只打印一次 `Waiting for other host...`，连接成功也不更新该提示。
在测试目录另开终端，查看现有运行日志（无需重启正在运行的测试）：

```sh
tail -n 30 result-B-*/worker.log  # B 上
```

只有日志明确显示 A 无法建立 TCP 连接时，再检查 B 的 18982 监听及两机网络。
