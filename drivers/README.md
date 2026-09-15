# Kernel drivers

Optional Linux kernel modules used by SDK experiments live here. They are built
separately from the userspace runtime and benchmarks.

- [`uncached_ram/`](uncached_ram/): experimental character device that allocates
  RAM and exposes write-combining mappings to userspace.

Benchmarks and correctness tests remain in `tests/`.
