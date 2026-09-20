/* Included only by nand_id.c: fixed single-block modifying qualification.
 * No generic transport, private payload export, retry or factory FTL. */
static uint32_t *qualify_words(void) { return qualify_active->result->words; }

static int qualify_fail(enum nand_qualify_outcome outcome, int rc)
{
	uint32_t *w = qualify_words();
	if (w[NQ_PRIMARY_OUTCOME] == NQ_NOT_RUN) {
		w[NQ_PRIMARY_OUTCOME] = outcome; w[NQ_PRIMARY_RC] = (uint32_t)(rc != 0 ? rc : -EIO);
		w[NQ_FAILED_PHASE] = w[NQ_PHASE];
	}
	return (int32_t)w[NQ_PRIMARY_RC];
}

static void qualify_snapshot(struct nand_qualify_context *q)
{
	uint32_t *w = q->result->words;
	w[NQ_STOPPED] = atomic_get(&stopped) != 0; w[NQ_BUS_RELEASED] = atomic_get(&bus_released) != 0;
	w[NQ_FAULT] = atomic_get(&fault) != 0; w[NQ_CS_CONFIGURED] = atomic_get(&cs_configured) != 0;
	w[NQ_CS_HIGH] = atomic_get(&cs_high) != 0; w[NQ_AUX_CONFIGURED] = atomic_get(&aux_configured) != 0;
	w[NQ_AUX_HIGH] = atomic_get(&aux_high) != 0; w[NQ_ATTEMPTED] = atomic_get(&attempted) != 0;
	w[NQ_BUSY] = atomic_get(&busy) != 0;
	w[NQ_RAM_SCRUBBED] = atomic_get(&marker_ram_scrubbed) && atomic_get(&page0_ram_scrubbed);
	w[NQ_ELAPSED_MS] = q->clock_started ? (uint32_t)(k_uptime_get() - q->first_start_ms) : 0;
	w[NQ_BLOCK_QUARANTINED] = 1;
}

static bool qualify_safe(void)
{
	return atomic_get(&stopped) && !atomic_get(&fault) && atomic_get(&cs_high) &&
		observe_aux() && !qualify_words()[NQ_READY_UNKNOWN];
}

static bool qualify_allowed(enum nand_transport_operation op)
{
	struct nand_qualify_context *q = qualify_active; uint32_t *w = q->result->words;
	if (w[NQ_TRANSFERS] >= NAND_QUALIFY_MAX_STARTS || q->row_index >= 64U) { return false; }
	if (q->restoring) {
		if (!qualify_safe()) { return false; }
		if (op == TRANSPORT_FINAL || op == TRANSPORT_MARKER_A0 || op == TRANSPORT_CONFIG) { return true; }
		if (!w[NQ_WEL_VALID] || w[NQ_WEL]) {
			return op == TRANSPORT_QUALIFY_WRDI && w[NQ_WEL_VALID] && w[NQ_WEL] && !w[NQ_WRDI_ATTEMPTED];
		}
		return (op == TRANSPORT_QUALIFY_LOCK && w[NQ_A0_CHANGE_ATTEMPTED] && !w[NQ_A0_RESTORE_ATTEMPTED]) ||
			(op == TRANSPORT_MARKER_ON && w[NQ_B0_OFF_STARTS] && !w[NQ_B0_RESTORE_ATTEMPTED]);
	}
	uint32_t phase = w[NQ_PHASE];
	if (op == TRANSPORT_ID) { return phase == NQ_PHASE_PREFLIGHT && w[NQ_TRANSFERS] < 3U; }
	if (op == TRANSPORT_INITIAL || op == TRANSPORT_FINAL || op == TRANSPORT_CONFIG || op == TRANSPORT_MARKER_A0) { return true; }
	if (op == TRANSPORT_MARKER_OFF) {
		return w[NQ_B0_CURRENT_VALID] && w[NQ_B0_CURRENT] == 0x10 &&
			((phase == NQ_PHASE_PREFLIGHT && !w[NQ_B0_OFF_STARTS] && w[NQ_ID_VALID] &&
			  w[NQ_INITIAL_A0_VALID] && w[NQ_INITIAL_A0] == 0x7c && w[NQ_INITIAL_VALID] && !w[NQ_INITIAL_C0]) ||
			 (phase == NQ_PHASE_MARKER && w[NQ_B0_OFF_STARTS] == 1 && w[NQ_VERIFY_READS] == 65 && w[NQ_PATTERN_READS] == 2));
	}
	if (op == TRANSPORT_MARKER_ON) {
		return phase == NQ_PHASE_ECC_ON && !w[NQ_B0_ON_STARTS] && w[NQ_ERASE_COMPLETED] &&
			w[NQ_BLANK_ROWS] == 64 && w[NQ_B0_CURRENT_VALID] && w[NQ_B0_CURRENT] == 0;
	}
	if (op == TRANSPORT_QUALIFY_UNLOCK) {
		return phase == NQ_PHASE_PROTECTION && w[NQ_PREIMAGE_MATCH] && w[NQ_PREIMAGE_ROWS] == 64 &&
			w[NQ_MARKER_BEFORE_VALID] && w[NQ_MARKER_BEFORE] == 0xffff && !w[NQ_A0_CHANGE_ATTEMPTED] &&
			w[NQ_A0_CURRENT_VALID] && w[NQ_A0_CURRENT] == 0x7c && w[NQ_WEL_VALID] && !w[NQ_WEL];
	}
	if (op == TRANSPORT_QUALIFY_WREN) {
		return w[NQ_A0_CURRENT_VALID] && w[NQ_A0_CURRENT] == 0x48 && w[NQ_WEL_VALID] && !w[NQ_WEL] &&
			((phase == NQ_PHASE_ERASE && w[NQ_WREN_STARTS] == 0 && w[NQ_PREIMAGE_MATCH]) ||
			 (phase == NQ_PHASE_PROGRAM && w[NQ_WREN_STARTS] == 1 && w[NQ_ERASE_COMPLETED] && w[NQ_BLANK_ROWS] == 64 &&
			  w[NQ_B0_CURRENT_VALID] && w[NQ_B0_CURRENT] == 0x10));
	}
	if (op == TRANSPORT_QUALIFY_ERASE) {
		return phase == NQ_PHASE_ERASE && w[NQ_WREN_STARTS] == 1 && w[NQ_WEL_VALID] && w[NQ_WEL] &&
			w[NQ_A0_CURRENT_VALID] && w[NQ_A0_CURRENT] == 0x48 && w[NQ_PREIMAGE_MATCH] && !w[NQ_ERASE_STARTED];
	}
	if (op == TRANSPORT_QUALIFY_PROGRAM_LOAD || op == TRANSPORT_QUALIFY_PROGRAM_EXECUTE) {
		return phase == NQ_PHASE_PROGRAM && w[NQ_WREN_STARTS] == 2 && w[NQ_WEL_VALID] && w[NQ_WEL] &&
			w[NQ_A0_CURRENT_VALID] && w[NQ_A0_CURRENT] == 0x48 && w[NQ_B0_CURRENT_VALID] && w[NQ_B0_CURRENT] == 0x10 &&
			w[NQ_ERASE_COMPLETED] && w[NQ_BLANK_ROWS] == 64 &&
			(op == TRANSPORT_QUALIFY_PROGRAM_LOAD ? !w[NQ_PROGRAM_LOAD_STARTED] : w[NQ_PROGRAM_LOAD_VALID] && !w[NQ_PROGRAM_STARTED]);
	}
	if (op == TRANSPORT_QUALIFY_LOAD) {
		return !w[NQ_READY_UNKNOWN] && w[NQ_B0_CURRENT_VALID] &&
			((phase == NQ_PHASE_VERIFY && w[NQ_B0_CURRENT] == 0x10 && w[NQ_PROGRAM_COMPLETED]) ||
			 ((phase == NQ_PHASE_PREIMAGE || phase == NQ_PHASE_BLANK || phase == NQ_PHASE_MARKER) && w[NQ_B0_CURRENT] == 0));
	}
	if (op == TRANSPORT_MARKER_FIRST || op == TRANSPORT_MARKER_SECOND || op == TRANSPORT_PAGE0_FIRST) {
		return q->cache_armed && !w[NQ_READY_UNKNOWN] && w[NQ_B0_CURRENT_VALID] &&
			(op == TRANSPORT_PAGE0_FIRST ? phase == NQ_PHASE_VERIFY && w[NQ_B0_CURRENT] == 0x10 :
			 (phase == NQ_PHASE_PREIMAGE || phase == NQ_PHASE_BLANK || phase == NQ_PHASE_MARKER) && w[NQ_B0_CURRENT] == 0);
	}
	return false;
}

