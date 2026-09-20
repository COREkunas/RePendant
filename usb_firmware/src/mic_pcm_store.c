#define MIC_PCM_STORE_IMPLEMENTATION
#include "mic_pcm_store.h"

#include <errno.h>
#include <zephyr/kernel.h>

/* Exact-size symbol is checked by the offline ELF memory-boundary audit. */
static uint8_t pcm_store_buffer[MIC_PCM_STORE_MAX_BYTES];
static struct mic_pcm_store_core store = { .data = pcm_store_buffer, .scrubbed = true };
static struct k_spinlock store_lock;
static void expiry_handler(struct k_work *work);
K_WORK_DELAYABLE_DEFINE(store_expiry, expiry_handler);

static int result_errno(enum mic_pcm_core_result result)
{
	switch (result) {
	case MIC_PCM_CORE_OK: return 0;
	case MIC_PCM_CORE_INVALID: return -EINVAL;
	case MIC_PCM_CORE_EXPIRED: return -ETIMEDOUT;
	case MIC_PCM_CORE_FULL: return -ENOSPC;
	default: return -EACCES;
	}
}

static void expiry_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	k_spinlock_key_t key = k_spin_lock(&store_lock);
	int64_t now = k_uptime_get();
	(void)mic_pcm_core_expire(&store, now);
	if (mic_pcm_core_live(&store)) {
		/* A stale callback may belong to a prior begin. It must honour the
		 * CURRENT absolute deadline rather than expire/resurrect old data.
		 * Reschedule is nonblocking/ISR-safe; serialize it with begin/scrub.
		 */
		if (k_work_reschedule(&store_expiry, K_MSEC(store.deadline_ms - now)) < 0) {
			mic_pcm_core_wipe(&store, MIC_PCM_STORE_EXPIRED);
		}
	}
	k_spin_unlock(&store_lock, key);
}

int mic_pcm_store_begin(void)
{
	k_spinlock_key_t key = k_spin_lock(&store_lock);
	int err = result_errno(mic_pcm_core_begin(&store, k_uptime_get()));
	int64_t remaining = store.deadline_ms - k_uptime_get();
	if (err == 0 && (remaining <= 0 ||
	    k_work_reschedule(&store_expiry, K_MSEC(remaining)) < 0)) {
		mic_pcm_core_wipe(&store, MIC_PCM_STORE_EMPTY);
		err = -EIO;
	}
	k_spin_unlock(&store_lock, key);
	return err;
}

int mic_pcm_store_append(const int16_t *frame, size_t sample_count)
{
	k_spinlock_key_t key = k_spin_lock(&store_lock);
	int err = result_errno(mic_pcm_core_append(&store, frame, sample_count, k_uptime_get()));
	if (mic_pcm_core_expire(&store, k_uptime_get())) { err = -ETIMEDOUT; }
	k_spin_unlock(&store_lock, key);
	return err;
}

int mic_pcm_store_seal(struct mic_pcm_store_info *info)
{
	if (info == NULL) { return -EINVAL; }
	k_spinlock_key_t key = k_spin_lock(&store_lock);
	int err = result_errno(mic_pcm_core_seal(&store, k_uptime_get()));
	if (mic_pcm_core_expire(&store, k_uptime_get())) { err = -ETIMEDOUT; }
	mic_pcm_core_info(&store, info);
	k_spin_unlock(&store_lock, key);
	return err;
}

int mic_pcm_store_take(size_t offset, uint8_t *out, size_t capacity)
{
	size_t copied = 0;
	k_spinlock_key_t key = k_spin_lock(&store_lock);
	int64_t original_deadline = store.deadline_ms;
	int err = result_errno(mic_pcm_core_take(&store, offset, out, capacity, &copied,
					       k_uptime_get()));
	int64_t now = k_uptime_get();
	if ((copied != 0 && now >= original_deadline) || mic_pcm_core_expire(&store, now)) {
		if (copied != 0) { mic_pcm_core_zero(out, copied); }
		mic_pcm_core_wipe(&store, MIC_PCM_STORE_EXPIRED);
		err = -ETIMEDOUT;
	}
	if (store.state == MIC_PCM_STORE_DRAINED) {
		(void)k_work_cancel_delayable(&store_expiry);
	}
	k_spin_unlock(&store_lock, key);
	return err == 0 ? (int)copied : err;
}

void mic_pcm_store_scrub(void)
{
	k_spinlock_key_t key = k_spin_lock(&store_lock);
	mic_pcm_core_wipe(&store, MIC_PCM_STORE_EMPTY);
	(void)k_work_cancel_delayable(&store_expiry);
	k_spin_unlock(&store_lock, key);
}

void mic_pcm_store_get_state(struct mic_pcm_store_info *info)
{
	if (info == NULL) { return; }
	k_spinlock_key_t key = k_spin_lock(&store_lock);
	(void)mic_pcm_core_expire(&store, k_uptime_get());
	mic_pcm_core_info(&store, info);
	k_spin_unlock(&store_lock, key);
}
