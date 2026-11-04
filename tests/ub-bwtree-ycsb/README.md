# 入口已迁移到原 YCSB-C 框架

本目录 `run.sh` 现转到 `tests/YCSB-C/run.sh`，执行原 `ycsbc` 框架。
旧 `worker.cc` 不再由一键入口编译或运行。

配置请修改 `tests/YCSB-C/ub/config.sh`，本目录的旧 config.sh 不再生效。
说明见 [原框架 UB 模式](../YCSB-C/ub/README.md)。

两边分别执行：

```sh
cd /mnt/disk1/wfn/CXL-SDK/tests/YCSB-C
bash ./run.sh          # OPT；NOOPT 用 OPT=0 bash ./run.sh
```
