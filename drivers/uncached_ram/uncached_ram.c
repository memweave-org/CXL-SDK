/**
 * @file    uncached_ram.c
 * @author  lemonsqueeze
 * @date    26 Jul 2017
 * @version 0.1
 * @brief Map uncached memory in userspace
 * @see http://github.com/lemonsqueeze/uncached_ram_lkm
 */

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>

#include <asm/set_memory.h>
#include <linux/page-flags.h>

#include <linux/device.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/numa.h>
#include <linux/slab.h>

// 1GB = 2^30 bytes = 2^18 pages (assuming 4KB pages)
#define SZ_1GB (1UL << 30)
#define PAGES_PER_1GB (SZ_1GB / PAGE_SIZE)
#define HUGE_PAGE_ORDER_1GB 30  // 2^30 * 4KB = 1GB

MODULE_LICENSE("GPL");
MODULE_AUTHOR("lemonsqueeze");
MODULE_DESCRIPTION("Map uncached mem to userspace with huge pages.");
MODULE_VERSION("0.1");

static int uncached_mem_numa_node = -1;
static int use_huge_pages = 1;  // Use huge pages by default

struct buffer {
  char **pages;  // Store page addresses (normal pages or huge pages)
  size_t page_count;  // Number of pages (normal page count or huge page count)
  size_t total_size;  // Total size (bytes)
  int is_huge_page;  // Whether to use huge pages
};

struct client {
  struct buffer buffer;
  unsigned long vm_start;
};

/* From kernel Documentation/x86/pat.txt:
 *
 * Advanced APIs for drivers
 * -------------------------
 * A. Exporting pages to users with remap_pfn_range, io_remap_pfn_range,
 * vm_insert_pfn
 *
 * Drivers wanting to export some pages to userspace do it by using mmap
 * interface and a combination of
 * 1) pgprot_noncached()
 * 2) io_remap_pfn_range() or remap_pfn_range() or vm_insert_pfn()
 *
 * With PAT support, a new API pgprot_writecombine is being added. So, drivers
 * can continue to use the above sequence, with either pgprot_noncached() or
 * pgprot_writecombine() in step 1, followed by step 2.
 *
 * In addition, step 2 internally tracks the region as UC or WC in memtype
 * list in order to ensure no conflicting mapping.
 *
 * Note that this set of APIs only works with IO (non RAM) regions. If driver
 * wants to export a RAM region, it has to do set_memory_uc() or set_memory_wc()
 * as step 0 above and also track the usage of those pages and use
 * set_memory_wb() before the page is freed to free pool.
 */

/* insert_page() code is based on drivers/firewire/core-cdev.c */

static void buffer_destroy(struct buffer *buffer) {
  size_t i;
  printk(KERN_INFO "Freeing %s pages (count: %zu)\n", 
         buffer->is_huge_page ? "huge" : "normal", buffer->page_count);

  for (i = 0; i < buffer->page_count; i++) {
    if (buffer->pages[i]) {
      char *addr = buffer->pages[i];
      struct page *page = virt_to_page(addr);
      size_t pages_to_wb;

      if (buffer->is_huge_page) {
        // Huge page: restore WB mode, in 1GB units
        pages_to_wb = PAGES_PER_1GB;
        set_memory_wb((unsigned long)addr, pages_to_wb);
        ClearPageReserved(page);
        __free_pages(page, HUGE_PAGE_ORDER_1GB);
      } else {
        // Normal page
        pages_to_wb = 1;
        set_memory_wb((unsigned long)addr, pages_to_wb);
        ClearPageReserved(page);
        if (uncached_mem_numa_node == -1) {
          free_page((unsigned long)addr);
        } else {
          __free_pages(page, 0);
        }
      }
    }
  }

  kvfree(buffer->pages);
  buffer->pages = NULL;
  buffer->page_count = 0;
  buffer->total_size = 0;
}

