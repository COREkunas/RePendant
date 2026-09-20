#define BLE_AUDIO_TAP_IMPLEMENTATION
#include "ble_audio.h"
#include "ble_security.h"
#include "mic_commands.h"
#include "mic_pcm_store.h"

#include <errno.h>
#include <limits.h>
#include <string.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/kernel.h>
#include <zephyr/random/random.h>
#include <zephyr/sys/byteorder.h>

#define AUDIO_WAIT_MS 15000
#define AUDIO_POLL_MS 20

enum op_status { OP_OK = 0, OP_LENGTH = 4, OP_HARDWARE = 5, OP_BUSY = 6,
	OP_DENIED = 7, OP_STATE = 8, OP_OFFSET = 9, OP_MTU = 10 };

static struct {
	/* One ref prevents connection-object reuse while a request is active.
	 * Terminal metadata retains the ref until disconnect or next BEGIN.
	 */
	struct bt_conn *owner;
	uint32_t id;
	enum ble_audio_state state;
	bool worker_active;
	bool cancelled;
	bool disconnected;
	int64_t wait_deadline;
	int16_t error;
	uint32_t total_bytes;
	uint32_t crc32;
	uint32_t remaining;
} audio;
K_MUTEX_DEFINE(audio_lock);

static int16_t wire_error(int err)
{
	return err < INT16_MIN || err > INT16_MAX ? -EIO : (int16_t)err;
}

static bool terminal(enum ble_audio_state state)
{
	return state >= BLE_AUDIO_DRAINED;
}

/* Caller holds audio_lock. Never touch the store for terminal/inactive
 * metadata: a subsequent USB job may already own the shared PCM buffer.
 */
static void refresh_ready_locked(void)
{
	if (!audio.worker_active || audio.state != BLE_AUDIO_READY) { return; }
	struct mic_pcm_store_info info;
	mic_pcm_store_get_state(&info);
	if (info.state == MIC_PCM_STORE_READY) {
		audio.remaining = info.bytes_remaining;
	} else if (info.state == MIC_PCM_STORE_DRAINED) {
		audio.remaining = 0;
		audio.state = BLE_AUDIO_DRAINED;
	} else {
		audio.remaining = 0;
		audio.state = info.state == MIC_PCM_STORE_EXPIRED ?
			BLE_AUDIO_EXPIRED : BLE_AUDIO_ERROR;
		audio.error = info.state == MIC_PCM_STORE_EXPIRED ? -ETIMEDOUT : -EIO;
		mic_pcm_store_scrub();
	}
}

static void cancel_locked(void)
{
	if (!audio.worker_active || terminal(audio.state)) { return; }
	audio.cancelled = true;
	if (audio.state == BLE_AUDIO_READY) {
		/* No peripheral/DMA ownership remains in READY. */
		mic_pcm_store_scrub();
		audio.remaining = 0;
		audio.state = BLE_AUDIO_CANCELLED;
		audio.error = 0;
	}
}

void ble_audio_cancel_all(struct bt_conn *conn)
{
	k_mutex_lock(&audio_lock, K_FOREVER);
	if (conn == NULL || conn == audio.owner) { cancel_locked(); }
	k_mutex_unlock(&audio_lock);
}

void ble_audio_disconnected(struct bt_conn *conn)
{
	k_mutex_lock(&audio_lock, K_FOREVER);
	if (conn == audio.owner && conn != NULL) {
		audio.disconnected = true;
		cancel_locked();
		if (!audio.worker_active) {
			bt_conn_unref(audio.owner);
			audio.owner = NULL;
		}
	}
	k_mutex_unlock(&audio_lock);
}

