ARG BUILD_IMAGE
FROM ${BUILD_IMAGE} AS build
COPY include/ /sdk/include/
COPY lib/ /sdk/lib/
COPY worker.cc /src/worker.cc
RUN c++ -std=c++17 -O2 -g -Wall -Wextra -Werror -I/sdk/include \
    /src/worker.cc -L/sdk/lib -lubsm_sdk -Wl,-rpath-link,/sdk/lib -o /worker
