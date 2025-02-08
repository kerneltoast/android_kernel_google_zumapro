// SPDX-License-Identifier: GPL-2.0-only
/*
 * GCIP Mailbox Ops for the in-kernel VII mailbox
 *
 * Copyright (C) 2024 Google LLC
 */

#include <linux/atomic.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>

#include <gcip/gcip-mailbox.h>

#include "edgetpu-ikv-mailbox-ops.h"
#include "edgetpu-ikv.h"
#include "edgetpu-mailbox.h"

/*
 * Helper function with shared logic needed for notifying awaiters when a command has a response
 * ready or is being flushed.
 */
static void signal_response_waiters(struct edgetpu_ikv_response *resp, int error, bool notify_group)
{
	/* Refund the credit before notifying any waiters in case they send another command. */
	if (resp->group_to_notify)
		atomic_inc(&resp->group_to_notify->available_vii_credits);

	/*
	 * Signal dma_fences before notifying the group of the response.
	 * It is likely the user-space client that owns the group will need any drivers waiting on
	 * the command to finish their work before user-space can make use of the results.
	 */
	if (resp->out_fence) {
		gcip_signal_dma_fence_with_status(resp->out_fence, error, true);
		dma_fence_put(resp->out_fence);
	}

	if (resp->group_to_notify && notify_group)
		edgetpu_group_notify(resp->group_to_notify, EDGETPU_EVENT_RESPDATA);
}

/* In-Kernel VII gcip_mailbox_ops */

static u32 edgetpu_ikv_get_cmd_queue_head(struct gcip_mailbox *mailbox)
{
	struct edgetpu_ikv *ikv = gcip_mailbox_get_data(mailbox);
	struct edgetpu_mailbox *mbx_hw = ikv->mbx_hardware;

	return EDGETPU_MAILBOX_CMD_QUEUE_READ(mbx_hw, head);
}

static u32 edgetpu_ikv_get_cmd_queue_tail(struct gcip_mailbox *mailbox)
{
	struct edgetpu_ikv *ikv = gcip_mailbox_get_data(mailbox);
	struct edgetpu_mailbox *mbx_hw = ikv->mbx_hardware;

	return mbx_hw->cmd_queue_tail;
}

static void edgetpu_ikv_inc_cmd_queue_tail(struct gcip_mailbox *mailbox, u32 inc)
{
	struct edgetpu_ikv *ikv = gcip_mailbox_get_data(mailbox);
	struct edgetpu_mailbox *mbx_hw = ikv->mbx_hardware;

	edgetpu_mailbox_inc_cmd_queue_tail(mbx_hw, inc);
}

static int edgetpu_ikv_acquire_cmd_queue_lock(struct gcip_mailbox *mailbox, bool try, bool *atomic)
{
	struct edgetpu_ikv *ikv = gcip_mailbox_get_data(mailbox);

	*atomic = false;
	mutex_lock(&ikv->cmd_queue_lock);
	return 1;
}

static void edgetpu_ikv_release_cmd_queue_lock(struct gcip_mailbox *mailbox)
{
	struct edgetpu_ikv *ikv = gcip_mailbox_get_data(mailbox);

	mutex_unlock(&ikv->cmd_queue_lock);
}

static u64 edgetpu_ikv_get_cmd_elem_seq(struct gcip_mailbox *mailbox, void *cmd)
{
	struct edgetpu_vii_command *vii_cmd = cmd;

	return vii_cmd->seq;
}

static void edgetpu_ikv_set_cmd_elem_seq(struct gcip_mailbox *mailbox, void *cmd, u64 seq)
{
	struct edgetpu_vii_command *vii_cmd = cmd;

	vii_cmd->seq = seq;
}

static u32 edgetpu_ikv_get_cmd_elem_code(struct gcip_mailbox *mailbox, void *cmd)
{
	struct edgetpu_vii_command *vii_cmd = cmd;

	return vii_cmd->code;
}

