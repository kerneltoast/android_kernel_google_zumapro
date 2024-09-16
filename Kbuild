# SPDX-License-Identifier: GPL-2.0-or-later

ifeq ($(wildcard $(srctree)/arch/$(SRCARCH)/include/uapi/asm/a.out.h),)
no-export-headers += linux/a.out.h
endif

ifeq ($(wildcard $(srctree)/arch/$(SRCARCH)/include/uapi/asm/kvm.h),)
no-export-headers += linux/kvm.h
endif

ifeq ($(wildcard $(srctree)/arch/$(SRCARCH)/include/uapi/asm/qbt_handler.h),)
no-export-headers += linux/qbt_handler.h
endif

ifeq ($(wildcard $(srctree)/arch/$(SRCARCH)/include/uapi/asm/kvm_para.h),)
ifeq ($(wildcard $(objtree)/arch/$(SRCARCH)/include/generated/uapi/asm/kvm_para.h),)
no-export-headers += linux/kvm_para.h
endif
endif

ccflags-y += -I$(srctree)/../private/google-modules/fingerprint/qcom/qfs4008

obj-$(CONFIG_QCOM_QBT_HANDLER) = qbt_handler.o
