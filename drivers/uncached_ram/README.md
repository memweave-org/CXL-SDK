# Experimental RAM mapping driver

This optional Linux kernel module allocates RAM and exposes it through a
character device for userspace `mmap`. Despite the `uncached_ram` name, the
current implementation uses write-combining mappings (`set_memory_wc` and
`pgprot_writecombine`). It is experiment infrastructure, not a test case or a
required SDK runtime dependency.

The module and its loading script previously lived in `tests/basic/kmodule/`.

## Build

Use a Linux machine with matching kernel development headers and the x86 memory
attribute APIs used by the module. From the repository root:

```bash
make -C drivers/uncached_ram
# Optionally select another configured kernel build tree:
make -C drivers/uncached_ram KDIR=/path/to/kernel/build
make -C drivers/uncached_ram clean
```

## Load and create the device

On the experiment host, review and run:

```bash
./drivers/uncached_ram/mount.sh /dev/uncached_mem_dev
```

The script builds and loads the module, replaces the requested device node, and
sets its permissions to `666`. It requires `sudo`; the default device path is
`/dev/uncached_mem_dev`. It can be invoked from any working directory.

This relocation does not validate or repair the experimental allocation and
mapping implementation. In particular, the existing huge-page path uses
`HUGE_PAGE_ORDER_1GB = 30`; that is inconsistent with a 1 GiB allocation on
4 KiB pages. Kernel compilation and runtime validation on the target Linux
kernel are still required before using this driver.

## Origin

The source identifies `lemonsqueeze/uncached_ram_lkm` as its upstream and declares
`MODULE_LICENSE("GPL")`. See the attribution in `uncached_ram.c`.