static int buffer_alloc(struct buffer *buffer, size_t total_size) {
  size_t i;
  gfp_t gfp_flags = GFP_KERNEL | __GFP_NOWARN;
  
  buffer->total_size = total_size;
  buffer->is_huge_page = use_huge_pages;

  if (buffer->is_huge_page) {
    // Use 1GB huge pages
    size_t gb_count = (total_size + SZ_1GB - 1) / SZ_1GB;  // Round up
    
    printk(KERN_INFO "Allocating %zu 1GB huge pages (total size: %zu bytes)\n", 
           gb_count, total_size);
    
    buffer->page_count = gb_count;
    buffer->pages = kvcalloc(gb_count, sizeof(buffer->pages[0]), GFP_KERNEL);
    if (buffer->pages == NULL) {
      return -ENOMEM;
    }

    for (i = 0; i < gb_count; i++) {
      struct page *page = NULL;
      char *addr = NULL;
      
      // Allocate 1GB huge page (order = 30)
      if (uncached_mem_numa_node == -1) {
        page = alloc_pages(gfp_flags, HUGE_PAGE_ORDER_1GB);
      } else {
        page = alloc_pages_node(uncached_mem_numa_node, gfp_flags, 
                                HUGE_PAGE_ORDER_1GB);
      }
      
      if (page == NULL) {
        printk(KERN_ERR "Failed to allocate 1GB huge page %zu\n", i);
        break;
      }
      
      addr = page_address(page);
      if (addr == NULL) {
        __free_pages(page, HUGE_PAGE_ORDER_1GB);
        break;
      }
      
      buffer->pages[i] = addr;
      SetPageReserved(page);
      
      // Set to WC mode, in 1GB units (PAGES_PER_1GB pages)
      if (set_memory_wc((unsigned long)addr, PAGES_PER_1GB)) {
        printk(KERN_ERR "Failed to set memory WC for 1GB huge page %zu\n", i);
        ClearPageReserved(page);
        __free_pages(page, HUGE_PAGE_ORDER_1GB);
        break;
      }
      
      printk(KERN_INFO "Allocated 1GB huge page %zu at %p\n", i, addr);
    }
  } else {
    // Use normal 4KB pages
    size_t page_count = total_size >> PAGE_SHIFT;
    
    printk(KERN_INFO "Allocating %zu normal pages (total size: %zu bytes)\n", 
           page_count, total_size);
    
    buffer->page_count = page_count;
    buffer->pages = kvcalloc(page_count, sizeof(buffer->pages[0]), GFP_KERNEL);
    if (buffer->pages == NULL) {
      return -ENOMEM;
    }

    for (i = 0; i < page_count; i++) {
      char *addr = NULL;
      if (uncached_mem_numa_node == -1) {
        addr = (char *)__get_free_page(GFP_KERNEL);
      } else {
        struct page *page =
            alloc_pages_node(uncached_mem_numa_node, GFP_KERNEL, 0);
        if (page == NULL)
          break;
        addr = page_address(page);
      }
      if (addr == NULL)
        break;
      buffer->pages[i] = addr;
      SetPageReserved(virt_to_page(addr));
      if (set_memory_wc((unsigned long)addr, 1))
        break;
    }
  }

  if (i < buffer->page_count) {
    buffer_destroy(buffer);
    return -ENOMEM;
  }

  return 0;
}

static int buffer_map_vma(struct buffer *buffer, struct vm_area_struct *vma) {
  unsigned long uaddr;
  size_t i;
  int err;
  unsigned long remaining_size = buffer->total_size;

  uaddr = vma->vm_start;
  
  if (buffer->is_huge_page) {
    // Map 1GB huge pages
    for (i = 0; i < buffer->page_count && remaining_size > 0; i++) {
      unsigned long map_size = (remaining_size < SZ_1GB) ? remaining_size : SZ_1GB;
      unsigned long pfn = page_to_pfn(virt_to_page(buffer->pages[i]));
      
      // Use remap_pfn_range to map the entire 1GB region (or remaining part)
      err = remap_pfn_range(vma, uaddr, pfn, map_size, vma->vm_page_prot);
      if (err) {
        printk(KERN_ERR "Failed to remap 1GB huge page %zu at %lx (size: %lu)\n", 
               i, uaddr, map_size);
        return err;
      }
      
      uaddr += map_size;
      remaining_size -= map_size;
    }
  } else {
    // Map normal 4KB pages
    for (i = 0; i < buffer->page_count; i++) {
      err = vm_insert_page(vma, uaddr, virt_to_page(buffer->pages[i]));
      if (err) {
        printk(KERN_ERR "Failed to insert page %zu at %lx\n", i, uaddr);
        return err;
      }
      uaddr += PAGE_SIZE;
    }
  }

  return 0;
}

