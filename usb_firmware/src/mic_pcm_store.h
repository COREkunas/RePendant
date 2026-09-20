/* Bounded volatile PCM handoff; no USB, flash, or hardware operations. */
#ifndef OPENPENDANT_MIC_PCM_STORE_H_
#define OPENPENDANT_MIC_PCM_STORE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MIC_PCM_STORE_FRAME_SAMPLES 320U
#define MIC_PCM_STORE_MAX_SAMPLES 16000U
#define MIC_PCM_STORE_MAX_BYTES (MIC_PCM_STORE_MAX_SAMPLES * 2U)
#define MIC_PCM_STORE_MAX_CHUNK 64U
#define MIC_PCM_STORE_EXPIRY_MS 30000U

enum mic_pcm_store_state {
	MIC_PCM_STORE_EMPTY,
	MIC_PCM_STORE_CAPTURING,
	MIC_PCM_STORE_READY,
	MIC_PCM_STORE_DRAINED,
	MIC_PCM_STORE_EXPIRED,
};

struct mic_pcm_store_info {
	enum mic_pcm_store_state state;
	uint32_t samples;
	uint32_t total_bytes;
	uint32_t bytes_remaining;
	uint32_t next_offset;
	uint32_t crc32;
	bool scrubbed;
};

/* begin starts the independent 30-second deadline BEFORE capture. Only an
 * explicit begin can activate a new store after expiry/draining/scrubbing.
 */
int mic_pcm_store_begin(void);
int mic_pcm_store_append(const int16_t *frame, size_t sample_count);
/* Caller MUST establish STOP, DMA cleanup, and microphone power OFF first.
 * Accepts a nonempty whole-frame result, up to one second at 16 kHz.
 */
int mic_pcm_store_seal(struct mic_pcm_store_info *info);
/* Returns 1..64 copied bytes, or negative errno; exact sequential offsets only.
 * Successfully copied bytes are irreversibly erased before this call returns.
 * EXPIRED returns -ETIMEDOUT. Other inactive states never yield PCM.
 */
int mic_pcm_store_take(size_t offset, uint8_t *out, size_t capacity);
void mic_pcm_store_scrub(void);
void mic_pcm_store_get_state(struct mic_pcm_store_info *info);

/* Private, platform-independent implementation shared with native tests.
 * The Zephyr wrapper must serialize EVERY core operation and buffer access.
 */
#ifdef MIC_PCM_STORE_IMPLEMENTATION
enum mic_pcm_core_result {
	MIC_PCM_CORE_OK,
	MIC_PCM_CORE_INVALID,
	MIC_PCM_CORE_STATE,
	MIC_PCM_CORE_EXPIRED,
	MIC_PCM_CORE_FULL,
};

struct mic_pcm_store_core {
	/* Permanently bound to one fixed-size static buffer by the wrapper. */
	uint8_t *data;
	int64_t deadline_ms;
	uint32_t used;
	uint32_t offset;
	uint32_t crc32;
	enum mic_pcm_store_state state;
	bool scrubbed;
};

static inline void mic_pcm_core_zero(uint8_t *data, size_t count)
{
	volatile uint8_t *bytes = data;
	while (count-- != 0) { *bytes++ = 0; }
}

static inline void mic_pcm_core_wipe(struct mic_pcm_store_core *store,
				     enum mic_pcm_store_state state)
{
	mic_pcm_core_zero(store->data, MIC_PCM_STORE_MAX_BYTES);
	store->deadline_ms = 0;
	store->used = 0;
	store->offset = 0;
	store->crc32 = 0;
	store->state = state;
	store->scrubbed = true;
}

static inline bool mic_pcm_core_live(const struct mic_pcm_store_core *store)
{
	return store->state == MIC_PCM_STORE_CAPTURING || store->state == MIC_PCM_STORE_READY;
}

static inline bool mic_pcm_core_expire(struct mic_pcm_store_core *store, int64_t now_ms)
{
	if (mic_pcm_core_live(store) && now_ms >= store->deadline_ms) {
		mic_pcm_core_wipe(store, MIC_PCM_STORE_EXPIRED);
	}
	return store->state == MIC_PCM_STORE_EXPIRED;
}

static inline void mic_pcm_core_info(const struct mic_pcm_store_core *store,
				     struct mic_pcm_store_info *info)
{
	info->state = store->state;
	info->samples = store->used / 2U;
	info->total_bytes = store->used;
	info->bytes_remaining = store->used - store->offset;
	info->next_offset = store->offset;
	info->crc32 = store->crc32;
	info->scrubbed = store->scrubbed;
}

