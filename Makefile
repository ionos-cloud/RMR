# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 IONOS SE

# Called as kbuild
ifneq ($(KERNELRELEASE),)

ifdef PKG_VERSION
DEFINES += -D RMR_VER_STRING=\"$(PKG_VERSION)\"
DEFINES += -D BRMR_VER_STRING=\"$(PKG_VERSION)\"
KBUILD_CFLAGS += $(DEFINES)
endif

# brmr needs rmr public headers; rmr-srv.h pulls in brmr-srv.h
KBUILD_CFLAGS += -I$(src)/rmr
KBUILD_CFLAGS += -I$(src)/brmr
KBUILD_CFLAGS += -I$(srctree)/drivers/infiniband/ulp/rtrs/

export CONFIG_RMR_CLIENT := m
export CONFIG_RMR_SERVER := m
export CONFIG_BRMR_CLIENT := m
export CONFIG_BRMR_SERVER := m

obj-m += rmr/
obj-m += brmr/

# Normal Makefile, redirect to kbuild
else

KDIR ?= /lib/modules/$(shell uname -r)/build

.PHONY: all
all:
	@$(MAKE) -C $(KDIR) M=$(CURDIR) modules

%:
	@$(MAKE) -C $(KDIR) M=$(CURDIR) $@

endif