static int begin_locked(struct bt_conn *conn, uint8_t *reply)
{
	if (audio.worker_active) { return OP_BUSY; }
	uint32_t id = 0;
	/* A failed entropy source must not silently produce a predictable ID. */
	for (unsigned int i = 0; i < 4U; ++i) {
		if (sys_csrand_get(&id, sizeof(id)) != 0) { return OP_HARDWARE; }
		if (id != 0 && id != audio.id) { break; }
	}
	if (id == 0 || id == audio.id) { return OP_HARDWARE; }
	/* Queue while holding audio_lock: the worker cannot observe partially
	 * initialized session state, and a failed reservation preserves metadata.
	 */
	int err = mic_commands_queue_ble();
	if (err != 0) { return err == -EBUSY ? OP_BUSY : OP_HARDWARE; }
	if (audio.owner != NULL) { bt_conn_unref(audio.owner); }
	memset(&audio, 0, sizeof(audio));
	audio.owner = bt_conn_ref(conn);
	audio.id = id;
	audio.state = BLE_AUDIO_WAITING;
	audio.worker_active = true;
	audio.wait_deadline = k_uptime_get() + AUDIO_WAIT_MS;
	sys_put_le32(id, reply);
	reply[4] = BLE_AUDIO_WAITING;
	return OP_OK;
}

int ble_audio_command(struct bt_conn *conn, uint8_t command,
		      const uint8_t *payload, size_t len,
		      uint8_t *reply, size_t *reply_len)
{
	if (reply_len == NULL) { return OP_LENGTH; }
	size_t capacity = *reply_len;
	*reply_len = 0;
	if (conn == NULL || !pendant_ble_authorized(conn)) {
		if (conn != NULL) { ble_audio_cancel_all(conn); }
		return OP_DENIED;
	}
	if (bt_gatt_get_mtu(conn) < BLE_AUDIO_MIN_MTU) { return OP_MTU; }
	size_t input_size, output_size;
	switch (command) {
	case BLE_AUDIO_BEGIN: input_size = 0; output_size = 5; break;
	case BLE_AUDIO_STATUS: input_size = 4; output_size = 19; break;
	case BLE_AUDIO_CHUNK: input_size = 8; output_size = BLE_AUDIO_REPLY_MAX; break;
	case BLE_AUDIO_CANCEL: input_size = 4; output_size = 5; break;
	default: return OP_STATE;
	}
	if (len != input_size || (len != 0 && payload == NULL) ||
	    reply == NULL || capacity < output_size) { return OP_LENGTH; }
	int result = OP_OK;
	k_mutex_lock(&audio_lock, K_FOREVER);
	if (command == BLE_AUDIO_BEGIN) {
		result = begin_locked(conn, reply);
		goto done;
	}
	if (conn != audio.owner || sys_get_le32(payload) != audio.id || audio.id == 0) {
		result = OP_DENIED;
		goto done;
	}
	refresh_ready_locked();
	if (command == BLE_AUDIO_STATUS) {
		sys_put_le32(audio.id, reply);
		reply[4] = audio.state;
		sys_put_le32(audio.total_bytes, reply + 5);
		sys_put_le32(audio.crc32, reply + 9);
		sys_put_le32(audio.remaining, reply + 13);
		sys_put_le16((uint16_t)audio.error, reply + 17);
	} else if (command == BLE_AUDIO_CANCEL) {
		cancel_locked();
		sys_put_le32(audio.id, reply);
		/* WAITING/RECORDING cancellation is acknowledged immediately but
		 * may remain in its current state until the worker safely finishes.
		 */
		reply[4] = audio.state;
	} else {
		if (audio.state != BLE_AUDIO_READY || audio.cancelled) {
			result = OP_STATE;
			goto done;
		}
		struct mic_pcm_store_info info;
		mic_pcm_store_get_state(&info);
		uint32_t offset = sys_get_le32(payload + 4);
		if (offset != info.next_offset) { result = OP_OFFSET; goto done; }
		int count = mic_pcm_store_take(offset, reply + 8, MIC_PCM_STORE_MAX_CHUNK);
		if (count <= 0) {
			mic_pcm_store_scrub();
			audio.remaining = 0;
			audio.error = wire_error(count < 0 ? count : -EIO);
			audio.state = count == -ETIMEDOUT ? BLE_AUDIO_EXPIRED : BLE_AUDIO_ERROR;
			result = OP_STATE;
			goto done;
		}
		sys_put_le32(audio.id, reply);
		sys_put_le32(offset, reply + 4);
		output_size = 8U + (size_t)count;
		refresh_ready_locked();
	}
done:
	if (result == OP_OK) { *reply_len = output_size; }
	k_mutex_unlock(&audio_lock);
	return result;
}

/* Initially require a debounced release, then a NEW debounced press/release.
 * A press over one second is rejected; its release merely rearms a new tap.
 * All timing belongs to this request, never to a previous button gesture.
 */
