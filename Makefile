ifneq ($(KERNELRELEASE),)

# CONFIG_AUFSNG_FS drives whether this becomes part of vmlinux (=y,
# what PorteuX's own live-boot sequence requires - see Kconfig) or a
# loadable module (=m, useful for a quick standalone test build
# against an already-built kernel tree without a full rebuild).
obj-$(CONFIG_AUFSNG_FS) += aufs-ng.o
aufs-ng-y := super.o params.o namei.o dcache.o file.o readdir.o inode.o copy_up.o dir.o dynlayer.o

else

# Out-of-tree convenience build: only produces a .ko if the target
# kernel's own .config has CONFIG_AUFSNG_FS=m; produces nothing (by
# design) if it's =y there, since it's already part of that vmlinux.
KDIR ?= /lib/modules/$(shell uname -r)/build

default:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean

endif
