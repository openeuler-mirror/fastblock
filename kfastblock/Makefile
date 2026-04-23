obj-m := kfastblock.o

KFASTBLOCK_ROOT := $(PWD)
UNAME_R := $(shell uname -r)
HOSTBUILD_KDIR := /root/kernel/.hostbuild
HOSTBUILD_READY := $(shell \
	if [ -f "$(HOSTBUILD_KDIR)/include/config/auto.conf" ]; then \
		printf '%s' y; \
	fi)

ifeq ($(HOSTBUILD_READY),y)
DEFAULT_KDIR := $(HOSTBUILD_KDIR)
else
DEFAULT_KDIR := /lib/modules/$(UNAME_R)/build
endif

kfastblock-objs := \
	src/main.o \
	src/control.o \
	src/meta.o \
	src/request.o \
	src/transport.o \
	src/volume.o

ccflags-y := -I$(src)/include

KDIR ?= $(DEFAULT_KDIR)
PWD := $(shell pwd)

all: modules kfastblock-admin

modules:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

kfastblock-admin:
	$(CC) $(CFLAGS) -o tool/kfastblock-admin tool/kfastblock-admin.c

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f tool/kfastblock-admin

.PHONY: all modules clean kfastblock-admin
