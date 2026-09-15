# Validation

The repository has several test areas:

- [`tests/basic/`](../tests/basic/) — low-level runtime and shared-memory tests.
- [`tests/correctness/`](../tests/correctness/) — data-structure correctness tests.
- [`tests/allocator/`](../tests/allocator/) — allocator-focused tests.
- [`tests/YCSB-C/`](../tests/YCSB-C/) — configurable key-value workloads.
- [`apps/stamp/`](../apps/stamp/) — transactional-memory application suite.

The [GitHub Actions workflow](../.github/workflows/build-and-test.yml) builds
the shared-memory STL layer and YCSB-C index adapters, runs a file-backed
BwTree smoke workload, and builds/tests the memory allocators. These portable
checks do not validate real CXL/UB behavior, multi-host topology, failure
recovery, or performance.
Such claims require suitable hardware and separate experiment records.

Run component checks from their source directory and record any hardware,
privilege, kernel, or topology requirements that prevent local execution.
