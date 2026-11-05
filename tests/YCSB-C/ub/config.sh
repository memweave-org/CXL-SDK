# Environment overrides work for both run.sh and run_all.sh.
A_IP=${A_IP:-76.53.137.81}
B_IP=${B_IP:-76.53.137.82}
PROVIDER_B=${PROVIDER_B:-} # Empty: B system hostname / UB topology name
BUILD_IMAGE=${BUILD_IMAGE:-shm-pcc-sdk:ub-glibc2.39-gcc12.3.1}
SDK_ROOT=${SDK_ROOT:-/usr/local/ubs_mem}
PORT=${PORT:-18982}
THREADS_A=${THREADS_A:-1}
THREADS_B=${THREADS_B:-1}
RECORDS=${RECORDS:-100000}
OPS=${OPS:-1000000} # Per host
SIZE_MB=${SIZE_MB:-4096}
NC_SIZE_MB=${NC_SIZE_MB:-1024}
SPEC=${SPEC:-workloadb_90read.spec}
OPT=${OPT:-1} # Legacy run.sh: 1 OPT, 0 NOOPT
SEED=${SEED:-20261104}
A_WRITABLE=${A_WRITABLE:-0} # Compatibility only: A is always read-only; 1 is rejected.
# CPU_A / CPU_B: start at this allowed CPU ID, skipping unavailable IDs.
# Each worker uses a distinct allowed logical CPU; RPC shares B's first selected CPU.