static int qualify_before_start(int64_t deadline)
{
	struct nand_qualify_context *q = qualify_active; uint32_t *w = q->result->words;
	if (q->restoring) { return k_uptime_get() < deadline ? 0 : -ETIMEDOUT; }
	int rc;
	if (!q->clock_started) {
		q->data_deadline_ms = k_uptime_get() + NAND_QUALIFY_DATA_MS;
		rc = q->observer->start(q->observer->user, q->data_deadline_ms);
		if (rc != 0) { w[NQ_OBSERVER_RC] = (uint32_t)rc; return qualify_fail(NQ_OBSERVER_ERROR, rc); }
	}
	if (k_uptime_get() >= deadline || k_uptime_get() >= q->data_deadline_ms) { return qualify_fail(NQ_DEADLINE_ERROR, -ETIMEDOUT); }
	rc = q->observer->check(q->observer->user, q->data_deadline_ms);
	if (rc != 0) { w[NQ_OBSERVER_RC] = (uint32_t)rc; return qualify_fail(NQ_OBSERVER_ERROR, rc); }
	int64_t now = k_uptime_get();
	if (now >= deadline || now >= q->data_deadline_ms) { return qualify_fail(NQ_DEADLINE_ERROR, -ETIMEDOUT); }
	if (!q->clock_started) { q->first_start_ms = now; q->clock_started = true; }
	return 0;
}

