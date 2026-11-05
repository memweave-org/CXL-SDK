# 双机 NC 最小验证

复用上一层 CACHE micro 的编译和 TCP 控制框架，只映射一块 NC 内存，不启动 YCSB。
两边使用上一层 `config.sh` 的 IP、SDK 和编译镜像配置。

在 A、B 两台物理机上分别执行：

```bash
cd tests/ub-cache-visibility/nc
bash ./run.sh
```

- B 创建 4 MiB 对象：`UBSM_FLAG_NONCACHE | UBSM_FLAG_WR_DELAY_COMP`。
- 两边 `PROT_READ | PROT_WRITE`、`MAP_SHARED`，请求地址 `0x400000000000`。
  `MAP_SHARED` 下地址只是请求；若 SDK 返回不同地址，测试明确失败，不继续访问。
- B 初始化一个对齐的 8 byte，A 读回；B 改写、A 读回；A 改写、B 读回。
- A、B 分别在同一位置执行 64-bit CAS 成功与失败用例，检查成功标志和返回的
  expected（失败时必须更新为实际旧值），并检查失败没有改写内存。
- A、B 分别执行 64-bit fetch-add，检查返回的是加法前的旧值；覆盖大于 32-bit
  的增量和 uint64 回绕。每次写/CAS/fetch-add 后，两端都读回检查最终值。
- 普通读写使用标量 volatile，CAS/fetch-add 使用原生无锁 `__atomic_*` 和全系统
  内存屏障。不使用模拟原子队列，不调用 ownership、cache flush/invalidate。
- 控制走 TCP，默认端口 18983。A 退出后 B 才释放对象。
- Docker 只 build，导出 worker 后在物理机执行。成功显示 `"pass": true`。
  `result.json` 的 `checks` 保存每项的命令、期望、实际回复和通过状态；32 项全部
  通过且两端清理成功才算成功。任何错误立即失败，不挑选重试中的成功样本。

这是顺序交接的基本正确性测试，不验证双端同时竞争的线性一致性或性能。
更新时同步两边整个 `tests/ub-cache-visibility` 目录；NC 控制协议版本不匹配会失败。

比较当前 YCSB 的映射 flag（两边都使用同一命令）：

```bash
NC_MAP_MODE=noreplace bash ./run.sh
```

这会使用 `MAP_SHARED | MAP_FIXED_NOREPLACE`，不自动重试或切换 flag。
验证 YCSB 同样的 1 GiB NC 大小时：

```bash
NC_SIZE_MB=1024 NC_MAP_MODE=noreplace bash ./run.sh
```

日志在上一层 `result-nc-A-*/worker.log` / `result-nc-B-*/worker.log`，
并同步打印在终端。每个正常初始化/清理 SDK 调用前有 `BEGIN`，返回后有 `rc=`；
每条内存操作也有 `BEGIN command ...`，便于区分初始化、原子操作和清理等待。
例如停在 `BEGIN map` 且没有 `map rc=`，表示该次 SDK map 尚未返回。
`NC_TIMEOUT` 默认为 120 秒，可按需设置；超时报最后阶段及 worker PID。

从仓库根目录运行本地替身验证：

```bash
python3 tests/ub-cache-visibility/test_nc.py
```

它编译真实 worker、使用本地 SDK fixture 及两个 TCP 控制端，检查返回值、双向
读回、错误注入和安全释放。fixture 仅支持 `noreplace`，使用本机普通共享文件，
不能证明真实 SDK 的 NC 属性、跨机器原子语义或 flag 支持。
