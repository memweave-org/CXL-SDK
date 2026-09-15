obj-m += uncached_ram.o

KDIR ?= /lib/modules/$(shell uname -r)/build
MODULE_DIR := $(CURDIR)

.PHONY: all clean install
all:
	$(MAKE) -C $(KDIR) M=$(MODULE_DIR) modules
clean:
	$(MAKE) -C $(KDIR) M=$(MODULE_DIR) clean
install:
	sudo insmod $(MODULE_DIR)/uncached_ram.ko

# Create a device node with mount.sh after loading the module.
