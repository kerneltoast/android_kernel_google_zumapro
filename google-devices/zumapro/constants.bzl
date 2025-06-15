# SPDX-License-Identifier: GPL-2.0-only

"""
ZUMAPRO constants.
"""

ZUMAPRO_DTBS = [
    # keep sorted
    "zumapro-a0-foplp.dtb",
    "zumapro-a0-ipop.dtb",
    "zumapro-a1-foplp.dtb",
    "zumapro-a1-ipop.dtb",
]

ZUMAPRO_DPM_DTBOS = [
    "zumapro-dpm-eng.dtbo",
    "zumapro-dpm-userdebug.dtbo",
    "zumapro-dpm-user.dtbo",
]

ZUMAPRO_MODULE_OUTS = [
    # keep sorted
    "drivers/gpu/drm/display/drm_display_helper.ko",
    "drivers/i2c/i2c-dev.ko",
    "drivers/misc/eeprom/at24.ko",
    "drivers/misc/open-dice.ko",
    "drivers/perf/arm-cmn.ko",
    "drivers/perf/arm_dsu_pmu.ko",
    "drivers/scsi/sg.ko",
    "drivers/spi/spidev.ko",
    "drivers/watchdog/softdog.ko",
    "net/mac80211/mac80211.ko",
    "net/wireless/cfg80211.ko",
]
