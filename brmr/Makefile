# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 IONOS SE

RMR=$(M)/..
KBUILD_EXTRA_SYMBOLS := $(RMR)/Module.symvers

brmr-client-y := brmr-clt.o \
		 brmr-clt-sysfs.o \
		 brmr-clt-reque.o \
		 brmr-clt-stats.o

brmr-server-y := brmr-srv-sysfs.o \
		 brmr-srv.o

obj-$(CONFIG_BRMR_CLIENT) += brmr-client.o
obj-$(CONFIG_BRMR_SERVER) += brmr-server.o
