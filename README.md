# CXL-SDK

### [Documentation website →](https://memweave-org.github.io/docs/)

[Source code](https://github.com/memweave-org/CXL-SDK) ·
[Chinese guide](https://memweave-org.github.io/docs/zh/) ·
[English guide](https://memweave-org.github.io/docs/en/)

[![License: MIT](https://img.shields.io/badge/license-MIT-3da639.svg)](LICENSE)
[![C++](https://img.shields.io/badge/C%2B%2B-17-00599C.svg?logo=c%2B%2B)](https://en.cppreference.com/w/cpp/17)
[![CMake](https://img.shields.io/badge/CMake-3.10%2B-064F8C.svg?logo=cmake)](https://cmake.org/)
[![Platform](https://img.shields.io/badge/platform-Linux-FCC624.svg?logo=linux&logoColor=black)](https://www.kernel.org/)

A C++17 toolkit for shared-memory systems on
[Compute Express Link (CXL)](https://computeexpresslink.org/) and
[Unified Bus (UB)](https://www-file.huawei.com/admin/asset/v1/pro/view/a1c4051f4f3849ac9f09c2c301069c3a.pdf).
It includes a runtime, concurrent data structures, STL-style containers,
allocators, transactional memory, and workloads. A file-backed region supports
local development without CXL hardware.

## Why CXL-SDK

- **One abstraction across platforms.** CXL and UB expose memory-style access,
  but ownership, cache coherence, and synchronization vary. The SDK keeps those
  differences below the [data-structure layer](https://memweave-org.github.io/docs/zh/content/architecture.html).
- **Reusable shared state.** Object stores, file systems, and RPC systems need
  maps, indexes, and other concurrent structures. The SDK makes them a common
  building block; the [index-design preprint](https://arxiv.org/abs/2511.06460)
  explains how to adapt indexes to partial cache coherence.
- **Shared-memory primitives in one place.** The runtime, allocators, and
  transactional-memory implementations can be reused rather than rebuilt for
  each workload. See the [component overview](https://memweave-org.github.io/docs/zh/content/repo_components.html).
- **A path from prototype to hardware.** File-backed mappings support local
  experiments, while device-backed mappings can be used on suitable platforms.
  The [allocation guide](https://memweave-org.github.io/docs/zh/guides/memory-allocation.html)
  describes both paths.

The [BigMem paper](docs/bigmem26.pdf) develops the full SDK rationale.

## News

- **Code release:** [CXL-SDK source](https://github.com/memweave-org/CXL-SDK)
  is now available.
- **BigMem 2026:** [CXL-SDK: Building a Software Development Kit for CXL
  Shared Memory](docs/bigmem26.pdf) was accepted; see the
  [workshop program](https://bigmem2026.github.io/).
- **November 2025:** The data-structure design preprint,
  [Guidelines for Building Indexes on Partially Cache-Coherent CXL Shared
  Memory](https://arxiv.org/abs/2511.06460), is available on arXiv.

## Architecture

[![CXL-SDK architecture](docs/arch.png)](docs/arch.html)

## Components

| Directory | Role |
| --- | --- |
| [`shm-lib/`](shm-lib/) | Shared-memory runtime and messaging; [API guide](https://memweave-org.github.io/docs/zh/api/shm-lib-api.html) |
| [`ds/`](ds/) | Concurrent trees, tries, and hash tables; [data-structure guide](https://memweave-org.github.io/docs/zh/components/data_structures.html) |
| [`STL/`](STL/) | STL-style map wrappers; [API and examples](STL/README.md) |
| [`allocator/`](allocator/) | Shared-memory allocator implementations; [allocation guide](https://memweave-org.github.io/docs/zh/guides/memory-allocation.html) |
| [`TXMem/`](TXMem/) | TinySTM, TL2, and SwissTM transactional memory |
| [`apps/`](apps/) | STAMP application benchmarks |
| [`tests/`](tests/) | YCSB-C, correctness, and allocator tests |

## Quick start

Requires Linux, a C++17 compiler, CMake 3.10+, NUMA, and memkind development
libraries. From the repository root:

```bash
cmake -S STL -B build/STL
cmake --build build/STL --parallel
./build/STL/simple_demo
```

Build the runtime alone with `cmake -S shm-lib -B build/shm-lib` followed by
`cmake --build build/shm-lib`. For a
file-backed YCSB-C workload, follow the [YCSB-C guide](tests/YCSB-C/README.md),
[configuration guide](docs/configuration.md), and
[website user guide](https://memweave-org.github.io/docs/zh/user-guide.html).

## Documentation

See the [repository guides](docs/README.md) and
[validation guide](docs/validation.md).

## Contributing and license

See [CONTRIBUTING.md](CONTRIBUTING.md) for development steps. Original CXL-SDK
work is offered under the [MIT License](LICENSE); bundled components retain
their own terms, listed in [THIRDPARTY.md](THIRDPARTY.md).
