/*
 * Misc utility routines used by kernel or app-level.
 * Contents are wifi-specific, used by any kernel or app-level
 * software that might want wifi things as it grows.
 *
 * Copyright (C) 2024, Broadcom.
 *
 *      Unless you and Broadcom execute a separate written software license
 * agreement governing use of this software, this software is licensed to you
 * under the terms of the GNU General Public License version 2 (the "GPL"),
 * available at http://www.broadcom.com/licenses/GPLv2.php, with the
 * following added to such license:
 *
 *      As a special exception, the copyright holders of this software give you
 * permission to link this software with independent modules, and to copy and
 * distribute the resulting executable under terms of your choice, provided that
 * you also meet, for each linked independent module, the terms and conditions of
 * the license of that module.  An independent module is a module which is not
 * derived from this software.  The special exception does not apply to any
 * modifications of the software.
 *
 *
 * <<Broadcom-WL-IPTag/Dual:>>
 */

#include <typedefs.h>
#include <bcmutils.h>
#include <bcmwifi_channels.h>

/* 320Mhz Chan Id to Center Channel map */
static const uint8 BCMPOST_TRAP_RODATA(map_320m_chanid_cc)[] = {
	31,    /* CC 31 */
	95,    /* CC 63 */
	159,   /* CC 95 */
	0,     /* INVALCHAN */
	63,    /* CC 127 */
	127,   /* CC 159 */
	191,   /* CC 191 */
};

/**
 * This function returns the the 6GHz 320MHz center channel for the given chanspec 320MHz ID
 *
 * @param    chan_320MHz_id    320MHz chanspec ID
 *
 * @return   Return the center channel number, or 0 on error.
 *
 */
uint8
BCMPOSTTRAPFN(wf_chspec_6G_id320_to_ch)(uint8 chan_320MHz_id)
{
	uint8 ch = 0;

	if (chan_320MHz_id <= WF_NUM_6G_320M_CHAN_ID_MAX) {
		/* The 6GHz center channels have a spacing of 64
		 * starting from the first 320MHz center
		 */
		if (chan_320MHz_id < ARRAYSIZE(map_320m_chanid_cc)) {
			return map_320m_chanid_cc[chan_320MHz_id];
		}
	}

	return ch;
}

/* Retrive the chan_id and convert it to center channel */
uint8
BCMPOSTTRAPFN(wf_chspec_320_id2cch)(chanspec_t chanspec)
{
	if (CHSPEC_BAND(chanspec) == WL_CHANSPEC_BAND_6G &&
	    CHSPEC_BW(chanspec) == WL_CHANSPEC_BW_320) {
		uint8 ch_id = WL_CHSPEC_320_CHAN(chanspec);

		return wf_chspec_6G_id320_to_ch(ch_id);
	}
	return 0;
}

#ifdef BCMWIFI_BW320MHZ
/*
 * Returns center channel for a contiguous chanspec and
 * INVCHANNEL for non-contiguous chanspec.
 */
uint8
BCMPOSTTRAPFN(wf_chspec_center_channel)(chanspec_t chspec)
{
	uint8 cc;
	if (CHSPEC_IS8080(chspec)) {
		cc = INVCHANNEL;
	} else {
		cc = CHSPEC_IS320(chspec) ? wf_chspec_320_id2cch(chspec):
			CHSPEC_CHANNEL(chspec);
	}
	return cc;
}
#endif /* BCMWIFI_BW320MHZ */
