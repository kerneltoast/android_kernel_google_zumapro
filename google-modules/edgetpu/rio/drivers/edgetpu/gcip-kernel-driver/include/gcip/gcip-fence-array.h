/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Interface for the array of abstracted fences.
 *
 * Copyright (C) 2023 Google LLC
 */

#ifndef __GCIP_FENCE_ARRAY_H__
#define __GCIP_FENCE_ARRAY_H__

#include <linux/kref.h>

#include <gcip/gcip-fence.h>

/*
 * Contains multiple fences.
 *
 * This structure must be created by calling the `gcip_fence_array_create` function.
 */
struct gcip_fence_array {
	/* Fences. */
	struct gcip_fence **fences;
	/* The number of fences. */
	int size;
	/* Refcount. */
	struct kref kref;
	/* The type of fences in the array. Only available when @same_type is true. */
	enum gcip_fence_type type;
	/* True if the fences are the same type. */
	bool same_type;
};

/*
 * Gets the fence objects from fence FD array, @fences. If @check_same_type is true, it will check
 * whether the fence type is all the same or not. If not, it will return -EINVAL error pointer.
 *
 * Returns `struct gcip_fence_array` instance which contains the fence objects. Otherwise, returns
 * an errno pointer.
 *
 * Note that the returned instance will be released when its refcount becomes 0.
 */
struct gcip_fence_array *gcip_fence_array_create(int *fences, int num_fences, bool check_same_type);

/* Increments the refcount of @fence_array. */
struct gcip_fence_array *gcip_fence_array_get(struct gcip_fence_array *fence_array);

/*
 * Decrements the refcount of @fence_array. If it becomes 0, it will release the refcount of the
 * fences which it is referring to.
 */
void gcip_fence_array_put(struct gcip_fence_array *fence_array);

/* Signals the fences in @fence_array. */
void gcip_fence_array_signal(struct gcip_fence_array *fence_array, int errno);

/*
 * Notifies the fences in @fence_array that a command which waited on them has finished their work.
 *
 * This function is only meaningful when a fence is GCIP_INTER_IP_FENCE.
 */
void gcip_fence_array_waited(struct gcip_fence_array *fence_array);

/* Submits a signaler to the fences in @fence_array. */
void gcip_fence_array_submit_signaler(struct gcip_fence_array *fence_array);

/* Submits a waiter to the fences in @fence_array. */
void gcip_fence_array_submit_waiter(struct gcip_fence_array *fence_array);

/*
 * Submits a waiter to each fence in @in_fences and a signaler to each fence in @out_fences. Either
 * @in_fences or @out_fences is allowed to be NULL.
 *
 * For the waiter submission, if at least one fence of @in_fences haven't finished the signaler
 * submission, this function will fail and return -EAGAIN.
 *
 * For the signaler submission, if at least one fence of @out_fences have already finished the
 * signaler submission, this function will fail and -EPERM.
 *
 * This function will be useful when the caller wants to accomplish the waiter submission and the
 * signaler submission atomically. Also, it can be called in the IRQ context.
 *
 * Otherwise, returns 0 on success.
 */
int gcip_fence_array_submit_waiter_and_signaler(struct gcip_fence_array *in_fences,
						struct gcip_fence_array *out_fences);

/*
 * Allocates and returns the array of inter-IP fence IDs. The number of IIFs in @fence_array will
 * be returned to @num_iif.
 *
 * If @out_fences is true, it will check whether the signaler IP type of all parsed IIFs are the
 * same with @signaler_ip. If @out_fences is false, @signaler_ip will be ignored.
 *
 * Note that the caller must free the returned array using kfree.
 *
 * Returns an array pointer if there was at least one IIF in the array, returns NULL if there was
 * no IIF. Otherwise, returns a negative errno pointer.
 */
uint16_t *gcip_fence_array_get_iif_id(struct gcip_fence_array *fence_array, int *num_iif,
				      bool out_fences, enum iif_ip_type signaler_ip);

/*
 * Its functionality is the same with the `gcip_fence_wait_signaler_submission` function, but
 * receives a `struct gcip_fence_array` instance.
 *
 * See the `gcip_fence_wait_signaler_submission` function for details.
 */
int gcip_fence_array_wait_signaler_submission(struct gcip_fence_array *fence_array,
					      unsigned int eventfd, int *remaining_signalers);

#endif /* __GCIP_FENCE_ARRAY_H__ */