static u32 edgetpu_ikv_get_resp_queue_size(struct gcip_mailbox *mailbox)
{
	struct edgetpu_ikv *ikv = gcip_mailbox_get_data(mailbox);
	struct edgetpu_mailbox *mbx_hw = ikv->mbx_hardware;

	return mbx_hw->resp_queue_size;
}

static u32 edgetpu_ikv_get_resp_queue_head(struct gcip_mailbox *mailbox)
{
	struct edgetpu_ikv *ikv = gcip_mailbox_get_data(mailbox);
	struct edgetpu_mailbox *mbx_hw = ikv->mbx_hardware;

	return mbx_hw->resp_queue_head;
}

static u32 edgetpu_ikv_get_resp_queue_tail(struct gcip_mailbox *mailbox)
{
	struct edgetpu_ikv *ikv = gcip_mailbox_get_data(mailbox);
	struct edgetpu_mailbox *mbx_hw = ikv->mbx_hardware;

	return EDGETPU_MAILBOX_RESP_QUEUE_READ_SYNC(mbx_hw, tail);
}

static void edgetpu_ikv_inc_resp_queue_head(struct gcip_mailbox *mailbox, u32 inc)
{
	struct edgetpu_ikv *ikv = gcip_mailbox_get_data(mailbox);
	struct edgetpu_mailbox *mbx_hw = ikv->mbx_hardware;

	edgetpu_mailbox_inc_resp_queue_head(mbx_hw, inc);
}

static int edgetpu_ikv_acquire_resp_queue_lock(struct gcip_mailbox *mailbox, bool try, bool *atomic)
{
	struct edgetpu_ikv *ikv = gcip_mailbox_get_data(mailbox);

	*atomic = true;

	if (try)
		return spin_trylock_irqsave(&ikv->resp_queue_lock, ikv->resp_queue_lock_flags);

	spin_lock_irqsave(&ikv->resp_queue_lock, ikv->resp_queue_lock_flags);
	return 1;
}

static void edgetpu_ikv_release_resp_queue_lock(struct gcip_mailbox *mailbox)
{
	struct edgetpu_ikv *ikv = gcip_mailbox_get_data(mailbox);

	spin_unlock_irqrestore(&ikv->resp_queue_lock, ikv->resp_queue_lock_flags);
}

static u64 edgetpu_ikv_get_resp_elem_seq(struct gcip_mailbox *mailbox, void *resp)
{
	struct edgetpu_vii_response *vii_resp = resp;

	return vii_resp->seq;
}

static void edgetpu_ikv_set_resp_elem_seq(struct gcip_mailbox *mailbox, void *resp, u64 seq)
{
	struct edgetpu_vii_response *vii_resp = resp;

	vii_resp->seq = seq;
}

static void edgetpu_ikv_acquire_wait_list_lock(struct gcip_mailbox *mailbox, bool irqsave,
					       unsigned long *flags)
{
	struct edgetpu_ikv *ikv = gcip_mailbox_get_data(mailbox);

	spin_lock_irqsave(&ikv->wait_list_lock, *flags);
}

static void edgetpu_ikv_release_wait_list_lock(struct gcip_mailbox *mailbox, bool irqrestore,
					       unsigned long flags)
{
	struct edgetpu_ikv *ikv = gcip_mailbox_get_data(mailbox);

	spin_unlock_irqrestore(&ikv->wait_list_lock, flags);
}

static int edgetpu_ikv_wait_for_cmd_queue_not_full(struct gcip_mailbox *mailbox)
{
	struct edgetpu_ikv *ikv = gcip_mailbox_get_data(mailbox);
	u32 tail = mailbox->ops->get_cmd_queue_tail(mailbox);
	int ret;

	if (mailbox->ops->get_cmd_queue_head(mailbox) != (tail ^ mailbox->queue_wrap_bit))
		return 0;

	/* Credit enforcement should prevent this from ever happening. Log an error. */
	etdev_warn_ratelimited(ikv->etdev, "kernel VII command queue full\n");

	ret = wait_event_timeout(ikv->pending_commands,
				 mailbox->ops->get_cmd_queue_head(mailbox) !=
					 (tail ^ mailbox->queue_wrap_bit),
				 msecs_to_jiffies(mailbox->timeout));
	if (!ret)
		return -ETIMEDOUT;

	return 0;
}

