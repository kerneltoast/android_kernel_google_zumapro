# SPDX-License-Identifier: GPL-2.0
#
# Makefile for GXP driver.
#

GXP_CHIP := CALLISTO
CONFIG_$(GXP_CHIP) ?= m
GCIP_DIR := gcip-kernel-driver/drivers/gcip
CURRENT_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

obj-$(CONFIG_$(GXP_CHIP)) += gxp.o

gxp-objs += \
		gxp-bpm.o \
		gxp-client.o \
		gxp-core-telemetry.o \
		gxp-dci.o \
		gxp-debug-dump.o \
		gxp-dma-fence.o \
		gxp-dma-iommu.o \
		gxp-dmabuf.o \
		gxp-domain-pool.o \
		gxp-doorbell.o \
		gxp-eventfd.o \
		gxp-firmware-data.o \
		gxp-firmware-loader.o \
		gxp-firmware.o \
		gxp-lpm.o \
		gxp-mailbox-manager.o \
		gxp-mailbox.o \
		gxp-mapping.o \
		gxp-mb-notification.o \
		gxp-pm.o \
		gxp-thermal.o \
		gxp-trace.o \
		gxp-vd.o

gxp-mcu-objs := \
		gxp-kci.o \
		gxp-mcu-firmware.o \
		gxp-mcu-fs.o \
		gxp-mcu-platform.o \
		gxp-mcu-telemetry.o \
		gxp-mcu.o \
		gxp-uci.o \
		gxp-usage-stats.o

gsx01-objs := \
		gxp-gsx01-mailbox.o \
		gxp-gsx01-ssmt.o \
		mobile-soc-gsx01.o

ifeq ($(GXP_CHIP),CALLISTO)

gxp-objs += \
		$(gsx01-objs) \
		$(gxp-mcu-objs) \
		callisto-mcu.o \
		callisto-platform.o \
		callisto-pm.o

GMODULE_PATH := $(OUT_DIR)/../private/google-modules
EDGETPU_CHIP := rio

endif

ifeq ($(CONFIG_$(GXP_CHIP)),m)

gxp-objs += $(GCIP_DIR)/gcip.o

endif

KERNEL_SRC ?= /lib/modules/$(shell uname -r)/build
-include $(KERNEL_SRC)/../private/google-modules/soc/gs/Makefile.include
M ?= $(shell pwd)

# Obtain the current git commit hash for logging on probe
GIT_PATH=$(shell cd $(KERNEL_SRC); readlink -e $(M))
GIT_BIN=/usr/bin/git
GIT=$(GIT_BIN) -C $(GIT_PATH)
ifeq ($(shell $(GIT) rev-parse --is-inside-work-tree),true)
        GIT_REPO_STATE=$(shell ($(GIT) --work-tree=$(GIT_PATH) status --porcelain | grep -q .) && echo -dirty)
        ccflags-y       += -DGIT_REPO_TAG=\"$(shell $(GIT) rev-parse --short HEAD)$(GIT_REPO_STATE)\"
endif

# If building via make directly, specify target platform by adding
#     "GXP_PLATFORM=<target>"
# With one of the following values:
#     - SILICON
#     - ZEBU
#     - IP_ZEBU
#     - GEM5
# Defaults to building for SILICON if not otherwise specified.
GXP_PLATFORM ?= SILICON

gxp-flags := -DCONFIG_GXP_$(GXP_PLATFORM) -DCONFIG_$(GXP_CHIP)=1 \
	     -I$(CURRENT_DIR)/include -I$(CURRENT_DIR)/gcip-kernel-driver/include \
	     -I$(KERNEL_SRC)/../private/google-modules/power/mitigation
# TODO(b/336717718): Remove path of embedded IIF
gxp-flags += -I$(CURRENT_DIR)/gcip-kernel-driver/drivers/gcip/iif/include \
	     -I$(KERNEL_SRC)/../private/google-modules/iif/include

ccflags-y += $(EXTRA_CFLAGS) $(gxp-flags)
# Flags needed for external modules.
ccflags-y += -DCONFIG_GOOGLE_BCL

KBUILD_OPTIONS += GXP_CHIP=$(GXP_CHIP) GXP_PLATFORM=$(GXP_PLATFORM)

ifneq ($(OUT_DIR),)
# Access TPU driver's exported symbols.
EXTRA_SYMBOLS += $(GMODULE_PATH)/edgetpu/$(EDGETPU_CHIP)/drivers/edgetpu/Module.symvers
ifneq ($(wildcard $(GMODULE_PATH)/soc/gs/drivers/soc/google/gsa/Module.symvers),)
EXTRA_SYMBOLS += $(GMODULE_PATH)/soc/gs/drivers/soc/google/gsa/Module.symvers
endif

ifneq ($(GXP_POWER_MITIGATION), false)
EXTRA_SYMBOLS += $(GMODULE_PATH)/power/mitigation/Module.symvers
endif

ifneq ($(wildcard $(GMODULE_PATH)/iif/Module.symvers),)
EXTRA_SYMBOLS += $(GMODULE_PATH)/iif/Module.symvers
endif
endif # OUT_DIR

modules modules_install:
	$(MAKE) -C $(KERNEL_SRC) M=$(M)/$(GCIP_DIR) gcip.o
	$(MAKE) -C $(KERNEL_SRC) M=$(M) W=1 $(KBUILD_OPTIONS) \
	EXTRA_CFLAGS="$(EXTRA_CFLAGS)" KBUILD_EXTRA_SYMBOLS="$(EXTRA_SYMBOLS)" $(@)
clean:
	$(MAKE) -C $(KERNEL_SRC) M=$(M)/$(GCIP_DIR) $(@)
	$(MAKE) -C $(KERNEL_SRC) M=$(M) W=1 $(KBUILD_OPTIONS) $(@)
