#---------------------------------#
#  Makefile for FignerprintCard   #
#---------------------------------#

obj-$(CONFIG_FPC_FINGERPRINT) += fpc1020_platform_tee.o

KERNEL_SRC ?= /lib/modules/$(shell uname -r)/build
M ?= $(shell pwd)
KBUILD_OPTIONS += CONFIG_FPC_FINGERPRINT=m

modules modules_install clean:
	$(MAKE) -C $(KERNEL_SRC) M=$(M) $(KBUILD_OPTIONS) W=1 $(@)
