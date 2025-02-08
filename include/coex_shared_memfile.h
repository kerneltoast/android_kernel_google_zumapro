/*
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

/**
 * @file
 * @brief Definitions for combined memory file hosting contents of multiple memories in coex cpu
 */

#ifndef coex_shared_memfile_h
#define coex_shared_memfile_h

/* ---- Include Files ---------------------------------------------------- */

#include <typedefs.h>

/* ---- Constants and Types ---------------------------------------------- */

#define COEX_COMBINED_FW_MAGIC		0x57465843u	/* 'CXFW', for firmware binary */
#define COEX_COMBINED_DUMP_MAGIC	0x504d4443u	/* 'CDMP', for memory dump */
#define COEX_COMBINED_FW_HDR_VERSION	1u

#define COEX_ITCM_BASE 0x1a000000u
#define COEX_ITCM_SIZE 98304u
#define COEX_DTCM_BASE 0x1a018000u
#define COEX_DTCM_SIZE 24576u

/**
 * @brief Coex firmware tlv header with 32bit length.
 */
typedef struct coex_fw_tlv {
	uint32	id;
	uint32	len;
	uint8	data[];
} coex_fw_tlv_t;

/**
 * @brief Coex firmware payload tlv id.
 */
enum coex_fw_tlv_id {
	COEX_FW_TLV_ITCM = 0u,	/* ITCM payload */
	COEX_FW_TLV_DTCM = 1u,	/* DTCM payload */
};

/**
 * @brief Combined firmware structure to host both ITCM and DTCM contents.
 */
typedef struct coex_combined_fw {
	uint32 magic;		/* COEX_COMBINED_FW_MAGIC or COEX_COMBINED_DUMP_MAGIC */
	uint16 version;		/* version of this header */
	uint16 flags;		/* Reserved 16 bit flags for future usage */
	uint32 len;		/* length of payload not including this header */
	coex_fw_tlv_t tlv[];
} coex_combined_fw_t;

#endif  /* coex_shared_memfile_h */