static int edgetpu_ikv_before_enqueue_wait_list(struct gcip_mailbox *mailbox, void *resp,
						struct gcip_mailbox_resp_awaiter *awaiter)
{
	struct edgetpu_ikv_response *ikv_resp;

	/*
	 * Save the awaiter inside the response, so it can be cleaned up on response arrival,
	 * time-out, or free due to owning device-group closing.
	 *
	 * Since awaiters are only NULL for synchronous commands (which in-kernel VII does not
	 * support), there's no need to check it.
	 */
	ikv_resp = awaiter->data;
	ikv_resp->awaiter = awaiter;

	return 0;
}

static int edgetpu_ikv_after_enqueue_cmd(struct gcip_mailbox *mailbox, void *cmd)
{
	struct edgetpu_ikv *ikv = gcip_mailbox_get_data(mailbox);

	EDGETPU_MAILBOX_CMD_QUEUE_WRITE_SYNC(ikv->mbx_hardware, doorbell_set, 1);

	return 0;
}

static void edgetpu_ikv_after_fetch_resps(struct gcip_mailbox *mailbox, u32 num_resps)
{
	struct edgetpu_ikv *ikv = gcip_mailbox_get_data(mailbox);
	u32 size = mailbox->ops->get_resp_queue_size(mailbox);
	/*
	 * We consumed a lot of responses - ring the doorbell of *cmd* queue to notify the firmware,
	 * which might be waiting us to consume the response queue.
	 */
	if (num_resps >= size / 2)
		EDGETPU_MAILBOX_CMD_QUEUE_WRITE(ikv->mbx_hardware, doorbell_set, 1);
}

static void edgetpu_ikv_handle_awaiter_arrived(struct gcip_mailbox *mailbox,
					       struct gcip_mailbox_resp_awaiter *awaiter)
{
	struct edgetpu_ikv_response *resp = awaiter->data;

	edgetpu_ikv_process_response(resp, NULL, NULL, 0);
}

static void edgetpu_ikv_handle_awaiter_timedout(struct gcip_mailbox *mailbox,
						struct gcip_mailbox_resp_awaiter *awaiter)
{
	struct edgetpu_ikv *ikv = gcip_mailbox_get_data(mailbox);
	struct edgetpu_ikv_response *resp = awaiter->data;
	u16 code = VII_RESPONSE_CODE_KERNEL_CMD_TIMEOUT;
	u64 data = (u64)IKV_TIMEOUT;

	etdev_warn(ikv->etdev, "IKV seq %llu timed out", resp->client_seq);
	edgetpu_ikv_process_response(resp, &code, &data, -ETIMEDOUT);
}

static void edgetpu_ikv_flush_awaiter(struct gcip_mailbox *mailbox,
				      struct gcip_mailbox_resp_awaiter *awaiter)
{
	struct edgetpu_ikv_response *resp = awaiter->data;
	/* Store an independent pointer to `dest_queue_lock`, since `resp` may be released. */
	spinlock_t *dest_queue_lock = resp->dest_queue_lock;
	unsigned long flags;

	spin_lock_irqsave(dest_queue_lock, flags);

	if (resp->processed) {
		spin_unlock_irqrestore(dest_queue_lock, flags);
		return;
	}
	resp->processed = true;

	spin_unlock_irqrestore(dest_queue_lock, flags);

	/* Signal any out-fence, but skip the device group since it's being flushed. */
	signal_response_waiters(resp, -ECANCELED, false);

	gcip_mailbox_release_awaiter(awaiter);
}

static void edgetpu_ikv_release_awaiter_data(void *data)
{
	struct edgetpu_ikv_response *resp = data;

	kfree(resp);
}