static inline enum mic_pcm_core_result mic_pcm_core_begin(struct mic_pcm_store_core *store,
							   int64_t now_ms)
{
	mic_pcm_core_wipe(store, MIC_PCM_STORE_EMPTY);
	if (now_ms < 0 || now_ms > INT64_MAX - MIC_PCM_STORE_EXPIRY_MS) {
		return MIC_PCM_CORE_INVALID;
	}
	store->deadline_ms = now_ms + MIC_PCM_STORE_EXPIRY_MS;
	store->state = MIC_PCM_STORE_CAPTURING;
	return MIC_PCM_CORE_OK;
}

static inline enum mic_pcm_core_result mic_pcm_core_append(struct mic_pcm_store_core *store,
		 const int16_t *frame, size_t sample_count, int64_t now_ms)
{
	if (mic_pcm_core_expire(store, now_ms)) { return MIC_PCM_CORE_EXPIRED; }
	if (frame == NULL || sample_count != MIC_PCM_STORE_FRAME_SAMPLES) {
		return MIC_PCM_CORE_INVALID;
	}
	if (store->state != MIC_PCM_STORE_CAPTURING) { return MIC_PCM_CORE_STATE; }
	if (store->used > MIC_PCM_STORE_MAX_BYTES - MIC_PCM_STORE_FRAME_SAMPLES * 2U) {
		return MIC_PCM_CORE_FULL;
	}
	for (size_t i = 0; i < sample_count; ++i) {
		uint16_t value = (uint16_t)frame[i];
		store->data[store->used + i * 2U] = (uint8_t)value;
		store->data[store->used + i * 2U + 1U] = (uint8_t)(value >> 8);
	}
	store->used += MIC_PCM_STORE_FRAME_SAMPLES * 2U;
	store->scrubbed = false;
	return MIC_PCM_CORE_OK;
}

/* CRC-32/ISO-HDLC: polynomial 0xEDB88320, init/xor-out 0xFFFFFFFF.
 * Nibble table bounds work under the spinlock; matches zlib.crc32.
 */
static inline uint32_t mic_pcm_core_crc32(const uint8_t *data, size_t count)
{
	static const uint32_t table[16] = {
		0x00000000U, 0x1DB71064U, 0x3B6E20C8U, 0x26D930ACU,
		0x76DC4190U, 0x6B6B51F4U, 0x4DB26158U, 0x5005713CU,
		0xEDB88320U, 0xF00F9344U, 0xD6D6A3E8U, 0xCB61B38CU,
		0x9B64C2B0U, 0x86D3D2D4U, 0xA00AE278U, 0xBDBDF21CU,
	};
	uint32_t crc = UINT32_MAX;
	for (size_t i = 0; i < count; ++i) {
		crc ^= data[i];
		crc = (crc >> 4) ^ table[crc & 15U];
		crc = (crc >> 4) ^ table[crc & 15U];
	}
	return ~crc;
}

static inline enum mic_pcm_core_result mic_pcm_core_seal(struct mic_pcm_store_core *store,
							  int64_t now_ms)
{
	if (mic_pcm_core_expire(store, now_ms)) { return MIC_PCM_CORE_EXPIRED; }
	if (store->state != MIC_PCM_STORE_CAPTURING) { return MIC_PCM_CORE_STATE; }
	if (store->used == 0 || store->used > MIC_PCM_STORE_MAX_BYTES ||
	    store->used % (MIC_PCM_STORE_FRAME_SAMPLES * 2U) != 0) {
		return MIC_PCM_CORE_INVALID;
	}
	store->crc32 = mic_pcm_core_crc32(store->data, store->used);
	store->state = MIC_PCM_STORE_READY;
	return MIC_PCM_CORE_OK;
}

static inline enum mic_pcm_core_result mic_pcm_core_take(struct mic_pcm_store_core *store,
		 size_t offset, uint8_t *out, size_t capacity, size_t *copied, int64_t now_ms)
{
	*copied = 0;
	if (mic_pcm_core_expire(store, now_ms)) { return MIC_PCM_CORE_EXPIRED; }
	if (out == NULL || capacity == 0 || capacity > MIC_PCM_STORE_MAX_CHUNK) {
		return MIC_PCM_CORE_INVALID;
	}
	if (store->state != MIC_PCM_STORE_READY) { return MIC_PCM_CORE_STATE; }
	if (offset != store->offset || store->offset >= store->used) {
		return MIC_PCM_CORE_INVALID;
	}
	size_t count = store->used - store->offset;
	if (count > capacity) { count = capacity; }
	for (size_t i = 0; i < count; ++i) { out[i] = store->data[store->offset + i]; }
	mic_pcm_core_zero(store->data + store->offset, count);
	store->offset += (uint32_t)count;
	*copied = count;
	if (store->offset == store->used) {
		mic_pcm_core_zero(store->data, MIC_PCM_STORE_MAX_BYTES);
		store->scrubbed = true;
		store->state = MIC_PCM_STORE_DRAINED;
		store->deadline_ms = 0;
	}
	return MIC_PCM_CORE_OK;
}
#endif /* MIC_PCM_STORE_IMPLEMENTATION */
#endif
