obj-m := kfastblock.o

KFASTBLOCK_ROOT := $(PWD)
UNAME_R := $(shell uname -r)
HOSTBUILD_KDIR := /root/kernel/.hostbuild
HOSTBUILD_UTS_HDR := $(HOSTBUILD_KDIR)/include/generated/utsrelease.h
HOSTBUILD_READY := $(shell \
	if [ -f "$(HOSTBUILD_KDIR)/include/config/auto.conf" ] && \
	   [ -f "$(HOSTBUILD_UTS_HDR)" ]; then \
		printf '%s' y; \
	fi)
HOSTBUILD_UTS_RELEASE := $(shell \
	if [ -f "$(HOSTBUILD_UTS_HDR)" ]; then \
		sed -n 's/^#define UTS_RELEASE "\\(.*\\)"/\\1/p' \
			"$(HOSTBUILD_UTS_HDR)"; \
	fi)

ifeq ($(HOSTBUILD_READY),y)
DEFAULT_KDIR := $(HOSTBUILD_KDIR)
else
DEFAULT_KDIR := /lib/modules/$(UNAME_R)/build
endif

kfastblock-objs := \
	src/main.o \
	src/control.o \
	src/buffer.o \
	src/meta.o \
	src/request.o \
	src/scheduler.o \
	src/transport.o \
	src/volume.o

ccflags-y := -I$(src)/include

KDIR ?= $(DEFAULT_KDIR)
PWD := $(shell pwd)

all: modules kfastblock-admin kfastblock-rawctl

modules:
	@if [ "$(KDIR)" = "$(HOSTBUILD_KDIR)" ] && \
	    [ -n "$(HOSTBUILD_UTS_RELEASE)" ] && \
	    [ "$(HOSTBUILD_UTS_RELEASE)" != "$(UNAME_R)" ]; then \
		printf '%s\n' \
		  "warning: $(HOSTBUILD_KDIR) UTS_RELEASE=$(HOSTBUILD_UTS_RELEASE) != running kernel $(UNAME_R)" >&2; \
		printf '%s\n' \
		  "warning: kfastblock.ko may build but fail to load; override KDIR if you have a matching tree" >&2; \
	fi
	$(MAKE) -C $(KDIR) M=$(PWD) modules

kfastblock-admin:
	$(CC) $(CFLAGS) -o tool/kfastblock-admin tool/kfastblock-admin.c

kfastblock-rawctl:
	$(CC) $(CFLAGS) -o tool/kfastblock-rawctl tool/kfastblock-rawctl.c

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f tool/kfastblock-admin tool/kfastblock-rawctl

.PHONY: all modules clean kfastblock-admin kfastblock-rawctl
