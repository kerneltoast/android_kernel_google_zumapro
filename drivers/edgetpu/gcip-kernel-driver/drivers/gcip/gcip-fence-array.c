// SPDX-License-Identifier: GPL-2.0-only
/*
 * Interface for the array of abstracted fences.
 *
 * Copyright (C) 2023 Google LLC
 */

#include <linux/kref.h>
#include <linux/slab.h>

#include <gcip/gcip-fence-array.h>
#include <gcip/gcip-fence.h>

/*
 * Holds the spin locks which protect the number of signalers of each fence in @fence_array.
 *
 * The caller must use the `gcip_fence_array_submitted_signalers_unlock` function to release the
 * locks.
 */
static void gcip_fence_array_submitted_signalers_lock(struct gcip_fence_array *fence_array)
{
	int i;

	if (!fence_array || fence_array->size <= 0)
		return;

	for (i = 0; i < fence_array->size; i++)
		gcip_fence_submitted_signalers_lock(fence_array->fences[i]);
}

/* Releases the spin locks held by the `gcip_fence_array_submitted_signalers_lock` function. */
static void gcip_fence_array_submitted_signalers_unlock(struct gcip_fence_array *fence_array)
{
	int i;

	if (!fence_array || fence_array->size <= 0)
		return;

	for (i = fence_array->size - 1; i >= 0; i--)
		gcip_fence_submitted_signalers_unlock(fence_array->fences[i]);
}

struct gcip_fence_array *gcip_fence_array_create(int *fences, int num_fences, bool check_same_type)
{
	int i, ret;
	struct gcip_fence_array *fence_array;
	struct gcip_fence *fence;

	if ((!fences && num_fences) || num_fences < 0)
		return ERR_PTR(-EINVAL);

	fence_array = kzalloc(sizeof(*fence_array), GFP_KERNEL);
	if (!fence_array)
		return ERR_PTR(-ENOMEM);

	fence_array->fences = kcalloc(num_fences, sizeof(*fence_array->fences), GFP_KERNEL);
	if (!fence_array->fences) {
		ret = -ENOMEM;
		goto err_free_fence_array;
	}

	fence_array->same_type = true;

	for (i = 0; i < num_fences; i++) {
		fence = gcip_fence_fdget(fences[i]);
		if (IS_ERR(fence)) {
			ret = PTR_ERR(fence);
			goto err_put_fences;
		}

		if (i && fence_array->same_type && fence->type != fence_array->fences[0]->type) {
			/* Check whether all fences are the same type. */
			if (check_same_type) {
				ret = -EINVAL;
				gcip_fence_put(fence);
				goto err_put_fences;
			}
			fence_array->same_type = false;
		}

		fence_array->fences[i] = fence;
	}

	if (i && fence_array->same_type)
		fence_array->type = fence_array->fences[0]->type;

	fence_array->size = i;
	kref_init(&fence_array->kref);

	return fence_array;

err_put_fences:
	while (i--)
		gcip_fence_put(fence_array->fences[i]);
	kfree(fence_array->fences);
err_free_fence_array:
	kfree(fence_array);

	return ERR_PTR(ret);
}

static void gcip_fence_array_release(struct kref *kref)
{
	struct gcip_fence_array *fence_array = container_of(kref, struct gcip_fence_array, kref);
	int i;

	for (i = 0; i < fence_array->size; i++)
		gcip_fence_put(fence_array->fences[i]);
	kfree(fence_array->fences);
	kfree(fence_array);
}

struct gcip_fence_array *gcip_fence_array_get(struct gcip_fence_array *fence_array)
{
	if (!fence_array)
		return NULL;
	kref_get(&fence_array->kref);
	return fence_array;
}

void gcip_fence_array_put(struct gcip_fence_array *fence_array)
{
	if (fence_array)
		kref_put(&fence_array->kref, gcip_fence_array_release);
}

void gcip_fence_array_signal(struct gcip_fence_array *fence_array, int errno)
{
	int i;

	if (!fence_array)
		return;

	for (i = 0; i < fence_array->size; i++)
		gcip_fence_signal(fence_array->fences[i], errno);
}

