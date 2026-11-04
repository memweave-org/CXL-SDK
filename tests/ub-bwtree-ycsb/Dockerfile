ARG BUILD_IMAGE
FROM ${BUILD_IMAGE}
COPY include/ /sdk/include/
COPY lib/ /sdk/lib/
COPY src/ /src/
RUN UBSM_INCLUDE_DIR=/sdk/include UBSM_LIBRARY=/sdk/lib/libubsm_sdk.so \
    bash /src/tests/ub-bwtree-ycsb/build.sh /out/worker
