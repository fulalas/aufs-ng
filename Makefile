ifneq ($(KERNELRELEASE),)

# CONFIG_AUFSNG_FS drives whether this becomes part of vmlinux (=y,
# what a live-boot sequence typically requires - see Kconfig) or a
# loadable module (=m, useful for a quick standalone test build
# against an already-built kernel tree without a full rebuild).
obj-$(CONFIG_AUFSNG_FS) += aufs-ng.o
aufs-ng-y := super.o params.o namei.o dcache.o file.o readdir.o inode.o copy_up.o dir.o dynlayer.o

else

# Out-of-tree convenience build.  The 'default' target below passes no
# CONFIG_ value, so it follows the target kernel's own .config: a .ko
# when CONFIG_AUFSNG_FS=m there, nothing when it's =y, since it's
# already part of that vmlinux.  Setting CONFIG_AUFSNG_FS=m on the make
# command line overrides the .config either way - that is what the
# README's one-liner does, and it works against a =y tree too.
KDIR ?= /lib/modules/$(shell uname -r)/build

default:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean

endif
