#ifndef UNCACHED_RAM_UAPI_H
#define UNCACHED_RAM_UAPI_H
#include <linux/ioctl.h>
#include <linux/types.h>
/* Keep the original node-selection ioctl number. One configuration per open. */
#define UNCACHED_RAM_SET_NODE 0
#define UNCACHED_RAM_SET_HUGE 1
#define UNCACHED_RAM_ABI_VERSION 3
#define UNCACHED_RAM_CACHE_UC_MINUS 1
struct uncached_ram_info {
  __u32 abi_version;
  __u32 cache_mode;
  __s32 node;
  __u32 reserved;
  __u64 bytes;
  __u64 pages;
  __u64 mapping_page_size;
  __u64 pmd_faults; /* Successful PMD fault operations, not unique PMDs. */
};
#define UNCACHED_RAM_DMA_READ 0
#define UNCACHED_RAM_DMA_WRITE 1
struct uncached_ram_dma_copy {
  __u64 offset;
  __u64 user_buffer;
  __u32 bytes;
  __u32 direction;
};
/* Requests submitted and not yet reaped by their synchronous callers. */
struct uncached_ram_dma_stats {
  __u32 slots;
  __u32 active;
  __u32 peak;
  __u32 reserved;
};
#define UNCACHED_RAM_DMA_STATS _IOR('U', 5, struct uncached_ram_dma_stats)
/* Optional direct path, ABI3 extension. Enable after arena + DMA setup.
 * mmap slot i at file offset (i+1)*PAGE_SIZE, length slot_bytes.
 * The mapping is the final local DMA buffer: no copy_to/from_user on this path.
 * Callers must not touch a slot until its synchronous DIRECT ioctl returns.
 */
struct uncached_ram_direct_info {
  __u32 slots;
  __u32 slot_bytes;
};
struct uncached_ram_direct_copy {
  __u64 remote_offset;
  __u32 bytes;
  __u32 slot;
  __u32 direction;
  __u32 flags; /* bit 0: poll completion; zero: sleep */
};
#define UNCACHED_RAM_DIRECT_ENABLE _IOR('U', 6, struct uncached_ram_direct_info)
#define UNCACHED_RAM_DIRECT_COPY _IOW('U', 7, struct uncached_ram_direct_copy)
#define UNCACHED_RAM_ENABLE_DMA _IOW('U', 3, __s32)
#define UNCACHED_RAM_DMA_COPY _IOW('U', 4, struct uncached_ram_dma_copy)
#define UNCACHED_RAM_GET_INFO _IOR('U', 2, struct uncached_ram_info)
#endif
