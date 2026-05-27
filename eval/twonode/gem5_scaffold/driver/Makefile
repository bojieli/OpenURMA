# SPDX-License-Identifier: GPL-2.0
# Build uburma.ko against an ARM64 cross-compiled kernel.
#
# Usage:
#   make KDIR=/path/to/linux-source ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-

obj-m := uburma.o

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
