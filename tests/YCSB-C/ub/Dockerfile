ARG BUILD_IMAGE=shm-pcc-sdk:ub-glibc2.39-gcc12.3.1
FROM ${BUILD_IMAGE}
COPY include/ /sdk/include/
COPY lib/ /sdk/lib/
COPY src/ /src/
ARG OPT=1
ARG YCSB_RPC_BUILD=0
ARG JOBS=2
ARG YCSB_INDEXES=bwtree
ENV LD_LIBRARY_PATH=/sdk/lib
RUN set -eu; \
    case "$OPT" in 1) variant=nocc;; 0) variant=nocc_no_opt;; *) exit 2;; esac; \
    if [ "$YCSB_RPC_BUILD" = 1 ]; then variant=cc_nocc_mq; fi; \
    cd /src/tests/YCSB-C; \
    YCSB_UB_TWO_HOST=1 SHM_UB_API=ubsm UBSM_INCLUDE_DIR=/sdk/include \
      UBSM_LIBRARY=/sdk/lib/libubsm_sdk.so JOBS="$JOBS" YCSB_INDEXES="$YCSB_INDEXES" bash ./build.sh "$variant"; \
    mkdir -p /out; \
    cp "build_${variant}_ub/ycsbc_${variant}" /out/ycsbc; \
    python3 /src/tests/two-process-ycsb/collect_ub_runtime.py --library /out/ycsbc --output /out/lib; \
    rm /out/lib/ycsbc
