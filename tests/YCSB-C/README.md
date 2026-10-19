# CXL-SDK YCSB-C driver

This directory contains the YCSB-C workload driver and adapters used to test
CXL-SDK's concurrent indexes. The driver is based on the C++ port of the
[Yahoo! Cloud Serving Benchmark](https://github.com/basicthinker/YCSB-C) and has
been extended with shared-memory setup, paper experiment variants, and adapters
for BwTree, ClevelHash, CLHT, HOT, Masstree, RadixART, and BTreeOLC.

## Dependencies

On Ubuntu or Debian:

```bash
sudo apt-get install -y \
  build-essential cmake libboost-all-dev \
  libibverbs-dev libjemalloc-dev libmemkind-dev libnuma-dev \
  libssh-dev libtbb-dev
```

## Build

### UB Docker toolchain

`Dockerfile.openEuler` uses openEuler GCC/G++ 12.3.1 and builds glibc 2.39
in a separate stage before copying it into the image. Build-time C/C++ checks
verify the loaded libc version. This changes only the container, not the host.
The installed glibc comes from source; the base image's RPM database still
records its distribution glibc. Do not upgrade glibc inside this image with dnf;
rebuild the Dockerfile and retain its version checks instead.

From any directory, invoke the script by its path (example from repository root):

```bash
bash tests/YCSB-C/docker_run_build.sh -b -r cc nocc
# Open an interactive shell in the same environment:
bash tests/YCSB-C/docker_run_build.sh -r
```

Default image: `shm-pcc-sdk:ub-glibc2.39-gcc12.3.1`. The script checks versions
even when reusing an existing image or specifying `-i`. It builds for the Docker
host architecture by default; build natively on the ARM UB machine for ARM
binaries. Cross-platform builds require Docker emulation support, for example
with `DOCKER_DEFAULT_PLATFORM=linux/arm64`. Both architectures share the same
local tag, so rebuild with `-b` when changing target architecture.

```bash
docker run --rm shm-pcc-sdk:ub-glibc2.39-gcc12.3.1 getconf GNU_LIBC_VERSION
docker run --rm shm-pcc-sdk:ub-glibc2.39-gcc12.3.1 gcc -dumpfullversion
```

This is a compilation environment. Vendor Memlink/UBSM SDK libraries and UB
runtime devices/services must still be supplied separately for hardware runs.

### Native build

Build one or more variants. The final variant becomes the `./ycsbc` symlink.

```bash
./build.sh cc nocc
```

The `cc` variant uses ordinary cache-coherent atomics. The `nocc` variant
enables the paper's cache-bypassing implementation plus the G2/G3 optimization
flags. Additional variants are declared in [`CMakeLists.txt`](CMakeLists.txt).

For an isolated compile-only check, use a temporary build directory:

```bash
cmake -S . -B /tmp/cxl-sdk-ycsb -DVARIANT=nocc
cmake --build /tmp/cxl-sdk-ycsb --parallel
```

## Local smoke run

The tracked `config.ini` describes the paper hardware setup and expects a DAX
device. For a single-process file-backed smoke test, create the backing file and
select `config.local.ini` explicitly:

```bash
truncate -s 1G /tmp/cxl-sdk-shm
./run_shm_ds.sh -db=bwtree -mode=smoke -config=config.local.ini
```

The local configuration requests a fixed virtual address. If that address is
already occupied, adjust `mmap_base_addr`; all cooperating processes must use
the same address.

## Paper workloads

The 100-million-record YCSB A/B/C configurations used by the paper are:

- `workloads/workloada_zipfian_100m.spec` — 50% read, 50% update;
- `workloads/workloadb_zipfian_100m.spec` — 95% read, 5% update; and
- `workloads/workloadc_zipfian_100m.spec` — 100% read.

The generator's Zipf exponent defaults to 0.99. Paper-scale runs are
hardware-specific and may modify ASLR only when explicitly requested:

```bash
DISABLE_ASLR=1 ./run_shm_ds.sh \
  -db=bwtree -mode=server_thread_scale_test -config=config.ini
```

`DISABLE_ASLR=1` uses `sudo` and restores the previous kernel setting after the
command. It is unnecessary for ordinary builds and should be enabled only on a
dedicated experiment host.

## External Twitter traces

The Twitter cache traces are not distributed in this repository. Point the
driver at a dataset layout containing `<TRACE_PATH>/<TRACE_NAME>/clusterNNN`:

```bash
TRACE_PATH=/data/cache-trace/samples \
TRACE_NAME=2020Mar \
./run_shm_ds.sh -db=bwtree -mode=run_real_workloads
```

The launcher validates the dataset directory before starting. No developer
machine path is embedded in the script.

## Direct invocation

You can bypass the launcher and invoke the selected binary directly:

```bash
./ycsbc \
  -db bwtree \
  -client_threads 1 \
  -server_threads 2 \
  -dbnum 1 \
  -P workloads/workloada_small.spec \
  -C config.local.ini
```

Run `./ycsbc` without arguments to see all supported options.
