obj-m := kfastblock.o

KFASTBLOCK_ROOT := $(PWD)

kfastblock-objs := \
	src/main.o \
	src/control.o \
	src/meta.o \
	src/request.o \
	src/transport.o \
	src/volume.o

ccflags-y := -I$(src)/include

KDIR ?= /lib/modules/$(shell uname -r)/build
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
