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

/* The maximum number of fences can be passed to one ioctl request. */
#define IIF_MAX_NUM_FENCES 64

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
	IIF_IP_AP,
	IIF_IP_NUM,

	/* Reserve the number of IP type to expand the fence table easily in the future. */
	IIF_IP_RESERVED = 16,
};

/*
 * ioctls for /dev/iif.
 */

struct iif_create_fence_ioctl {
	/*
	 * Input:
	 * The type of the fence signaler IP. (See enum iif_ip_type)
	 */
	__u8 signaler_ip;
	/*
	 * Input:
	 * The number of the signalers.
	 */
	__u16 total_signalers;
	/*
	 * Output:
	 * The file descriptor of the created fence.
	 */
	__s32 fence;
};

/* Create an IIF fence. */
#define IIF_CREATE_FENCE _IOWR(IIF_IOCTL_BASE, 0, struct iif_create_fence_ioctl)

/*
 * The ioctl won't register @eventfd and will simply return the number of
 * remaining signalers of each fence.
 */
#define IIF_FENCE_REMAINING_SIGNALERS_NO_REGISTER_EVENTFD (~0u)

struct iif_fence_remaining_signalers_ioctl {
	/*
	 * Input:
	 * User-space pointer to an int array of inter-IP fence file descriptors
	 * to check whether there are remaining signalers to be submitted or
	 * not.
	 */
	__u64 fences;
	/*
	 * Input:
	 * The number of fences in `fence_array`.
	 * If > IIF_MAX_NUM_FENCES, the ioctl will fail with errno == EINVAL.
	 */
	__u32 fences_count;
	/*
	 * Input:
	 * The eventfd which will be triggered if there were fence(s) which
	 * haven't finished the signaler submission yet when the ioctl is called
	 * and when they eventually have finished the submission. Note that if
	 * all fences already finished the submission (i.e., all values in the
	 * returned @remaining_signalers are 0), this eventfd will be ignored.
	 *
	 * Note that if `IIF_FENCE_REMAINING_SIGNALERS_NO_REGISTER_EVENTFD` is
	 * passed, this ioctl will simply return the number of remaining
	 * signalers of each fence to @remaining_signalers.
	 */
	__u32 eventfd;
	/*
	 * Output:
	 * User-space pointer to an int array where the driver will write the
	 * number of remaining signalers to be submitted per fence. The order
	 * will be the same with @fences.
	 */
	__u64 remaining_signalers;
};

/*
 * Check whether there are remaining signalers to be submitted to fences.
 * If all signalers have been submitted, the runtime is expected to send waiter
 * commands right away. Otherwise, it will listen the eventfd to wait signaler
 * submission to be finished.
 */
#define IIF_FENCE_REMAINING_SIGNALERS \
	_IOWR(IIF_IOCTL_BASE, 1, struct iif_fence_remaining_signalers_ioctl)

/*
 * ioctls for inter-IP fence FDs.
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

/* Returns the fence information. */
#define IIF_FENCE_GET_INFORMATION \
	_IOR(IIF_IOCTL_BASE, IIF_FENCE_IOCTL_NUM_BASE, struct iif_fence_get_information_ioctl)

#endif /* __IIF_IIF_H__ */