void gcip_fence_array_waited(struct gcip_fence_array *fence_array)
{
	int i;

	if (!fence_array)
		return;

	for (i = 0; i < fence_array->size; i++)
		gcip_fence_waited(fence_array->fences[i]);
}

void gcip_fence_array_submit_signaler(struct gcip_fence_array *fence_array)
{
	int i;

	if (!fence_array)
		return;

	for (i = 0; i < fence_array->size; i++)
		gcip_fence_submit_signaler(fence_array->fences[i]);
}

void gcip_fence_array_submit_waiter(struct gcip_fence_array *fence_array)
{
	int i;

	if (!fence_array)
		return;

	for (i = 0; i < fence_array->size; i++)
		gcip_fence_submit_waiter(fence_array->fences[i]);
}

int gcip_fence_array_submit_waiter_and_signaler(struct gcip_fence_array *in_fences,
						struct gcip_fence_array *out_fences)
{
	int i;

	gcip_fence_array_submitted_signalers_lock(in_fences);

	/* Checks whether we can submit a waiter to @in_fences. */
	for (i = 0; in_fences && i < in_fences->size; i++) {
		if (!gcip_fence_is_waiter_submittable_locked(in_fences->fences[i])) {
			gcip_fence_array_submitted_signalers_unlock(in_fences);
			return -EAGAIN;
		}
	}

	/*
	 * We can release the lock of @in_fences because once they are able to submit a waiter, it
	 * means that all signalers have been submitted to @in_fences and the fact won't be changed.
	 * Will submit a waiter to @in_fences if @out_fences are able to submit a signaler.
	 */
	gcip_fence_array_submitted_signalers_unlock(in_fences);
	gcip_fence_array_submitted_signalers_lock(out_fences);

	/* Checks whether we can submit a signaler to @out_fences. */
	for (i = 0; out_fences && i < out_fences->size; i++) {
		if (!gcip_fence_is_signaler_submittable_locked(out_fences->fences[i])) {
			gcip_fence_array_submitted_signalers_unlock(out_fences);
			return -EPERM;
		}
	}

	/* Submits a signaler to @out_fences. */
	for (i = 0; out_fences && i < out_fences->size; i++)
		gcip_fence_submit_signaler_locked(out_fences->fences[i]);

	gcip_fence_array_submitted_signalers_unlock(out_fences);

	/* Submits a waiter to @in_fences. */
	for (i = 0; in_fences && i < in_fences->size; i++)
		gcip_fence_submit_waiter(in_fences->fences[i]);

	return 0;
}

uint16_t *gcip_fence_array_get_iif_id(struct gcip_fence_array *fence_array, int *num_iif,
				      bool out_fences, enum iif_ip_type signaler_ip)
{
	uint16_t *iif_fences;
	struct iif_fence *iif;
	int i, j;

	*num_iif = 0;

	if (!fence_array)
		return NULL;

	for (i = 0; i < fence_array->size; i++) {
		if (fence_array->fences[i]->type == GCIP_INTER_IP_FENCE) {
			iif = fence_array->fences[i]->fence.iif;
			if (out_fences && iif->signaler_ip != signaler_ip) {
				*num_iif = 0;
				return ERR_PTR(-EINVAL);
			}
			(*num_iif)++;
		}
	}

	if (!(*num_iif))
		return NULL;

	iif_fences = kcalloc(*num_iif, sizeof(*iif_fences), GFP_KERNEL);
	if (!iif_fences)
		return ERR_PTR(-ENOMEM);

	for (i = 0, j = 0; i < fence_array->size; i++) {
		if (fence_array->fences[i]->type == GCIP_INTER_IP_FENCE)
			iif_fences[j++] = gcip_fence_get_iif_id(fence_array->fences[i]);
	}

	return iif_fences;
}

int gcip_fence_array_wait_signaler_submission(struct gcip_fence_array *fence_array,
					      unsigned int eventfd, int *remaining_signalers)
{
	return gcip_fence_wait_signaler_submission(fence_array->fences, fence_array->size, eventfd,
						   remaining_signalers);
}
