A_IP='76.53.137.81'
B_IP='76.53.137.82'
PROVIDER_B='' # 默认 B 的系统 hostname；可指定 UB 拓扑名
BUILD_IMAGE='shm-pcc-sdk:ub-glibc2.39-gcc12.3.1'
SDK_ROOT='/usr/local/ubs_mem'
PORT=18982
THREADS_A=1
THREADS_B=1
RECORDS=100000
OPS=1000000 # 每台机器的操作数
SIZE_MB=4096
SPEC=workloadb_90read.spec
# 可选 CPU_A=2 CPU_B=2；线程顺序绑定从该 CPU 开始的连续核心。

OPT=${OPT:-1} # 1: 原 BwTree OPT；0: NOOPT
NC_SIZE_MB=512 # A/B 都可读写的独立 NONCACHE 对象
