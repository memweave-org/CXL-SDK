# 同一份配置复制到 A、B；按本机 IP 自动识别角色。
A_IP='76.53.137.81'
B_IP='76.53.137.82'
# 留空时使用 B 的系统 hostname；若 UB 拓扑名不同，在这里指定。
PROVIDER_B=''

BUILD_IMAGE='shm-pcc-sdk:ub-glibc2.39-gcc12.3.1'
SDK_ROOT='/usr/local/ubs_mem'
PORT=18981
# 可选：CPU=2；不设置则自动选择当前允许的第一个 CPU。
