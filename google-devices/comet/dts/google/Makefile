# SPDX-License-Identifier: GPL-2.0

# zuma dtbo
zuma_comet_overlays := \
	zuma-comet-dev1.dtbo \
	zuma-comet-proto1-samsung-gnss.dtbo \
	zuma-comet-proto1-broadcom-gnss.dtbo \
	zuma-comet-proto1_1-samsung-gnss.dtbo \
	zuma-comet-proto1_1-broadcom-gnss.dtbo


# zuma dtb
zuma_comet_dtbs := \
	google-dts-zuma/zuma-a0-foplp.dtb \
	google-dts-zuma/zuma-a0-ipop.dtb \
	google-dts-zuma/zuma-b0-foplp.dtb \
	google-dts-zuma/zuma-b0-ipop.dtb

DTC_FLAGS_zuma-dpm-eng += -a 4096
DTC_FLAGS_zuma-dpm-user += -a 4096
DTC_FLAGS_zuma-dpm-userdebug += -a 4096

# zumapro dtbo
zumapro_comet_overlays := \
	zumapro-comet-evt1-samsung-gnss.dtbo \
	zumapro-comet-evt1-broadcom-gnss.dtbo \
	zumapro-comet-evt1-samsung-gnss-wingboard.dtbo \
	zumapro-comet-evt1-samsung-gnss-6layerflex.dtbo \
	zumapro-comet-evt1-samsung-gnss-sdc.dtbo \
	zumapro-comet-evt1-broadcom-gnss-sdc.dtbo \
	zumapro-comet-evt1_1.dtbo \
	zumapro-comet-dvt1.dtbo \
	zumapro-comet-pvt1.dtbo \
	zumapro-comet-mp.dtbo

# zumapro dtb
zumapro_comet_dtbs := \
	google-dts-zumapro/zumapro-a0-foplp.dtb \
	google-dts-zumapro/zumapro-a0-ipop.dtb \
	google-dts-zumapro/zumapro-a1-foplp.dtb \
	google-dts-zumapro/zumapro-a1-ipop.dtb

DTC_FLAGS_zumapro-dpm-eng += -a 4096
DTC_FLAGS_zumapro-dpm-user += -a 4096
DTC_FLAGS_zumapro-dpm-userdebug += -a 4096


# Usage:
#   $(call DTBS_overlay_template, overlay, base_dtb, suffix)
#
# This is a generic template to create the following construct:
#   <overlay>-<suffix>-dtbs := <base_dtb>.dtb <overlay>.dtbo
define DTBS_overlay_template
	$(1:%.dtbo=%-$(strip $3)-dtbs) := $2 $1
endef

# Usage:
#   $(call DTB_object_template, CONFIG_, overlay, suffix)
#
# This is a generic template to create the following construct:
#   dtb-$(CONFIG_) += <overlay>-<suffix>.dtb
define DTB_object_template
	dtb-$(strip $1) += $(2:%.dtbo=%-$(strip $3).dtb)
endef

# Usage:
#   $(call multi_dtbs_overlay, list_of_base_dtbs, list_of_overlays, CONFIG_)
#
# For each (base_dtb,overlay) pair, this will create the following constructs:
#   <overlay>-<base_dtb>-dtbs := <base_dtb>.dtb <overlay>.dtbo
#   dtb-$(CONFIG_) += <overlay>-<base_dtb>.dtb
define multi_dtbs_overlay
	$(foreach base, $1, \
		$(foreach overlay, $2, \
			$(eval $(call DTBS_overlay_template, $(overlay), $(base), $(notdir $(base:%.dtb=%)))) \
			$(eval $(call DTB_object_template, $3, $(overlay), $(notdir $(base:%.dtb=%))))))
endef

# Create the -dtbs and dtb-y constructs. We can do this by looping through the
# base dtbs and apply each overlay to it. Kbuild will use these -dtbs
# constructs to determine which dtbs are the base dtbs and which are the
# overlays. For the base dtbs, kbuild will expose the symbols as needed for the
# overlays. Additionally, kbuild will statically apply the overlay to the base
# dtb which could be used to verify the overlay applies cleanly.
$(call multi_dtbs_overlay, $(zuma_comet_dtbs), $(zuma_comet_overlays), y)
$(call multi_dtbs_overlay, $(zumapro_comet_dtbs), $(zumapro_comet_overlays), y)