static int qualify_step(enum nand_transport_operation op, int64_t deadline)
{
	struct nand_qualify_context *q = qualify_active; uint32_t *w = q->result->words;
	struct nand_page0_attempt a = {0}; bool started = false;
	w[NQ_LAST_OPERATION] = op;
	int rc = transfer_fixed(1, op, &a, deadline, &started);
	w[NQ_LAST_STARTED] = started; w[NQ_LAST_EXPECTED_LENGTH] = a.expected_length;
	w[NQ_LAST_TX_AMOUNT] = a.tx_amount; w[NQ_LAST_RX_AMOUNT] = a.rx_amount;
	w[NQ_LAST_RC] = (uint32_t)rc; w[NQ_LAST_CONTROL_VALID] = rc == 0 && a.raw_valid;
	w[NQ_LAST_CONTROL] = w[NQ_LAST_CONTROL_VALID] ? a.rx[2] : 0;
	if (started) {
		++w[NQ_TRANSFERS];
		if (op == TRANSPORT_QUALIFY_UNLOCK) { w[NQ_A0_CHANGE_ATTEMPTED] = 1; w[NQ_A0_RESTORE_REQUIRED] = 1; }
		if (op == TRANSPORT_QUALIFY_LOCK) { w[NQ_A0_RESTORE_ATTEMPTED] = 1; }
		if (op == TRANSPORT_QUALIFY_UNLOCK || op == TRANSPORT_QUALIFY_LOCK) { w[NQ_A0_CURRENT_VALID] = 0; w[NQ_A0_CURRENT] = 0; }
		if (op == TRANSPORT_MARKER_OFF) { ++w[NQ_B0_OFF_STARTS]; w[NQ_B0_RESTORE_REQUIRED] = 1; }
		if (op == TRANSPORT_MARKER_ON) { ++w[NQ_B0_ON_STARTS]; if (q->restoring) { w[NQ_B0_RESTORE_ATTEMPTED] = 1; } }
		if (op == TRANSPORT_MARKER_ON || op == TRANSPORT_MARKER_OFF) { w[NQ_B0_CURRENT_VALID] = 0; w[NQ_B0_CURRENT] = 0; }
		if (op == TRANSPORT_QUALIFY_WREN) { ++w[NQ_WREN_STARTS]; w[NQ_WEL_VALID] = 0; w[NQ_WEL] = 0; }
		if (op == TRANSPORT_QUALIFY_WRDI) { w[NQ_WRDI_ATTEMPTED] = 1; w[NQ_WEL_VALID] = 0; w[NQ_WEL] = 0; }
		if (op == TRANSPORT_QUALIFY_LOAD || op == TRANSPORT_QUALIFY_ERASE || op == TRANSPORT_QUALIFY_PROGRAM_EXECUTE) {
			w[NQ_READY_UNKNOWN] = 1; q->cache_armed = false;
		}
		if (op == TRANSPORT_QUALIFY_ERASE) { w[NQ_ERASE_STARTED] = 1; w[NQ_ARRAY_MAY_HAVE_CHANGED] = 1; w[NQ_WEL_VALID] = 0; }
		if (op == TRANSPORT_QUALIFY_PROGRAM_EXECUTE) { w[NQ_PROGRAM_STARTED] = 1; w[NQ_ARRAY_MAY_HAVE_CHANGED] = 1; w[NQ_WEL_VALID] = 0; }
		if (op == TRANSPORT_QUALIFY_PROGRAM_LOAD) { w[NQ_PROGRAM_LOAD_STARTED] = 1; }
		if (op == TRANSPORT_MARKER_FIRST || op == TRANSPORT_MARKER_SECOND || op == TRANSPORT_PAGE0_FIRST) { q->cache_armed = false; }
	}
	if (op == TRANSPORT_QUALIFY_PROGRAM_LOAD) { w[NQ_PROGRAM_LOAD_TX] = a.tx_amount; w[NQ_PROGRAM_LOAD_RX] = a.rx_amount; }
	if (rc == 0) {
		if (op == TRANSPORT_MARKER_A0) { w[NQ_A0_CURRENT_VALID] = 1; w[NQ_A0_CURRENT] = a.rx[2]; }
		if (op == TRANSPORT_CONFIG) { w[NQ_B0_CURRENT_VALID] = 1; w[NQ_B0_CURRENT] = a.rx[2]; }
		if (op == TRANSPORT_FINAL || op == TRANSPORT_INITIAL) { w[NQ_WEL_VALID] = 1; w[NQ_WEL] = (a.rx[2] >> 1) & 1U; }
		if (op == TRANSPORT_QUALIFY_ERASE) { w[NQ_ERASE_TRANSFER_VALID] = 1; }
		if (op == TRANSPORT_QUALIFY_PROGRAM_LOAD) { w[NQ_PROGRAM_LOAD_VALID] = 1; }
		if (op == TRANSPORT_QUALIFY_PROGRAM_EXECUTE) { w[NQ_PROGRAM_TRANSFER_VALID] = 1; }
	}
	if (rc != 0 && !q->restoring) { return qualify_fail(atomic_get(&stopped) ? NQ_TRANSFER_ERROR : NQ_STOP_ERROR, rc); }
	return rc;
}

static int qualify_status(int64_t deadline, uint8_t mask, uint8_t want,
			 enum nand_qualify_outcome error)
{
	int rc = qualify_step(TRANSPORT_FINAL, deadline);
	if (rc != 0) { return rc; }
	uint8_t value = (uint8_t)qualify_words()[NQ_LAST_CONTROL];
	if (value & 0x81U) { qualify_words()[NQ_READY_UNKNOWN] = 1; }
	if ((value & mask) != want) { return qualify_fail(error, -EPROTO); }
	return 0;
}

static int qualify_config(int64_t deadline, bool a0, uint8_t expected)
{
	int rc = qualify_step(a0 ? TRANSPORT_MARKER_A0 : TRANSPORT_CONFIG, deadline);
	if (rc != 0) { return rc; }
	if (qualify_words()[NQ_LAST_CONTROL] != expected) { return qualify_fail(a0 ? NQ_PROTECTION_ERROR : NQ_CONFIG_ERROR, -EPROTO); }
	return 0;
}