const struct gcip_mailbox_ops ikv_mailbox_ops = {
	.get_cmd_queue_head = edgetpu_ikv_get_cmd_queue_head,
	.get_cmd_queue_tail = edgetpu_ikv_get_cmd_queue_tail,
	.inc_cmd_queue_tail = edgetpu_ikv_inc_cmd_queue_tail,
	.acquire_cmd_queue_lock = edgetpu_ikv_acquire_cmd_queue_lock,
	.release_cmd_queue_lock = edgetpu_ikv_release_cmd_queue_lock,
	.get_cmd_elem_seq = edgetpu_ikv_get_cmd_elem_seq,
	.set_cmd_elem_seq = edgetpu_ikv_set_cmd_elem_seq,
	.get_cmd_elem_code = edgetpu_ikv_get_cmd_elem_code,
	.get_resp_queue_size = edgetpu_ikv_get_resp_queue_size,
	.get_resp_queue_head = edgetpu_ikv_get_resp_queue_head,
	.get_resp_queue_tail = edgetpu_ikv_get_resp_queue_tail,
	.inc_resp_queue_head = edgetpu_ikv_inc_resp_queue_head,
	.acquire_resp_queue_lock = edgetpu_ikv_acquire_resp_queue_lock,
	.release_resp_queue_lock = edgetpu_ikv_release_resp_queue_lock,
	.get_resp_elem_seq = edgetpu_ikv_get_resp_elem_seq,
	.set_resp_elem_seq = edgetpu_ikv_set_resp_elem_seq,
	.acquire_wait_list_lock = edgetpu_ikv_acquire_wait_list_lock,
	.release_wait_list_lock = edgetpu_ikv_release_wait_list_lock,
	.wait_for_cmd_queue_not_full = edgetpu_ikv_wait_for_cmd_queue_not_full,
	.before_enqueue_wait_list = edgetpu_ikv_before_enqueue_wait_list,
	.after_enqueue_cmd = edgetpu_ikv_after_enqueue_cmd,
	.after_fetch_resps = edgetpu_ikv_after_fetch_resps,
	/* .before_handle_resp is not needed */
	.handle_awaiter_arrived = edgetpu_ikv_handle_awaiter_arrived,
	.handle_awaiter_timedout = edgetpu_ikv_handle_awaiter_timedout,
	.flush_awaiter = edgetpu_ikv_flush_awaiter,
	.release_awaiter_data = edgetpu_ikv_release_awaiter_data,
};

void edgetpu_ikv_process_response(struct edgetpu_ikv_response *ikv_resp, u16 *resp_code,
				  u64 *resp_retval, int fence_error)
{
	unsigned long flags;

	spin_lock_irqsave(ikv_resp->dest_queue_lock, flags);

	/*
	 * Return immediately if either of the following caused the response to be "processed":
	 * - the response timed-out
	 * - the queue waiting for the response is being released
	 */
	if (ikv_resp->processed) {
		spin_unlock_irqrestore(ikv_resp->dest_queue_lock, flags);
		return;
	}
	ikv_resp->processed = true;

	/* If the command resulted in an error, override the error code and retval as provided. */
	if (resp_code)
		ikv_resp->resp.code = *resp_code;
	if (resp_retval)
		ikv_resp->resp.retval = *resp_retval;

	/* Set the response sequence number to the value expected by the client. */
	ikv_resp->resp.seq = ikv_resp->client_seq;

	/*
	 * Move the response from the "pending" list to the "ready" list.
	 *
	 * It's necessary to check if the response was actually in the "pending" list, since a
	 * command that was canceled before it was ever enqueued in the mailbox will have a
	 * floating response.
	 */
	if (ikv_resp->list_entry.prev)
		list_del(&ikv_resp->list_entry);
	list_add_tail(&ikv_resp->list_entry, ikv_resp->dest_queue);

	spin_unlock_irqrestore(ikv_resp->dest_queue_lock, flags);

	signal_response_waiters(ikv_resp, fence_error, true);
}