static int wait_for_tap(struct bt_conn *owner, int64_t deadline)
{
	struct ble_audio_tap_gate gate;
	int64_t now = k_uptime_get();
	ble_audio_tap_init(&gate, now);
	while ((now = k_uptime_get()) < deadline) {
		bool authorized = pendant_ble_authorized(owner);
		k_mutex_lock(&audio_lock, K_FOREVER);
		bool cancelled = audio.cancelled || audio.disconnected || !authorized;
		k_mutex_unlock(&audio_lock);
		if (cancelled) { return -ECANCELED; }
		int value = pendant_button_read();
		if (value < 0) { return value; }
		int result = ble_audio_tap_step(&gate, value, now);
		if (result < 0) { return -EIO; }
		if (result == 1) { return 0; }
		k_sleep(K_MSEC(AUDIO_POLL_MS));
	}
	return -ETIMEDOUT;
}

void ble_audio_worker(void)
{
	k_mutex_lock(&audio_lock, K_FOREVER);
	struct bt_conn *owner = audio.owner == NULL ? NULL : bt_conn_ref(audio.owner);
	int64_t deadline = audio.wait_deadline;
	k_mutex_unlock(&audio_lock);
	int err = owner == NULL ? -EIO : wait_for_tap(owner, deadline);
	/* Authorization calls stay outside audio_lock to avoid security-callback
	 * lock inversion. Revocation during an already committed capture marks
	 * cancellation; it never bypasses bounded STOP/DMA/power cleanup.
	 */
	bool authorized = owner != NULL && pendant_ble_authorized(owner);
	k_mutex_lock(&audio_lock, K_FOREVER);
	if (audio.cancelled || audio.disconnected || !authorized) { err = -ECANCELED; }
	if (err == 0) { audio.state = BLE_AUDIO_RECORDING; }
	k_mutex_unlock(&audio_lock);
	struct mic_pcm_store_info info = {0};
	if (err == 0) { err = mic_commands_ble_capture(&info); }
	if (err == 0 && (info.state != MIC_PCM_STORE_READY || info.total_bytes == 0 ||
	    info.total_bytes > MIC_PCM_STORE_MAX_BYTES ||
	    info.total_bytes % (MIC_PCM_STORE_FRAME_SAMPLES * 2U) != 0 ||
	    info.samples != info.total_bytes / 2U || info.next_offset != 0 ||
	    info.bytes_remaining != info.total_bytes)) { err = -EIO; }
	authorized = owner != NULL && pendant_ble_authorized(owner);
	k_mutex_lock(&audio_lock, K_FOREVER);
	if (audio.cancelled || audio.disconnected || !authorized) { err = -ECANCELED; }
	if (err == 0) {
		audio.total_bytes = info.total_bytes;
		audio.crc32 = info.crc32;
		audio.remaining = info.bytes_remaining;
		audio.state = BLE_AUDIO_READY;
	} else {
		mic_pcm_store_scrub();
		audio.remaining = 0;
		audio.error = err == -ECANCELED ? 0 : wire_error(err);
		audio.state = err == -ECANCELED ? BLE_AUDIO_CANCELLED :
			(err == -ETIMEDOUT ? BLE_AUDIO_EXPIRED : BLE_AUDIO_ERROR);
	}
	k_mutex_unlock(&audio_lock);
	for (;;) {
		authorized = owner != NULL && pendant_ble_authorized(owner);
		k_mutex_lock(&audio_lock, K_FOREVER);
		if (!authorized || audio.disconnected) { cancel_locked(); }
		refresh_ready_locked();
		if (terminal(audio.state)) {
			/* No callback may scrub/take after this transition: the main
			 * microphone worker will release busy immediately on return.
			 */
			mic_pcm_store_scrub();
			audio.remaining = 0;
			audio.worker_active = false;
			if (audio.disconnected && audio.owner != NULL) {
				bt_conn_unref(audio.owner);
				audio.owner = NULL;
			}
			k_mutex_unlock(&audio_lock);
			break;
		}
		k_mutex_unlock(&audio_lock);
		k_sleep(K_MSEC(AUDIO_POLL_MS));
	}
	if (owner != NULL) { bt_conn_unref(owner); }
}