static int qualify_event(enum nand_qualify_event event)
{
	struct nand_qualify_context *q = qualify_active; uint32_t *w = q->result->words;
	if (!qualify_safe()) { return qualify_fail(NQ_PRECHECK_ERROR, -EBUSY); }
	if (k_uptime_get() >= q->data_deadline_ms) { return qualify_fail(NQ_DEADLINE_ERROR, -ETIMEDOUT); }
	if (w[NQ_EVENT_COUNT] >= 256U) { return qualify_fail(NQ_OBSERVER_ERROR, -EOVERFLOW); }
	w[NQ_EVENT] = event; ++w[NQ_EVENT_COUNT]; qualify_snapshot(q);
	int64_t limit = k_uptime_get() + 2000; if (limit > q->data_deadline_ms) { limit = q->data_deadline_ms; }
	int rc = q->observer->event(q->observer->user, q->result, q->data_deadline_ms);
	if (rc != 0) { w[NQ_OBSERVER_RC] = (uint32_t)rc; return qualify_fail(NQ_OBSERVER_ERROR, rc); }
	if (k_uptime_get() >= limit) { return qualify_fail(NQ_DEADLINE_ERROR, -ETIMEDOUT); }
	return 0;
}

static int qualify_phase(enum nand_qualify_phase phase)
{
	uint32_t *w = qualify_words(); w[NQ_PHASE] = phase;
	w[NQ_ROW_INDEX] = phase == NQ_PHASE_ERASE || phase == NQ_PHASE_PROGRAM ? 0 : UINT32_MAX;
	return qualify_event(NQ_EVENT_PHASE);
}

static int qualify_ready(int64_t limit, bool page, bool program)
{
	struct nand_qualify_context *q = qualify_active; uint32_t *w = q->result->words;
	if (limit > q->ready_deadline_ms) { limit = q->ready_deadline_ms; }
	unsigned cap = page ? 8U : program ? 32U : 128U;
	unsigned field = page ? w[NQ_PHASE] == NQ_PHASE_PREIMAGE ? NQ_PREIMAGE_POLLS :
		w[NQ_PHASE] == NQ_PHASE_BLANK ? NQ_BLANK_POLLS : w[NQ_PHASE] == NQ_PHASE_VERIFY ? NQ_VERIFY_POLLS : NQ_MARKER_POLLS :
		program ? NQ_PROGRAM_POLLS : NQ_ERASE_POLLS;
	for (unsigned poll = 0; poll < cap; ++poll) {
		if (k_uptime_get() >= limit) { return qualify_fail(NQ_READY_TIMEOUT, -ETIMEDOUT); }
		int rc = qualify_step(TRANSPORT_FINAL, limit);
		if (w[NQ_LAST_STARTED]) { ++w[field]; }
		if (rc != 0) { return rc; }
		uint8_t value = (uint8_t)w[NQ_LAST_CONTROL];
		if (!page) {
			w[program ? NQ_PROGRAM_READY_VALID : NQ_ERASE_READY_VALID] = 1;
			w[program ? NQ_PROGRAM_STATUS : NQ_ERASE_STATUS] = value;
		}
		/* A completed failed array operation is idle, not successful. Sticky
		 * failure flags must not prevent later guarded protection restoration. */
		if (!(value & 0x81U)) { w[NQ_READY_UNKNOWN] = 0; }
		if (value & 0x80U) { return qualify_fail(NQ_PRECHECK_ERROR, -EPROTO); }
		if (value & 1U) { k_busy_wait(page || program ? 100 : 500); continue; }
		if ((value & 0x0eU) != 0) { return qualify_fail(page ? NQ_PRECHECK_ERROR : program ? NQ_PROGRAM_ERROR : NQ_ERASE_ERROR, -EIO); }
		if (page && w[NQ_PHASE] == NQ_PHASE_VERIFY && (value & 0x70U)) { return qualify_fail(NQ_ECC_ERROR, -EIO); }
		if (page) { q->cache_armed = true; }
		else { w[program ? NQ_PROGRAM_COMPLETED : NQ_ERASE_COMPLETED] = 1; }
		return 0;
	}
	return qualify_fail(NQ_READY_TIMEOUT, -ETIMEDOUT);
}

static void qualify_wipe(void)
{
	if (!atomic_get(&stopped)) { return; }
	block1024_wipe();
	volatile uint8_t *a = nand_page0_rx_first, *b = nand_page0_rx_second;
	for (size_t i = 0; i < NAND_PAGE0_CACHE_BYTES; ++i) { a[i] = 0; b[i] = 0; }
	atomic_set(&page0_ram_scrubbed, 1);
}

static int qualify_read(bool second, bool main_only, int64_t deadline, bool config_after)
{
	if (qualify_step(TRANSPORT_QUALIFY_LOAD, deadline) != 0 || qualify_ready(deadline, true, false) != 0) { return (int32_t)qualify_words()[NQ_PRIMARY_RC]; }
	if (qualify_step(main_only ? TRANSPORT_PAGE0_FIRST : second ? TRANSPORT_MARKER_SECOND : TRANSPORT_MARKER_FIRST, deadline) != 0) { return (int32_t)qualify_words()[NQ_PRIMARY_RC]; }
	if (qualify_status(deadline, main_only ? 0xff : 0x8f, 0, main_only ? NQ_ECC_ERROR : NQ_PRECHECK_ERROR) != 0) { return (int32_t)qualify_words()[NQ_PRIMARY_RC]; }
	return config_after ? qualify_config(deadline, false, main_only ? 0x10 : 0) : 0;
}

