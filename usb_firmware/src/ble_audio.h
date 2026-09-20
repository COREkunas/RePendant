/* Bounded, authenticated, physically confirmed BLE PCM handoff. */
#ifndef OPENPENDANT_BLE_AUDIO_H_
#define OPENPENDANT_BLE_AUDIO_H_

#include <stddef.h>
#include <stdint.h>

struct bt_conn;

enum ble_audio_state {
	BLE_AUDIO_IDLE = 0, BLE_AUDIO_WAITING, BLE_AUDIO_RECORDING,
	BLE_AUDIO_READY, BLE_AUDIO_DRAINED, BLE_AUDIO_EXPIRED,
	BLE_AUDIO_CANCELLED, BLE_AUDIO_ERROR,
};

#define BLE_AUDIO_BEGIN 0x10U
#define BLE_AUDIO_STATUS 0x11U
#define BLE_AUDIO_CHUNK 0x12U
#define BLE_AUDIO_CANCEL 0x13U
#define BLE_AUDIO_MIN_MTU 96U
#define BLE_AUDIO_REPLY_MAX 72U

/* reply_len is input capacity/output length. Returns an OP status, not errno.
 * No unsolicited notifications, pairing, flash writes or hardware operations
 * run from this dispatcher. The main dispatcher must erase its reply buffer
 * after notification and call cancel_all(conn) if delivery fails.
 */
int ble_audio_command(struct bt_conn *conn, uint8_t command,
		      const uint8_t *payload, size_t len,
		      uint8_t *reply, size_t *reply_len);
/* Safe in Bluetooth thread callbacks, not ISR. NULL cancels any owner.
 * Active capture is never interrupted; bounded STOP/power cleanup finishes.
 */
void ble_audio_cancel_all(struct bt_conn *conn);
void ble_audio_disconnected(struct bt_conn *conn);

/* Private worker entry, called ONLY by the existing microphone thread after
 * mic_commands_queue_ble has reserved its busy flag. Returns only after no
 * retained PCM remains; the microphone thread alone then releases busy.
 */
void ble_audio_worker(void);

/* Supplied by the main application: logical pressed=1, released=0, errno<0. */
int pendant_button_read(void);

/* Private pure button gate, compiled verbatim by native synthetic-trace tests.
 * The caller owns the request's absolute 15-second deadline and cancellation.
 */
#ifdef BLE_AUDIO_TAP_IMPLEMENTATION
struct ble_audio_tap_gate {
	int raw;
	int stable;
	unsigned int phase; /* 0 initial/rejected release, 1 press, 2 release */
	int64_t changed;
	int64_t pressed;
};

static inline void ble_audio_tap_init(struct ble_audio_tap_gate *gate, int64_t now)
{
	gate->raw = -1;
	gate->stable = -1;
	gate->phase = 0;
	gate->changed = now;
	gate->pressed = now;
}

/* -1 invalid sample/time, 0 waiting, 1 fresh short debounced tap accepted. */
static inline int ble_audio_tap_step(struct ble_audio_tap_gate *gate,
				     int value, int64_t now)
{
	if ((value != 0 && value != 1) || now < gate->changed) { return -1; }
	if (gate->raw != value) { gate->raw = value; gate->changed = now; }
	if (gate->phase == 2 && now - gate->pressed > 1000) { gate->phase = 0; }
	if (now - gate->changed >= 20 && gate->stable != gate->raw) {
		gate->stable = gate->raw;
		if (gate->phase == 0 && gate->stable == 0) { gate->phase = 1; }
		else if (gate->phase == 1 && gate->stable == 1) {
			gate->phase = 2;
			gate->pressed = gate->changed;
		} else if (gate->phase == 2 && gate->stable == 0) { return 1; }
	}
	/* A rejected hold's release rearms but does not itself authorize. */
	if (gate->phase == 0 && gate->stable == 0 && gate->raw == 0 &&
	    now - gate->changed >= 20) { gate->phase = 1; }
	return 0;
}
#endif
#endif
