KERNEL_SRC ?= /lib/modules/$(shell uname -r)/build
M ?= $(shell pwd)

EXTRA_CFLAGS	+= -I$(KERNEL_SRC)/../google-modules/fingerprint/qcom/qfs4008
EXTRA_CFLAGS	+= -I$(KERNEL_SRC)/../google-modules/fingerprint/qcom/qfs4008
EXTRA_CFLAGS	+= -I$(KERNEL_SRC)/../google-modules/gs/soc/include

EXTRA_SYMBOLS	+= $(OUT_DIR)/../private/google-modules/touch/common/Module.symvers

modules modules_install clean:
	$(MAKE) -C $(KERNEL_SRC) M=$(M) \
	EXTRA_CFLAGS="$(EXTRA_CFLAGS)" \
	KBUILD_EXTRA_SYMBOLS="$(EXTRA_SYMBOLS)" \
	$(@)