static uint32_t qualify_le32(const uint8_t *p)
{ return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static int qualify_hash_finish(psa_hash_operation_t *hash, const uint8_t expected[32])
{
	uint8_t digest[32] = {0}; size_t bytes = 0;
	psa_status_t rc = psa_hash_finish(hash, digest, sizeof(digest), &bytes);
	bool match = rc == PSA_SUCCESS && bytes == sizeof(digest) && memcmp(digest, expected, 32) == 0;
	volatile uint8_t *wipe = digest; for (size_t i = 0; i < sizeof(digest); ++i) { wipe[i] = 0; }
	if (rc != PSA_SUCCESS || bytes != 32U) { return qualify_fail(NQ_HASH_ERROR, -EIO); }
	return match ? 0 : qualify_fail(NQ_PREIMAGE_ERROR, -EIO);
}

static int qualify_preimage(struct nand_qualify_context *q)
{
	uint32_t *w = q->result->words;
	int64_t phase_end = k_uptime_get() + 90000; if (phase_end > q->data_deadline_ms) { phase_end = q->data_deadline_ms; }
	if (psa_hash_setup(&q->whole_hash, PSA_ALG_SHA_256) != PSA_SUCCESS) { return qualify_fail(NQ_HASH_ERROR, -EIO); }
	for (unsigned row = 0; row < 64; ++row) {
		q->row_index = (uint8_t)row; w[NQ_ROW_INDEX] = row;
		int64_t limit = k_uptime_get() + 1500; if (limit > phase_end) { limit = phase_end; }
		if (qualify_read(false, false, limit, false) != 0 || qualify_read(true, false, limit, true) != 0) { return (int32_t)w[NQ_PRIMARY_RC]; }
		if (!qualify_safe() || memcmp(nand_marker_rx_first + 4, nand_marker_rx_second + 4, 4352) != 0) { return qualify_fail(NQ_PREIMAGE_ERROR, -EIO); }
		if (row == 0) {
			w[NQ_MARKER_BEFORE_VALID] = 1;
			w[NQ_MARKER_BEFORE] = (uint32_t)nand_marker_rx_first[4100] << 8 | nand_marker_rx_first[4101];
			if (w[NQ_MARKER_BEFORE] != 0xffff) { return qualify_fail(NQ_MARKER_ERROR, -EIO); }
		}
		const uint8_t *binding = nand_qualify_rows + row * 44U;
		if (qualify_le32(binding) != 65536U + row || qualify_le32(binding + 4) != 4352U ||
		    marker_crc32(nand_marker_rx_first + 4) != qualify_le32(binding + 8)) { return qualify_fail(NQ_PREIMAGE_ERROR, -EIO); }
		if (psa_hash_setup(&q->row_hash, PSA_ALG_SHA_256) != PSA_SUCCESS ||
		    psa_hash_update(&q->row_hash, nand_marker_rx_first + 4, 4352) != PSA_SUCCESS ||
		    psa_hash_update(&q->whole_hash, nand_marker_rx_first + 4, 4352) != PSA_SUCCESS) { return qualify_fail(NQ_HASH_ERROR, -EIO); }
		if (qualify_hash_finish(&q->row_hash, binding + 12) != 0) { return (int32_t)w[NQ_PRIMARY_RC]; }
		++w[NQ_PREIMAGE_ROWS]; qualify_wipe();
		if (k_uptime_get() >= limit || k_uptime_get() >= phase_end) { return qualify_fail(NQ_DEADLINE_ERROR, -ETIMEDOUT); }
		if ((row & 7U) == 7U && qualify_event(NQ_EVENT_PROGRESS) != 0) { return (int32_t)w[NQ_PRIMARY_RC]; }
	}
	if (qualify_hash_finish(&q->whole_hash, nand_qualify_preimage_sha) != 0) { return (int32_t)w[NQ_PRIMARY_RC]; }
	w[NQ_PREIMAGE_HASH_VALID] = 1; w[NQ_PREIMAGE_MATCH] = 1;
	return 0;
}

static int qualify_scan(struct nand_qualify_context *q, bool verify)
{
	uint32_t *w = q->result->words;
	int64_t phase_end = k_uptime_get() + 45000; if (phase_end > q->data_deadline_ms) { phase_end = q->data_deadline_ms; }
	for (unsigned index = 0; index < (verify ? 65U : 64U); ++index) {
		unsigned row = verify && index != 0 ? index - 1U : index;
		q->row_index = (uint8_t)row; w[NQ_ROW_INDEX] = row;
		int64_t limit = k_uptime_get() + 1500; if (limit > phase_end) { limit = phase_end; }
		if (qualify_read(false, verify, limit, true) != 0) { return (int32_t)w[NQ_PRIMARY_RC]; }
		const uint8_t *data = verify ? nand_page0_rx_first + 4 : nand_marker_rx_first + 4;
		if (!qualify_safe()) { return qualify_fail(NQ_PRECHECK_ERROR, -EBUSY); }
		for (unsigned i = 0; i < (verify ? 4096U : 4352U); ++i) {
			uint8_t expected_byte = verify && row == 0 ? nand_qualify_program_tx[i + 3U] : 0xff;
			if (data[i] != expected_byte) { return qualify_fail(verify ? NQ_READBACK_ERROR : NQ_BLANK_ERROR, -EIO); }
		}
		++w[verify ? NQ_VERIFY_READS : NQ_BLANK_ROWS];
		if (verify && row == 0) { ++w[NQ_PATTERN_READS]; }
		qualify_wipe();
		if (k_uptime_get() >= limit || k_uptime_get() >= phase_end) { return qualify_fail(NQ_DEADLINE_ERROR, -ETIMEDOUT); }
		if (((index & 7U) == 7U || (verify && index == 64U)) && qualify_event(NQ_EVENT_PROGRESS) != 0) { return (int32_t)w[NQ_PRIMARY_RC]; }
	}
	return 0;
}

static int qualify_run(struct nand_qualify_context *q)
{
	uint32_t *w = q->result->words; w[NQ_PHASE] = NQ_PHASE_PREFLIGHT;
	int64_t provisional = k_uptime_get() + NAND_QUALIFY_DATA_MS;
	for (unsigned i = 0; i < 3; ++i) {
		if (qualify_step(TRANSPORT_ID, provisional) != 0) { return (int32_t)w[NQ_PRIMARY_RC]; }
		if (!expected(nand_id_rx)) { return qualify_fail(NQ_ID_ERROR, -EPROTO); }
	}
	w[NQ_ID_VALID] = 1;
	int64_t d = q->data_deadline_ms;
	if (qualify_status(d, 0xff, 0, NQ_PRECHECK_ERROR) != 0) { return (int32_t)w[NQ_PRIMARY_RC]; }
	w[NQ_INITIAL_VALID] = 1; w[NQ_INITIAL_C0] = w[NQ_LAST_CONTROL];
	if (qualify_config(d, true, 0x7c) != 0) { return (int32_t)w[NQ_PRIMARY_RC]; }
	w[NQ_INITIAL_A0_VALID] = 1; w[NQ_INITIAL_A0] = 0x7c;
	if (qualify_config(d, false, 0x10) != 0) { return (int32_t)w[NQ_PRIMARY_RC]; }
	w[NQ_INITIAL_B0_VALID] = 1; w[NQ_INITIAL_B0] = 0x10;
	if (qualify_status(d, 0xff, 0, NQ_PRECHECK_ERROR) != 0 || qualify_step(TRANSPORT_MARKER_OFF, d) != 0 ||
	    qualify_config(d, false, 0) != 0 || qualify_status(d, 0x8f, 0, NQ_PRECHECK_ERROR) != 0) { return (int32_t)w[NQ_PRIMARY_RC]; }
	if (qualify_phase(NQ_PHASE_PREIMAGE) != 0 || qualify_preimage(q) != 0) { return (int32_t)w[NQ_PRIMARY_RC]; }
	/* Also verify the public program artifact through the configured provider. */
	if (psa_hash_setup(&q->row_hash, PSA_ALG_SHA_256) != PSA_SUCCESS ||
	    psa_hash_update(&q->row_hash, nand_qualify_program_tx + 3, 4096) != PSA_SUCCESS) { return qualify_fail(NQ_HASH_ERROR, -EIO); }
	if (qualify_hash_finish(&q->row_hash, nand_qualify_pattern_sha) != 0) { return (int32_t)w[NQ_PRIMARY_RC]; }
	if (qualify_phase(NQ_PHASE_PROTECTION) != 0 || qualify_status(d, 0x8f, 0, NQ_PRECHECK_ERROR) != 0 ||
	    qualify_step(TRANSPORT_QUALIFY_UNLOCK, d) != 0 || qualify_config(d, true, 0x48) != 0) { return (int32_t)w[NQ_PRIMARY_RC]; }
	w[NQ_A0_CHANGE_CONFIRMED] = 1;
	if (qualify_phase(NQ_PHASE_ERASE) != 0 || qualify_status(d, 0x8f, 0, NQ_PRECHECK_ERROR) != 0 ||
	    qualify_event(NQ_EVENT_BEFORE_ERASE_WREN) != 0 || qualify_step(TRANSPORT_QUALIFY_WREN, d) != 0 ||
	    qualify_status(d, 0x8f, 2, NQ_WEL_ERROR) != 0 || qualify_event(NQ_EVENT_BEFORE_ERASE_EXECUTE) != 0 ||
	    qualify_step(TRANSPORT_QUALIFY_ERASE, d) != 0 || qualify_ready(d, false, false) != 0 ||
	    qualify_event(NQ_EVENT_AFTER_ERASE) != 0) { return (int32_t)w[NQ_PRIMARY_RC]; }
	if (qualify_phase(NQ_PHASE_BLANK) != 0 || qualify_scan(q, false) != 0 || qualify_phase(NQ_PHASE_ECC_ON) != 0 ||
	    qualify_status(d, 0x8f, 0, NQ_PRECHECK_ERROR) != 0 || qualify_step(TRANSPORT_MARKER_ON, d) != 0 ||
	    qualify_config(d, false, 0x10) != 0) { return (int32_t)w[NQ_PRIMARY_RC]; }
	q->row_index = 0;
	if (qualify_phase(NQ_PHASE_PROGRAM) != 0 || qualify_status(d, 0x8f, 0, NQ_PRECHECK_ERROR) != 0 ||
	    qualify_event(NQ_EVENT_BEFORE_PROGRAM_WREN) != 0 || qualify_step(TRANSPORT_QUALIFY_WREN, d) != 0 ||
	    qualify_status(d, 0x8f, 2, NQ_WEL_ERROR) != 0 || qualify_event(NQ_EVENT_BEFORE_PROGRAM_LOAD) != 0 ||
	    qualify_step(TRANSPORT_QUALIFY_PROGRAM_LOAD, d) != 0 || qualify_status(d, 0x8f, 2, NQ_WEL_ERROR) != 0 ||
	    qualify_event(NQ_EVENT_BEFORE_PROGRAM_EXECUTE) != 0 || qualify_step(TRANSPORT_QUALIFY_PROGRAM_EXECUTE, d) != 0 ||
	    qualify_ready(d, false, true) != 0 || qualify_event(NQ_EVENT_AFTER_PROGRAM) != 0) { return (int32_t)w[NQ_PRIMARY_RC]; }
	qualify_wipe();
	if (qualify_phase(NQ_PHASE_VERIFY) != 0 || qualify_scan(q, true) != 0 || qualify_phase(NQ_PHASE_MARKER) != 0 ||
	    qualify_status(d, 0x8f, 0, NQ_PRECHECK_ERROR) != 0 || qualify_step(TRANSPORT_MARKER_OFF, d) != 0 ||
	    qualify_config(d, false, 0) != 0) { return (int32_t)w[NQ_PRIMARY_RC]; }
	q->row_index = 0; w[NQ_ROW_INDEX] = 0;
	int64_t limit = k_uptime_get() + 1500; if (limit > d) { limit = d; }
	if (qualify_read(false, false, limit, true) != 0 || !qualify_safe()) { return qualify_fail(NQ_MARKER_ERROR, -EIO); }
	w[NQ_MARKER_AFTER_VALID] = 1;
	w[NQ_MARKER_AFTER] = (uint32_t)nand_marker_rx_first[4100] << 8 | nand_marker_rx_first[4101];
	if (w[NQ_MARKER_AFTER] != 0xffff) { return qualify_fail(NQ_MARKER_ERROR, -EIO); }
	qualify_wipe();
	if (k_uptime_get() >= limit) { return qualify_fail(NQ_DEADLINE_ERROR, -ETIMEDOUT); }
	return 0;
}

static void qualify_restore(struct nand_qualify_context *q)
{
	uint32_t *w = q->result->words;
	q->restoring = true; w[NQ_PHASE] = NQ_PHASE_RESTORE;
	bool a0 = w[NQ_A0_CHANGE_ATTEMPTED] != 0, b0 = w[NQ_B0_OFF_STARTS] != 0;
	if (!a0 && !b0 && !w[NQ_WREN_STARTS]) { return; }
	if (!qualify_safe()) {
		if (a0) { w[NQ_A0_RESTORE_STATE] = NQ_RESTORE_SKIPPED_UNSAFE; w[NQ_A0_RESTORE_RC] = (uint32_t)-EBUSY; }
		if (b0) { w[NQ_B0_RESTORE_STATE] = NQ_RESTORE_SKIPPED_UNSAFE; w[NQ_B0_RESTORE_RC] = (uint32_t)-EBUSY; }
		return;
	}
	int64_t deadline = k_uptime_get() + 100; int rc = qualify_step(TRANSPORT_FINAL, deadline);
	if (rc == 0 && (w[NQ_LAST_CONTROL] & 0x81U)) { w[NQ_READY_UNKNOWN] = 1; rc = -EBUSY; }
	/* Micron p48: WEL clears via04 or successful erase/program. E/P_FAIL may
	 * stay set until the next respective execute/reset; do NOT reset/execute
	 * to clear them. Idle sticky failure still permits guarded restoration. */
	if (rc == 0 && w[NQ_WEL]) {
		rc = qualify_step(TRANSPORT_QUALIFY_WRDI, deadline);
		if (rc == 0) { rc = qualify_step(TRANSPORT_FINAL, deadline); }
		if (rc == 0 && (w[NQ_LAST_CONTROL] & 0x83U)) { rc = -EPROTO; }
		if (rc == 0) { w[NQ_WRDI_VERIFIED] = 1; }
	}
	w[NQ_DISARM_RC] = (uint32_t)rc;
	if (rc != 0) {
		if (a0) { w[NQ_A0_RESTORE_STATE] = NQ_RESTORE_SKIPPED_UNSAFE; w[NQ_A0_RESTORE_RC] = (uint32_t)rc; }
		if (b0) { w[NQ_B0_RESTORE_STATE] = NQ_RESTORE_SKIPPED_UNSAFE; w[NQ_B0_RESTORE_RC] = (uint32_t)rc; }
		return;
	}
	int ar = 0, br = 0;
	if (a0) { w[NQ_A0_RESTORE_STATE] = NQ_RESTORE_FAILED; ar = qualify_step(TRANSPORT_QUALIFY_LOCK, deadline); }
	if (qualify_safe()) {
		int read_rc = qualify_step(TRANSPORT_MARKER_A0, deadline);
		if (read_rc == 0) { w[NQ_A0_RESTORE_VALID] = 1; w[NQ_A0_RESTORE_VALUE] = w[NQ_LAST_CONTROL]; }
		if (ar == 0) { ar = read_rc != 0 ? read_rc : w[NQ_LAST_CONTROL] == 0x7c ? 0 : -EPROTO; }
	} else if (ar == 0) { ar = -EBUSY; }
	w[NQ_A0_RESTORE_RC] = (uint32_t)ar;
	/* A readback mismatch is not proof of unresolved DMA/internal busy. Keep
	 * that failed obligation but attempt the single B0 restore if still safe. */
	if (b0 && qualify_safe()) { w[NQ_B0_RESTORE_STATE] = NQ_RESTORE_FAILED; br = qualify_step(TRANSPORT_MARKER_ON, deadline); }
	else if (b0) { w[NQ_B0_RESTORE_STATE] = NQ_RESTORE_SKIPPED_UNSAFE; br = -EBUSY; }
	if (qualify_safe()) {
		int read_rc = qualify_step(TRANSPORT_CONFIG, deadline);
		if (read_rc == 0) { w[NQ_B0_RESTORE_VALID] = 1; w[NQ_B0_RESTORE_VALUE] = w[NQ_LAST_CONTROL]; }
		if (br == 0) { br = read_rc != 0 ? read_rc : w[NQ_LAST_CONTROL] == 0x10 ? 0 : -EPROTO; }
	} else if (br == 0) { br = -EBUSY; }
	w[NQ_B0_RESTORE_RC] = (uint32_t)br;
	if (qualify_safe()) {
		rc = qualify_step(TRANSPORT_FINAL, deadline);
		if (rc == 0) {
			w[NQ_FINAL_VALID] = 1; w[NQ_FINAL_C0] = w[NQ_LAST_CONTROL];
			if (w[NQ_FINAL_C0] & 0x81U) { w[NQ_READY_UNKNOWN] = 1; }
			if (w[NQ_FINAL_C0] & 0x83U) { rc = -EPROTO; }
		}
	} else { rc = -EBUSY; }
	if (ar == 0 && rc == 0) { if (a0) { w[NQ_A0_RESTORE_STATE] = NQ_RESTORE_VERIFIED; } w[NQ_A0_RESTORE_REQUIRED] = 0; }
	else if (ar == 0) { w[NQ_A0_RESTORE_RC] = (uint32_t)rc; }
	if (br == 0 && rc == 0) { if (b0) { w[NQ_B0_RESTORE_STATE] = NQ_RESTORE_VERIFIED; } w[NQ_B0_RESTORE_REQUIRED] = 0; }
	else if (br == 0) { w[NQ_B0_RESTORE_RC] = (uint32_t)rc; }
	if (rc != 0 && !w[NQ_DISARM_RC]) { w[NQ_DISARM_RC] = (uint32_t)rc; }
}

static void qualify_finish(struct nand_qualify_context *q)
{
	uint32_t *w = q->result->words;
	/* Checked provider cleanup; no DMA pointers handed to provider. Never
	 * inspect/wipe unresolved DMA even if provider cleanup itself fails. */
	psa_status_t row_rc = psa_hash_abort(&q->row_hash), whole_rc = psa_hash_abort(&q->whole_hash);
	if (row_rc != PSA_SUCCESS || whole_rc != PSA_SUCCESS) { (void)qualify_fail(NQ_HASH_ERROR, -EIO); }
	qualify_wipe(); qualify_snapshot(q);
	bool restore_bad = w[NQ_A0_RESTORE_REQUIRED] || w[NQ_B0_RESTORE_REQUIRED] || w[NQ_DISARM_RC];
	if (restore_bad || w[NQ_READY_UNKNOWN]) { atomic_set(&fault, 1); }
	if (!w[NQ_STOPPED]) { w[NQ_OUTCOME] = NQ_STOP_ERROR; w[NQ_RC] = (uint32_t)-EBUSY; }
	else if (restore_bad || w[NQ_READY_UNKNOWN]) { w[NQ_OUTCOME] = NQ_RESTORE_ERROR; w[NQ_RC] = (uint32_t)-EIO; }
	else if (atomic_get(&fault) || !w[NQ_BUS_RELEASED] || !w[NQ_RAM_SCRUBBED]) { w[NQ_OUTCOME] = NQ_CLEANUP_ERROR; w[NQ_RC] = (uint32_t)-EIO; }
	else if (w[NQ_PRIMARY_OUTCOME]) { w[NQ_OUTCOME] = w[NQ_PRIMARY_OUTCOME]; w[NQ_RC] = w[NQ_PRIMARY_RC]; }
	else if (w[NQ_PREIMAGE_MATCH] && w[NQ_PREIMAGE_ROWS] == 64 && w[NQ_ERASE_COMPLETED] && w[NQ_BLANK_ROWS] == 64 &&
		 w[NQ_PROGRAM_COMPLETED] && w[NQ_VERIFY_READS] == 65 && w[NQ_PATTERN_READS] == 2 &&
		 w[NQ_MARKER_AFTER_VALID] && w[NQ_MARKER_AFTER] == 0xffff && w[NQ_FINAL_VALID] && !(w[NQ_FINAL_C0] & 0x8fU) &&
		 w[NQ_A0_RESTORE_STATE] == NQ_RESTORE_VERIFIED && w[NQ_B0_RESTORE_STATE] == NQ_RESTORE_VERIFIED &&
		 w[NQ_A0_CURRENT_VALID] && w[NQ_A0_CURRENT] == 0x7c && w[NQ_B0_CURRENT_VALID] && w[NQ_B0_CURRENT] == 0x10 &&
		 w[NQ_WREN_STARTS] == 2 && w[NQ_PROGRAM_LOAD_VALID] && w[NQ_A0_CHANGE_CONFIRMED]) {
		w[NQ_OUTCOME] = NQ_VERIFIED; w[NQ_PRIMARY_OUTCOME] = NQ_VERIFIED; w[NQ_PHASE] = NQ_PHASE_DONE;
	} else { (void)qualify_fail(NQ_CLEANUP_ERROR, -EIO); w[NQ_OUTCOME] = NQ_CLEANUP_ERROR; w[NQ_RC] = (uint32_t)-EIO; }
	qualify_snapshot(q); q->common.rc = (int32_t)w[NQ_RC];
}
