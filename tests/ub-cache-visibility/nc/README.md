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
- 仅用标量 volatile 读写和内存屏障，不调用 ownership、cache flush/invalidate。
- 控制走 TCP，默认端口 18983。A 退出后 B 才释放对象。
- Docker 只 build，导出 worker 后在物理机执行。成功显示 `"pass": true`。

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
并同步打印在终端。每次 SDK 调用前有 `BEGIN`，返回后有 `rc=`。
例如停在 `BEGIN map` 且没有 `map rc=`，表示该次 SDK map 尚未返回。
`NC_TIMEOUT` 默认为 120 秒，可按需设置；超时报最后阶段及 worker PID。

本地替身验证只能检查协议、地址与读写路径，不能证明真实 SDK 的 NC 属性或 flag 支持。
