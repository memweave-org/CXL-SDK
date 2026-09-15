### Principle
global:
1. init a cxl device's memory as allocator's shared memory
2. create background thread to do gc

local:
each thread/process want to use an allocator, it should:
1. init a local allocator and attach to shared memory so it can process shared memory

### data structure

[meta] [entries] [log_segment]

meta:





### Interface

global:
```cpp
void init_allocator(const char* cxl_dev_path, size_t total_size);
gc 
global epoch
```

local:

```cpp
class lsmallocator {
public:
    lsmallocator(void* shm_base, cid_t cid);
    ~lsmallocator();

    // allocate/free an object
    SHMRef<T> alloc() const;
    void free(const SHMRef<T>& ref) const;

    // read and write object as whole
    T* read(const SHMRef<T>& ref) const;
    void write(const SHMRef<T>& ref, const T& data) const;

    // unsafe get ref and release ref for performance
    T* acquire_ref(const SHMRef<T>& ref) const;
    void release_ref(const SHMRef<T>& ref) const;
private:
    cid_t cid;
    void* shm_base;
};
```