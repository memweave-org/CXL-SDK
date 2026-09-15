# STL

This is CXL-SDK's shared-memory STL-style container library. It provides
map-like APIs backed by concurrent data structures; the programs in `examples/`
demonstrate single-process and multi-process use.

| Container | Backend | Interface and types |
| --- | --- | --- |
| `pcc::btree` | BwTree | `std::map`-style; `int64_t` keys and values |
| `pcc::clevelhash` | ClevelHash | `std::unordered_map`-style; `uint64_t` keys and values |

Both offer `insert`, `update`, `find`, `erase`, `contains`, `size`, and `empty`.
These are STL-style wrappers with fixed integer types; they do not yet provide
standard iterators or allocator interfaces.

## Build and try

From the repository root:

```bash
cmake -S STL -B build/STL
cmake --build build/STL --parallel
./build/STL/simple_demo
./build/STL/simple_demo clevelhash
./build/STL/multi_process_demo
```

The CMake library target is `STL`; the two executables are usage examples.
Public headers are under `include/pcc/`.

## Multi-process use

Initialize shared memory with `pcc::init_shm(config)`, then create or attach to
a container using its static `init(shm_id, is_creator, thread_num)` method.
Each accessing thread calls `thread_init(thread_id)` before operations and
`thread_cleanup(thread_id)` afterward. Use the same shared-memory ID and
compatible mapping configuration in every process. Only the creator removes
the shared-memory instance with `cleanup(shm_id)`.

See [simple_demo.cc](examples/simple_demo.cc) for insert and lookup operations
and [multi_process_demo.cc](examples/multi_process_demo.cc) for creator/attacher
behavior.
