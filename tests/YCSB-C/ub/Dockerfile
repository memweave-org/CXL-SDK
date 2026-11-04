ARG BUILD_IMAGE=shm-pcc-sdk:ub-glibc2.39-gcc12.3.1
FROM ${BUILD_IMAGE}
COPY include/ /sdk/include/
COPY lib/ /sdk/lib/
COPY src/ /src/
ARG OPT=1
ARG JOBS=2
ENV LD_LIBRARY_PATH=/sdk/lib
RUN set -eu; \
    case "$OPT" in 1) variant=nocc;; 0) variant=nocc_no_opt;; *) exit 2;; esac; \
    cd /src/tests/YCSB-C; \
    YCSB_UB_TWO_HOST=1 SHM_UB_API=ubsm UBSM_INCLUDE_DIR=/sdk/include \
      UBSM_LIBRARY=/sdk/lib/libubsm_sdk.so JOBS="$JOBS" bash ./build.sh "$variant"; \
    mkdir -p /out; \
    cp "build_${variant}_ub/ycsbc_${variant}" /out/ycsbc; \
    python3 /src/tests/two-process-ycsb/collect_ub_runtime.py --library /out/ycsbc --output /out/lib; \
    rm /out/lib/ycsbc