int cdev_major;

static int device_op_open(struct inode *inode, struct file *file) {
  struct client *client;

  client = kzalloc(sizeof(*client), GFP_KERNEL);
  if (client == NULL)
    return -ENOMEM;

  file->private_data = client;

  return nonseekable_open(inode, file);
}

static int device_op_release(struct inode *inode, struct file *file) {
  struct client *client = file->private_data;

  if (client->buffer.pages)
    buffer_destroy(&client->buffer);

  kfree(client);
  return 0;
}

static int device_op_mmap(struct file *file, struct vm_area_struct *vma) {
  struct client *client = file->private_data;
  unsigned long size;
  int ret;

  if (!(vma->vm_flags & VM_SHARED))
    return -EINVAL;

  if (vma->vm_start & ~PAGE_MASK)
    return -EINVAL;

  client->vm_start = vma->vm_start;
  size = vma->vm_end - vma->vm_start;
  
  // Size must be a multiple of page size
  if (size & ~PAGE_MASK)
    return -EINVAL;

  /* only one mmap() call per device for now */
  if (client->buffer.pages)
    return -EAGAIN;

  // If using huge pages, require 1GB alignment (optional, but recommended)
  if (use_huge_pages && (vma->vm_start & (SZ_1GB - 1))) {
    printk(KERN_WARNING "vm_start not 1GB aligned: %lx (recommended for huge pages)\n", 
           vma->vm_start);
  }

  ret = buffer_alloc(&client->buffer, size);
  if (ret)
    return ret;

  // vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
  vma->vm_page_prot = pgprot_writecombine(vma->vm_page_prot);

  ret = buffer_map_vma(&client->buffer, vma);
  if (ret)
    goto fail;

  printk(KERN_INFO "uncached ram mmap successful: %lu bytes (%zu %s pages)\n",
         size, client->buffer.page_count, 
         client->buffer.is_huge_page ? "1GB huge" : "normal");
  return 0;
fail:
  buffer_destroy(&client->buffer);
  return ret;
}

enum { 
  MY_DEVICE_IOCTL_ALLOC_BUFFER = 0,
  MY_DEVICE_IOCTL_SET_HUGE_PAGES = 1,
};

static long device_op_ioctl(struct file *file, unsigned int cmd,
                            unsigned long arg) {
  pr_info("ioctl: %d\n", cmd);
  pr_info("arg: %ld\n", arg);
  pr_info("file: %p\n", file);
  pr_info("numa_node: %d\n", uncached_mem_numa_node);
  switch (cmd) {
  case MY_DEVICE_IOCTL_ALLOC_BUFFER:
    if (arg >= num_online_nodes()) {
      pr_err("Invalid NUMA node: %ld\n", arg);
      return -EINVAL;
    }
    uncached_mem_numa_node = arg;
    pr_info("Set numa node to %d\n", uncached_mem_numa_node);
    break;
  case MY_DEVICE_IOCTL_SET_HUGE_PAGES:
    use_huge_pages = (arg != 0) ? 1 : 0;
    pr_info("Set huge pages mode to %d\n", use_huge_pages);
    break;
  default:
    return -EINVAL;
  }

  return 0;
}

// Character device file operations
//   http://www.makelinux.net/ldd3/chp-3-sect-3

const struct file_operations device_ops = {.owner = THIS_MODULE,
                                           .open = device_op_open,
                                           .release = device_op_release,
                                           .mmap = device_op_mmap,
                                           .unlocked_ioctl = device_op_ioctl};

static int __init uncached_ram_init(void) {
  printk(KERN_INFO "Uncached ram module loaded\n");

  cdev_major = register_chrdev(0, "uncached_ram", &device_ops);
  if (cdev_major < 0)
    return cdev_major;
  printk(KERN_INFO "Created char device, major: %i\n", cdev_major);

  return 0;
}

static void __exit uncached_ram_exit(void) {
  unregister_chrdev(cdev_major, "uncached_ram");

  printk(KERN_INFO "Uncached ram module unloaded\n");
}

module_init(uncached_ram_init);
module_exit(uncached_ram_exit);
