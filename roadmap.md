# Development roadmap

This roadmap describes work in progress and intended next steps, not completed
features or a release schedule. Items may change as implementation and testing
progress.

## Current focus on `master`

- **Shared-memory runtime and index adapters:** keep `shm-lib`, demos, and
  YCSB-C adapters buildable from a clean checkout; strengthen file-backed
  functional tests for the indexes that currently receive only compile checks.
- **Memory allocators:** make the bundled `cxl-shm` and `lsmalloc` paths
  reproducible in CI, replace hard-coded test backing paths and oversized
  fixtures where practical, and add correctness checks beyond build success.
- **Portable CI:** maintain software-only build and smoke checks on GitHub-hosted
  Ubuntu runners. Device-backed, multi-host, kernel, and failure-injection tests
  remain separate because they need suitable hardware and privileges.
- **Repository documentation:** keep the root README concise, maintain technical
  guidance under [`docs/`](docs/README.md), and remove local experiment output
  and internal drafts from the public source tree.

## Separate experimental work

- **LRPC prototype:** developed on the `dev/rpc` branch, not on `master`. Its
  userspace build, kernel module, and QEMU integration should be validated on
  that branch before any decision to merge it into the main SDK.

## Milestones before a broader release

1. A clean checkout passes the portable CI checks without relying on local,
   untracked build files.
2. Each advertised allocator and index has at least one repeatable functional
   test with documented resource requirements.
3. Hardware-specific behavior and performance claims are backed by separate
   CXL/UB experiment records; portable CI is not presented as hardware proof.
