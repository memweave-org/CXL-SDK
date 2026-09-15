# GC Algorithm

Memory compaction is the core operation of garbage collection, which is to move the valid data blocks in memory to the starting position of the memory space to eliminate memory fragmentation. The specific implementation process is as follows:

## 1. Compression Algorithm Principle

```
[Memory space diagram]
+----------------+----------------+----------------+----------------+
|  used block1   |    free block  |    used block  |    free block  |
+----------------+----------------+----------------+----------------+
      ^               ^               ^               ^
      |               |               |               |
    read_ptr       free space     used block2    free space
    write_ptr
```

Compression process uses two pointers:
- `read_ptr`：Scan the entire memory space to find valid data blocks
- `write_ptr`：Point to the next writable position

## 2. Compression Steps

1. **Initialization**
   ```cpp
   char* read_ptr = static_cast<char*>(base_addr);
   char* write_ptr = static_cast<char*>(base_addr);
   ```
   - Both pointers start from the beginning of the memory space
   - `base_addr` points to the start of the data region (after the reference array)

2. **Scan and Move**
   ```cpp
   while (current_read_offset < current_offset.load(std::memory_order_relaxed)) {
       BlockHeader* header = reinterpret_cast<BlockHeader*>(read_ptr);
       
       if (header->is_used) {
           // If this is a used block, it needs to be moved
           if (read_ptr != write_ptr) {
               size_t block_size = sizeof(BlockHeader) + header->size;
               std::memcpy(write_ptr, read_ptr, block_size);
               // ... update references and cache
           }
           write_ptr += sizeof(BlockHeader) + header->size;
       }
       
       read_ptr += sizeof(BlockHeader) + header->size;
   }
   ```
   - Scan each data block header information
   - If the block is in use, move it to the `write_ptr` position
   - Update `write_ptr` to the next available position

3. **Update References**
   ```cpp
   void* old_data_addr = read_ptr + sizeof(BlockHeader);
   void* new_data_addr = write_ptr + sizeof(BlockHeader);
   update_references(old_data_addr, new_data_addr, header->size);
   ```
   - When the data block is moved, all references pointing to it need to be updated
   - Ensure that the address in the reference array points to the new position

4. **Cache Consistency**
   ```cpp
   flush_cache_lines(write_ptr, block_size);
   ```
   - After moving the data block, flush the relevant cache lines
   - Ensure that other machines can see the updated data

5. **Update Memory Usage**
   ```cpp
   current_offset.store(write_ptr - static_cast<char*>(base_addr), std::memory_order_release);
   ```
   - Update the current memory usage position
   - Use atomic operations to ensure thread safety

## 3. Compression Effect

Before compression:
```
[Memory space]
+----+----+----+----+----+----+
|used|free|used|free|used|free|
+----+----+----+----+----+----+
```

After compression:
```
[Memory space]
+----+----+----+     +----+
|used|used|used|     |free|
+----+----+----+     +----+
```
