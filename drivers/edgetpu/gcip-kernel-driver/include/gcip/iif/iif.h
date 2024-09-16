/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Defines the interface of the IIF driver.
 *
 * Copyright (C) 2023 Google LLC
 */

#ifndef __IIF_IIF_H__
#define __IIF_IIF_H__

#include <linux/ioctl.h>
#include <linux/types.h>

/* Interface Version. */
#define IIF_INTERFACE_VERSION_MAJOR 1
#define IIF_INTERFACE_VERSION_MINOR 0

#define IIF_IOCTL_BASE 'i'

/* The ioctl number for the fence FDs will start from here. */
#define IIF_FENCE_IOCTL_NUM_BASE 0x80

/*
 * The max number of fences can be created per IP.
 * Increasing this value needs to increase the size of fence table.
 */
#define IIF_NUM_FENCES_PER_IP 1024

/*
 * Type of IPs.
 *
 * The order of IP must be matched with the firmware side because the fence ID will be assigned
 * according to the IP type.
 */
enum iif_ip_type {
	IIF_IP_DSP,
	IIF_IP_TPU,
	IIF_IP_GPU,
	IIF_IP_NUM,

	/* Reserve the number of IP type to expand the fence table easily in the future. */
	IIF_IP_RESERVED = 16,
};

/*
 * ioctls for /dev/iif.
 * TODO(b/312161537): introduce ioctls once we have a standalone IIF driver.
 */

struct iif_fence_get_information_ioctl {
	/* The type of the signaler IP. (enum iif_ip_type) */
	__u8 signaler_ip;
	/* The number of total signalers. */
	__u16 total_signalers;
	/* The number of submitted signalers. */
	__u16 submitted_signalers;
	/* The number of signaled signalers. */
	__u16 signaled_signalers;
	/* The number of outstanding waiters. */
	__u16 outstanding_waiters;
	/* Reserved. */
	__u8 reserved[7];
};

/*
 * ioctls for inter-IP fence FDs.
 */

/* Returns the fence information. */
#define IIF_FENCE_GET_INFORMATION \
	_IOR(IIF_IOCTL_BASE, IIF_FENCE_IOCTL_NUM_BASE, struct iif_fence_get_information_ioctl)

#endif /* __IIF_IIF_H__ */
