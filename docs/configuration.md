# Shared-memory configuration

YCSB-C's [`config.ini`](../tests/YCSB-C/config.ini) records a DAX-backed
experiment setup. [`config.local.ini`](../tests/YCSB-C/config.local.ini) provides
an explicit file-backed configuration for local smoke tests. Important fields
include:

```ini
[shm/cacheable]
mem_type=local
device_path=/tmp/cxl-sdk-shm
mmap_base_addr=0x400000000000
mem_size=1024
allocator_backend=memkind
```

| Setting | Purpose |
| --- | --- |
| `mem_type` | Selects the configured shared-memory type |
| `device_path` | CXL device or file-backed shared-memory path |
| `mmap_base_addr` | Requested virtual mapping base address |
| `mem_size` | Region size in MiB |
| `allocator_backend` | Lower allocator; the public build provides `memkind` |

For multi-process runs, participants must use compatible mapping addresses,
region sizes, and shared-memory paths. Review the launcher before use: it can
create or resize backing files, and ASLR changes are opt-in and may require
privileges.

Twitter traces are external inputs. Configure `TRACE_PATH` and `TRACE_NAME`;
see the [YCSB-C guide](../tests/YCSB-C/README.md) for workload details.
