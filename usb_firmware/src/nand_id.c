/* SPDX-License-Identifier: Apache-2.0
 * Explicit, shared one-shot, fixed-command NAND diagnostics. Not a NAND driver.
 */
#include "nand_id.h"
#include "nand_public_object.h"
#ifdef OPENPENDANT_PUBLIC_OBJECT
#include "nand_public_object_internal.h"
#endif

#include <errno.h>
#include <stddef.h>
#include <string.h>
#include <soc.h>
#include <hal/nrf_gpio.h>
#include <hal/nrf_spim.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>
#include <psa/crypto.h>

#define NAND_ID_SCK 40U
#define NAND_ID_MOSI 41U
#define NAND_ID_MISO 42U
#define NAND_ID_EVENT_US 2000U
#define NAND_ID_POLL_LIMIT 4096U
#define NAND_ID_TOTAL_MS 75U
/* Qualified Micron M70A ABBFD12 family, Rev. I p.54: first access at 1.8 V
 * requires 2 ms after VCC reaches its minimum. Allow 5 ms after P1.07 HIGH;
 * its rail role/rise time and the exact fitted suffix remain unconfirmed.
 * This allowance does not establish that physical VCC timing is satisfied. */
#define NAND_ID_AUX_SETTLE_US 5000U

BUILD_ASSERT(DT_REG_ADDR(DT_NODELABEL(spi4)) == 0x5000a000UL,
	     "Only factory SPIM4 is permitted");
BUILD_ASSERT(!DT_NODE_HAS_STATUS(DT_NODELABEL(spi4), okay),
	     "No driver may own the diagnostic peripheral");
BUILD_ASSERT(!IS_ENABLED(CONFIG_SPI) && !IS_ENABLED(CONFIG_NRFX_SPIM4),
	     "This diagnostic uses bounded direct HAL, not an SPI driver");
BUILD_ASSERT(NRF_SPIM_FREQ_125K == 0x02000000UL && NAND_ID_MAX_ATTEMPTS == 6,
	     "Diagnostic frequency/transfer bounds changed");

static NRF_SPIM_Type *const nand_id_spim =
	(NRF_SPIM_Type *)DT_REG_ADDR(DT_NODELABEL(spi4));
static const uint32_t nand_id_cs_pins[4] __attribute__((used)) = {45, 47, 43, 44};
static const uint32_t nand_id_bus_pins[3] __attribute__((used)) = {40, 41, 42};
/* Factory descriptor0x7c168: P1.07, output-high. Its electrical role is still
 * unresolved. Only the explicit diagnostic configures it; never drive low. */
static const uint32_t nand_id_aux_pin __attribute__((used, retain)) = 39U;
/* DMA storage has permanent lifetime, including after a failed STOP. Never
 * reuse/clear/disconnect it while hardware ownership is unresolved. */
static __aligned(4) uint8_t nand_id_tx[4] = {0x9f, 0, 0, 0};
static __aligned(4) uint8_t nand_id_rx[4];
struct nand_fixed_operation {
	uint8_t tx[4];
	uint8_t length;
	uint8_t reg;
};
/* Exact allowlist. The last clocked zero of GET FEATURE is not write data.
 * Only the fixed program below selects these; none are caller parameters. */
static const struct nand_fixed_operation nand_fixed_operations[4]
	__attribute__((used, retain)) = {
	{{0x9f, 0x00, 0x00, 0x00}, 4, 0x00},
	{{0x0f, 0xc0, 0x00, 0x00}, 3, 0xc0},
	{{0x0f, 0xb0, 0x00, 0x00}, 3, 0xb0},
	{{0x0f, 0xc0, 0x00, 0x00}, 3, 0xc0},
};
static const enum nand_feature_operation nand_feature_program[6]
	__attribute__((used, retain)) = {
	NAND_FEATURE_OP_ID, NAND_FEATURE_OP_ID, NAND_FEATURE_OP_ID,
	NAND_FEATURE_OP_INITIAL_STATUS, NAND_FEATURE_OP_CONFIG,
	NAND_FEATURE_OP_FINAL_STATUS,
};
/* Closed page0 program. The opcode/header and all addresses are constants.
 * Cache TX is RAM for EasyDMA, but immutable after static initialization. */
static const uint8_t nand_page0_load_tx[4] __attribute__((used, retain)) = {0x13, 0, 0, 0};
static const uint8_t nand_page0_prefix[7] __attribute__((used, retain)) = {0, 0, 0, 1, 2, 3, 4};
static const uint8_t nand_page0_suffix[6] __attribute__((used, retain)) = {6, 7, 8, 9, 10, 11};
static const uint8_t nand_page0_transport[12] __attribute__((used, retain)) =
	{0, 1, 2, 3, 4, 3, 5, 3, 6, 3, 2, 3};
/* row, column, main bytes, cache length, START cap, ready polls, ready us,
 * cache END us, cache event polls, cache poll us, overall ms, busy-gap us. */
static const uint32_t nand_page0_bounds[12] __attribute__((used, retain)) =
	{0, 0, 4096, 4100, 21, 8, 5000, 350000, 8192, 50, 800, 100};
static __aligned(4) uint8_t nand_page0_tx[NAND_PAGE0_CACHE_BYTES]
	__attribute__((used, retain)) = {0x03, 0, 0, 0};
static __aligned(4) uint8_t nand_page0_rx_first[NAND_PAGE0_CACHE_BYTES];
static __aligned(4) uint8_t nand_page0_rx_second[NAND_PAGE0_CACHE_BYTES];
static atomic_t page0_ram_scrubbed = ATOMIC_INIT(1);
static int64_t page0_ready_deadline;

static const struct nand_fixed_operation nand_marker_operations[4]
	__attribute__((used, retain)) = {
	{{0x0f, 0xa0, 0, 0}, 3, 0xa0}, {{0x1f, 0xb0, 0, 0}, 3, 0xb0},
	{{0x1f, 0xb0, 0x10, 0}, 3, 0xb0}, {{0x13, 1, 0, 0}, 4, 0},
};
static const uint8_t nand_marker_prefix[10] __attribute__((used, retain)) = {0,0,0,1,2,3,4,5,6,7};
static const uint8_t nand_marker_first[3] __attribute__((used, retain)) = {8,10,11};
static const uint8_t nand_marker_second[4] __attribute__((used, retain)) = {12,14,15,16};
static const uint8_t nand_marker_restore[5] __attribute__((used, retain)) = {17,18,19,20,21};
static const uint8_t nand_marker_transport[22] __attribute__((used, retain)) =
	{0,3,7,2,3,8,2,3,10,3,11,3,10,3,12,3,2,3,9,2,7,3};
/* block,row,column,raw,wire,STARTcap,polls,ready_us,cache_us,eventpolls,
 * poll_us,data_ms,restore_ms,busy_gap_us. */
static const uint32_t nand_marker_bounds[14] __attribute__((used, retain)) =
	{1024,65536,0,4352,4356,38,8,5000,350000,8192,50,1500,100,100};
static __aligned(4) uint8_t nand_marker_tx[NAND_MARKER_CACHE_BYTES]
	__attribute__((used, retain)) = {0x03, 0, 0, 0};
static __aligned(4) uint8_t nand_marker_rx_first[NAND_MARKER_CACHE_BYTES];
static __aligned(4) uint8_t nand_marker_rx_second[NAND_MARKER_CACHE_BYTES];
static atomic_t marker_ram_scrubbed = ATOMIC_INIT(1);
static int64_t marker_ready_deadline;

/* Immutable finite row allowlist; only the internal ascending loop selects it.
 * Reuses marker DMA storage under the same one-shot owner, never stack DMA. */
static const uint8_t nand_block1024_loads[64][4] __attribute__((used, retain)) = {
	{0x13,1,0,0},{0x13,1,0,1},{0x13,1,0,2},{0x13,1,0,3},{0x13,1,0,4},{0x13,1,0,5},{0x13,1,0,6},{0x13,1,0,7},{0x13,1,0,8},{0x13,1,0,9},{0x13,1,0,10},{0x13,1,0,11},{0x13,1,0,12},{0x13,1,0,13},{0x13,1,0,14},{0x13,1,0,15},{0x13,1,0,16},{0x13,1,0,17},{0x13,1,0,18},{0x13,1,0,19},{0x13,1,0,20},{0x13,1,0,21},{0x13,1,0,22},{0x13,1,0,23},{0x13,1,0,24},{0x13,1,0,25},{0x13,1,0,26},{0x13,1,0,27},{0x13,1,0,28},{0x13,1,0,29},{0x13,1,0,30},{0x13,1,0,31},{0x13,1,0,32},{0x13,1,0,33},{0x13,1,0,34},{0x13,1,0,35},{0x13,1,0,36},{0x13,1,0,37},{0x13,1,0,38},{0x13,1,0,39},{0x13,1,0,40},{0x13,1,0,41},{0x13,1,0,42},{0x13,1,0,43},{0x13,1,0,44},{0x13,1,0,45},{0x13,1,0,46},{0x13,1,0,47},{0x13,1,0,48},{0x13,1,0,49},{0x13,1,0,50},{0x13,1,0,51},{0x13,1,0,52},{0x13,1,0,53},{0x13,1,0,54},{0x13,1,0,55},{0x13,1,0,56},{0x13,1,0,57},{0x13,1,0,58},{0x13,1,0,59},{0x13,1,0,60},{0x13,1,0,61},{0x13,1,0,62},{0x13,1,0,63}
};
/* block,first,rowcount,raw,wire,maxSTARTs,maxpolls,data_ms,row_ms,restore_ms */
static const uint32_t nand_block1024_bounds[10] __attribute__((used, retain)) =
	{1024,65536,64,4352,4356,1487,8,120000,1500,100};
struct nand_block1024_context {
	struct nand_marker_result marker; /* Bounded scratch, reset between rows. */
	struct nand_block1024_result *result;
	const struct nand_block1024_sink *sink;
	int64_t first_start_ms;
	int64_t data_deadline_ms;
	uint32_t crc_state;
	uint8_t row_index;
	bool clock_started;
};
/* Synchronous metadata context, never passed to DMA; installed only after CAS. */
static struct nand_block1024_context *block_active;
/* The shell stack is small. Entry CAS protects permanent scratch initialization
 * independently of the shared peripheral CAS; none of this scratch is DMA. */
static struct nand_block1024_context block_context;
static atomic_t block_entry;
#include "nand_qualify_constants.inc"
/* Qualification remains a separate, fixed state machine. These declarations
 * are private to this translation unit; no caller chooses transport bytes. */
struct nand_qualify_context {
	struct nand_id_result common;
	struct nand_qualify_result *result;
	const struct nand_qualify_observer *observer;
	psa_hash_operation_t whole_hash;
	psa_hash_operation_t row_hash;
	int64_t first_start_ms, data_deadline_ms, ready_deadline_ms;
	uint8_t row_index;
	bool clock_started, restoring, cache_armed;
};
static struct nand_qualify_context qualify_context;
static struct nand_qualify_context *qualify_active;
static atomic_t qualify_entry;
/* Block/row geometry, START cap, data/preimage/scan/row/restore ms, page
 * polls/us, erase polls/us, program polls/us, page/program and erase gaps us,
 * long END us, checkpoint ms/count, independent STOP us. */
static const uint32_t nand_qualify_bounds[25] __attribute__((used,retain)) =
	{1024,65536,64,4352,4356,4096,4099,3229,180000,90000,45000,1500,
	 100,8,5000,128,100000,32,10000,100,500,350000,2000,256,3000};
static const struct nand_fixed_operation nand_qualify_operations[7]
	__attribute__((used,retain)) = {
	{{0x1f,0xa0,0x48,0},3,0xa0}, {{0x1f,0xa0,0x7c,0},3,0xa0},
	{{0x06,0,0,0},1,0}, {{0x04,0,0,0},1,0},
	{{0xd8,1,0,0},4,0}, {{0x02,0,0,0},3,0}, {{0x10,1,0,0},4,0},
};
enum nand_transport_operation {
	TRANSPORT_ID = 0, TRANSPORT_INITIAL, TRANSPORT_CONFIG, TRANSPORT_FINAL,
	TRANSPORT_PAGE0_LOAD, TRANSPORT_PAGE0_FIRST, TRANSPORT_PAGE0_SECOND,
	TRANSPORT_MARKER_A0, TRANSPORT_MARKER_OFF, TRANSPORT_MARKER_ON,
	TRANSPORT_MARKER_LOAD, TRANSPORT_MARKER_FIRST, TRANSPORT_MARKER_SECOND,
	TRANSPORT_BLOCK1024_LOAD,
	TRANSPORT_QUALIFY_LOAD, TRANSPORT_QUALIFY_UNLOCK, TRANSPORT_QUALIFY_LOCK,
	TRANSPORT_QUALIFY_WREN, TRANSPORT_QUALIFY_WRDI, TRANSPORT_QUALIFY_ERASE,
	TRANSPORT_QUALIFY_PROGRAM_LOAD, TRANSPORT_QUALIFY_PROGRAM_EXECUTE,
};
static bool qualify_allowed(enum nand_transport_operation operation);
static int qualify_before_start(int64_t deadline);
static int qualify_run(struct nand_qualify_context *q);
static void qualify_restore(struct nand_qualify_context *q);
static void qualify_finish(struct nand_qualify_context *q);
BUILD_ASSERT(NAND_QUALIFY_RESULT_WORDS == 98U && NAND_QUALIFY_MAX_STARTS == 3229U &&
	     sizeof(nand_qualify_program_tx) == 4099U && sizeof(nand_qualify_rows) == 2816U,
	     "Fixed qualification contract changed");
BUILD_ASSERT(NAND_BLOCK1024_ROWS == 64U && NAND_BLOCK1024_MAX_TRANSFERS == 1487U &&
	     NB_TRANSPORT_QUARANTINED + 1U == NAND_BLOCK1024_END_WORDS,
	     "Fixed block backup geometry/result contract changed");
BUILD_ASSERT(NAND_PAGE0_CACHE_BYTES == NAND_PAGE0_MAIN_BYTES + 4U &&
	     NAND_PAGE0_CACHE_BYTES <= 0xffffU && NAND_PAGE0_MAX_TRANSFERS == 21U,
	     "Fixed page0 geometry/DMA bound changed");
BUILD_ASSERT(NAND_MARKER_CACHE_BYTES == NAND_MARKER_RAW_BYTES + 4U &&
	     NAND_MARKER_CACHE_BYTES <= 0xffffU && NAND_MARKER_MAX_TRANSFERS == 38U,
	     "Fixed raw marker geometry/DMA bound changed");
BUILD_ASSERT((SPIM_TXD_MAXCNT_MAXCNT_Msk >> SPIM_TXD_MAXCNT_MAXCNT_Pos) == 0xffffU &&
	     (SPIM_RXD_MAXCNT_MAXCNT_Msk >> SPIM_RXD_MAXCNT_MAXCNT_Pos) == 0xffffU,
	     "Pinned SPIM TX/RX MAXCNT fields must remain 16 bits");
static atomic_t attempted;
static atomic_t busy;
static atomic_t fault;
static atomic_t stopped = ATOMIC_INIT(1);
static atomic_t bus_released = ATOMIC_INIT(1);
static atomic_t cs_configured;
static atomic_t cs_high;
static atomic_t aux_configured;
static atomic_t aux_high;

void nand_id_get_state(struct nand_id_state *state)
{
	if (state == NULL) {
		return;
	}
	state->attempted = atomic_get(&attempted) != 0;
	state->busy = atomic_get(&busy) != 0;
	state->fault_latched = atomic_get(&fault) != 0;
	state->clock_stopped = atomic_get(&stopped) != 0;
	state->bus_released = atomic_get(&bus_released) != 0;
	state->cs_configured = atomic_get(&cs_configured) != 0;
	state->cs_high = atomic_get(&cs_high) != 0;
	state->aux_configured = atomic_get(&aux_configured) != 0;
	state->aux_high = atomic_get(&aux_high) != 0;
}

static void result_state(struct nand_id_result *result)
{
	result->clock_stopped = atomic_get(&stopped) != 0;
	result->bus_released = atomic_get(&bus_released) != 0;
	result->cs_high = atomic_get(&cs_high) != 0;
	result->fault_latched = atomic_get(&fault) != 0;
	result->aux_configured = atomic_get(&aux_configured) != 0;
	result->aux_high = atomic_get(&aux_high) != 0;
}

static bool peripheral_idle(void)
{
	return nand_id_spim->ENABLE == 0 && nand_id_spim->SHORTS == 0 &&
	       nand_id_spim->INTENSET == 0 &&
	       nand_id_spim->SUBSCRIBE_START == 0 &&
	       nand_id_spim->SUBSCRIBE_STOP == 0 &&
	       nand_id_spim->SUBSCRIBE_SUSPEND == 0 &&
	       nand_id_spim->SUBSCRIBE_RESUME == 0 &&
	       nand_id_spim->PUBLISH_STARTED == 0 &&
	       nand_id_spim->PUBLISH_END == 0 &&
	       nand_id_spim->PUBLISH_ENDRX == 0 &&
	       nand_id_spim->PUBLISH_ENDTX == 0 &&
	       nand_id_spim->PUBLISH_STOPPED == 0 &&
	       (nand_id_spim->PSEL.SCK & BIT(31)) != 0 &&
	       (nand_id_spim->PSEL.MOSI & BIT(31)) != 0 &&
	       (nand_id_spim->PSEL.MISO & BIT(31)) != 0 &&
	       (nand_id_spim->PSEL.CSN & BIT(31)) != 0 &&
	       (nand_id_spim->PSELDCX & BIT(31)) != 0 &&
	       nand_id_spim->RXD.LIST == 0 && nand_id_spim->TXD.LIST == 0;
}

static bool pins_idle(void)
{
	/* Exact reset-safe state: input, input buffer disconnected, no pull,
	 * standard drive, no sense, application-owned, including auxiliary P1.07. */
	if (NRF_P1->PIN_CNF[nand_id_aux_pin - 32U] != 2U) {
		return false;
	}
	for (size_t i = 0; i < ARRAY_SIZE(nand_id_bus_pins); ++i) {
		if (NRF_P1->PIN_CNF[nand_id_bus_pins[i] - 32U] != 2U) {
			return false;
		}
	}
	for (size_t i = 0; i < ARRAY_SIZE(nand_id_cs_pins); ++i) {
		if (NRF_P1->PIN_CNF[nand_id_cs_pins[i] - 32U] != 2U) {
			return false;
		}
	}
	return true;
}

static bool all_cs_high(void)
{
	bool good = true;
	for (size_t i = 0; i < ARRAY_SIZE(nand_id_cs_pins); ++i) {
		uint32_t pin = nand_id_cs_pins[i];
		nrf_gpio_pin_set(pin);
		good = good && nrf_gpio_pin_out_read(pin) == 1U;
	}
	atomic_set(&cs_high, good);
	return good;
}

static bool observe_aux(void)
{
	bool configured = NRF_P1->PIN_CNF[nand_id_aux_pin - 32U] == 3U;
	bool high = configured && nrf_gpio_pin_out_read(nand_id_aux_pin) == 1U;
	atomic_set(&aux_configured, configured);
	atomic_set(&aux_high, high);
	return high;
}

/* Same workaround and dynamic applicability as pinned nrfx_spim.c. This is
 * the SPIM4 internal erratum135 register, not NAND reset/feature control.
 * Disable it only after STOP is proved and SPIM is disabled. */
static void erratum135(bool enable)
{
	if (NRF_ERRATA_DYNAMIC_CHECK(53, 135)) {
		*(volatile uint32_t *)((uintptr_t)nand_id_spim + 0xc04U) = enable ? 1U : 0U;
	}
}

static bool wait_event(nrf_spim_event_t event, int64_t overall_deadline)
{
	uint32_t begin = k_cycle_get_32();
	uint32_t limit = k_us_to_cyc_ceil32(NAND_ID_EVENT_US);
	/* A stopped software time source cannot turn this into an infinite loop.
	 * Interrupt/scheduler latency can exceed the nominal budgets; no lock or
	 * interrupt masking is used to manufacture a false wall-clock guarantee. */
	for (uint32_t poll = 0; poll < NAND_ID_POLL_LIMIT; ++poll) {
		if ((uint32_t)(k_cycle_get_32() - begin) >= limit ||
		    k_uptime_get() >= overall_deadline) {
			return false;
		}
		if (nrf_spim_event_check(nand_id_spim, event)) {
			return true;
		}
		k_busy_wait(1);
	}
	return false;
}

static bool wait_cache_end(int64_t overall_deadline)
{
	uint32_t begin = k_cycle_get_32();
	uint32_t limit = k_us_to_cyc_ceil32(nand_page0_bounds[7]);
	for (uint32_t poll = 0; poll < nand_page0_bounds[8]; ++poll) {
		if ((uint32_t)(k_cycle_get_32() - begin) >= limit ||
		    k_uptime_get() >= overall_deadline) { return false; }
		if (nrf_spim_event_check(nand_id_spim, NRF_SPIM_EVENT_END)) { return true; }
		k_busy_wait(nand_page0_bounds[9]);
	}
	return false;
}

/* Private, closed enum selects only fixed buffers, lengths and permanent RX.
 * Neither callers nor the USB shell can supply SPI bytes/addresses/pointers. */
static int transfer_fixed(uint8_t index, enum nand_transport_operation operation,
			  struct nand_page0_attempt *attempt,
			  int64_t overall_deadline, bool *started)
{
	*started = false;
	if (index >= ARRAY_SIZE(nand_id_cs_pins) ||
	    (unsigned int)operation > TRANSPORT_QUALIFY_PROGRAM_EXECUTE ||
	    ((unsigned int)operation > TRANSPORT_BLOCK1024_LOAD && qualify_active == NULL) ||
	    (qualify_active != NULL && !qualify_allowed(operation)) ||
	    (operation != TRANSPORT_ID && index != 1U) ||
	    (operation == TRANSPORT_BLOCK1024_LOAD &&
	     (block_active == NULL || block_active->row_index >= NAND_BLOCK1024_ROWS)) ||
	    (block_active != NULL && block_active->result->end[NB_TRANSFERS] >= NAND_BLOCK1024_MAX_TRANSFERS) ||
	    !atomic_get(&stopped)) {
		return -EINVAL;
	}
	bool marker_cache = operation == TRANSPORT_MARKER_FIRST || operation == TRANSPORT_MARKER_SECOND;
	bool program_load = operation == TRANSPORT_QUALIFY_PROGRAM_LOAD;
	bool cache = marker_cache || operation == TRANSPORT_PAGE0_FIRST || operation == TRANSPORT_PAGE0_SECOND;
	bool long_dma = cache || program_load;
	uint8_t *tx = program_load ? nand_qualify_program_tx : marker_cache ? nand_marker_tx : cache ? nand_page0_tx : nand_id_tx;
	uint8_t *rx = operation == TRANSPORT_PAGE0_FIRST ? nand_page0_rx_first :
		operation == TRANSPORT_PAGE0_SECOND ? nand_page0_rx_second :
		operation == TRANSPORT_MARKER_FIRST ? nand_marker_rx_first :
		operation == TRANSPORT_MARKER_SECOND ? nand_marker_rx_second :
		program_load ? nand_marker_rx_first : nand_id_rx;
	const struct nand_fixed_operation *marker_control = operation >= TRANSPORT_MARKER_A0 &&
		operation <= TRANSPORT_MARKER_LOAD ? &nand_marker_operations[operation - TRANSPORT_MARKER_A0] : NULL;
	const struct nand_fixed_operation *q_control = operation >= TRANSPORT_QUALIFY_UNLOCK &&
		operation <= TRANSPORT_QUALIFY_PROGRAM_EXECUTE ?
		&nand_qualify_operations[operation - TRANSPORT_QUALIFY_UNLOCK] : NULL;
	uint16_t length = program_load ? NAND_QUALIFY_PROGRAM_BYTES :
		marker_cache ? NAND_MARKER_CACHE_BYTES : cache ? NAND_PAGE0_CACHE_BYTES :
		q_control != NULL ? q_control->length : operation == TRANSPORT_QUALIFY_LOAD ? 4U :
		marker_control != NULL ? marker_control->length :
		(operation == TRANSPORT_PAGE0_LOAD || operation == TRANSPORT_BLOCK1024_LOAD) ? 4U :
		nand_fixed_operations[operation].length;
	attempt->reg = (unsigned int)operation < ARRAY_SIZE(nand_fixed_operations) ?
		nand_fixed_operations[operation].reg : marker_control != NULL ? marker_control->reg :
		q_control != NULL ? q_control->reg : 0;
	attempt->expected_length = length;
	attempt->cs_pin = (uint8_t)(nand_id_cs_pins[index] - 32U);
	if (k_uptime_get() >= overall_deadline) {
		return -ETIMEDOUT;
	}
	if (!observe_aux()) {
		atomic_set(&fault, 1);
		return -EIO;
	}
	if (!long_dma) {
		memcpy(nand_id_tx, q_control != NULL ? q_control->tx :
		       operation == TRANSPORT_QUALIFY_LOAD ? nand_block1024_loads[qualify_active->row_index] :
		       marker_control != NULL ? marker_control->tx :
		       operation == TRANSPORT_BLOCK1024_LOAD ? nand_block1024_loads[block_active->row_index] :
		       operation == TRANSPORT_PAGE0_LOAD ? nand_page0_load_tx :
		       nand_fixed_operations[operation].tx, sizeof(nand_id_tx));
	}
	memset(rx, 0, long_dma ? length : sizeof(nand_id_rx));
	nrf_spim_event_clear(nand_id_spim, NRF_SPIM_EVENT_END);
	nrf_spim_event_clear(nand_id_spim, NRF_SPIM_EVENT_STOPPED);
	nrf_spim_tx_buffer_set(nand_id_spim, tx, length);
	nrf_spim_rx_buffer_set(nand_id_spim, rx, length);
	if (!all_cs_high()) {
		return -EIO;
	}
	nrf_gpio_pin_clear(nand_id_cs_pins[index]);
	atomic_clear(&cs_high);
	k_busy_wait(2);
	if (k_uptime_get() >= overall_deadline) {
		(void)all_cs_high();
		return -ETIMEDOUT;
	}
	if (block_active != NULL && !block_active->clock_started) {
		int64_t now = k_uptime_get();
		int rc = block_active->sink->start(block_active->sink->user,
						 now + nand_block1024_bounds[7]);
		if (rc != 0) {
			block_active->result->end[NB_USB_RC] = (uint32_t)rc;
			block_active->marker.primary_outcome = (enum nand_marker_outcome)NAND_BLOCK1024_USB_ERROR;
			block_active->marker.primary_rc = rc;
			(void)all_cs_high();
			return rc;
		}
		/* Never renew the transport's armed deadline after callback/preemption.
		 * Elapsed metadata begins only immediately before a real NAND START. */
		int64_t actual_start = k_uptime_get();
		if (actual_start >= now + nand_block1024_bounds[7] || actual_start >= overall_deadline) {
			block_active->marker.primary_outcome = (enum nand_marker_outcome)NAND_BLOCK1024_OVERALL_TIMEOUT;
			block_active->marker.primary_rc = -ETIMEDOUT;
			(void)all_cs_high();
			return -ETIMEDOUT;
		}
		block_active->first_start_ms = actual_start;
		block_active->data_deadline_ms = now + nand_block1024_bounds[7];
		block_active->clock_started = true;
	}
	if (qualify_active != NULL) {
		int rc = qualify_before_start(overall_deadline);
		if (rc != 0) { (void)all_cs_high(); return rc; }
	}
	atomic_clear(&stopped);
	if (marker_cache || program_load) { atomic_clear(&marker_ram_scrubbed); }
	else if (cache) { atomic_clear(&page0_ram_scrubbed); }
	__DMB();
	nrf_spim_task_trigger(nand_id_spim, NRF_SPIM_TASK_START);
	*started = true;
	bool ended = long_dma ? wait_cache_end(overall_deadline) :
		wait_event(NRF_SPIM_EVENT_END, overall_deadline);
	/* Deassert even on END failure before attempting STOP. Do not inspect or
	 * reuse RX DMA storage until STOPPED proves hardware has relinquished it. */
	bool high = all_cs_high();
	if (operation == TRANSPORT_PAGE0_LOAD) {
		page0_ready_deadline = k_uptime_get() + nand_page0_bounds[6] / 1000U;
	}
	if (operation == TRANSPORT_MARKER_LOAD || operation == TRANSPORT_BLOCK1024_LOAD) {
		marker_ready_deadline = k_uptime_get() + nand_marker_bounds[7] / 1000U;
	}
	if (operation == TRANSPORT_QUALIFY_LOAD || operation == TRANSPORT_QUALIFY_ERASE ||
	    operation == TRANSPORT_QUALIFY_PROGRAM_EXECUTE) {
		qualify_active->ready_deadline_ms = k_uptime_get() +
			(operation == TRANSPORT_QUALIFY_ERASE ? 100 :
			 operation == TRANSPORT_QUALIFY_PROGRAM_EXECUTE ? 10 : 5);
	}
	nrf_spim_event_clear(nand_id_spim, NRF_SPIM_EVENT_STOPPED);
	nrf_spim_task_trigger(nand_id_spim, NRF_SPIM_TASK_STOP);
	/* STOP gets its own bounded cleanup window even after overall expiry. */
	if (!wait_event(NRF_SPIM_EVENT_STOPPED, k_uptime_get() + 3)) {
		atomic_set(&fault, 1);
		return -EBUSY;
	}
	__DMB();
	atomic_set(&stopped, 1);
	attempt->tx_amount = nrf_spim_tx_amount_get(nand_id_spim);
	attempt->rx_amount = nrf_spim_rx_amount_get(nand_id_spim);
	bool amounts = attempt->tx_amount == length && attempt->rx_amount == length;
	attempt->raw_valid = !long_dma && amounts;
	if (attempt->raw_valid) {
		memcpy(attempt->rx, rx, length);
	}
	if (!ended) {
		return -ETIMEDOUT;
	}
	if (!high || !amounts) {
		return -EIO;
	}
	return 0;
}

static int transfer(uint8_t index, enum nand_feature_operation operation,
		    struct nand_id_attempt *attempt, int64_t deadline, bool *started)
{
	*started = false;
	if ((unsigned int)operation >= ARRAY_SIZE(nand_fixed_operations)) { return -EINVAL; }
	struct nand_page0_attempt evidence = {0};
	int rc = transfer_fixed(index, (enum nand_transport_operation)operation,
				&evidence, deadline, started);
	attempt->operation = operation;
	attempt->reg = evidence.reg;
	attempt->expected_length = (uint8_t)evidence.expected_length;
	attempt->cs_index = index;
	attempt->cs_pin = evidence.cs_pin;
	attempt->raw_valid = evidence.raw_valid;
	attempt->tx_amount = evidence.tx_amount;
	attempt->rx_amount = evidence.rx_amount;
	memcpy(attempt->rx, evidence.rx, sizeof(attempt->rx));
	return rc;
}

static bool expected(const uint8_t rx[4])
{
	return rx[2] == 0x2c && rx[3] == 0x35;
}

static bool blank(const uint8_t rx[4])
{
	return (rx[0] == 0 || rx[0] == 0xff) &&
	       rx[0] == rx[1] && rx[0] == rx[2] && rx[0] == rx[3];
}

static void cleanup(void)
{
	(void)all_cs_high();
	/* Do not speculate that aux can be released or driven low. Leave the
	 * factory HIGH setting intact, including STOP failures; only read it. */
	if (!observe_aux()) {
		atomic_set(&fault, 1);
	}
	if (!atomic_get(&stopped)) {
		/* DMA buffers/pointers/ENABLE/pin routing deliberately stay intact.
		 * CS high isolates the chips; only a physical reset permits reuse. */
		atomic_set(&fault, 1);
		return;
	}
	nrf_spim_disable(nand_id_spim);
	if (nand_id_spim->ENABLE != 0) {
		atomic_set(&fault, 1);
		return;
	}
	erratum135(false);
	/* No asynchronous driver, interrupt, DPPI or shortcut can restart SPIM. */
	nrf_spim_pins_set(nand_id_spim, NRF_SPIM_PIN_NOT_CONNECTED,
			  NRF_SPIM_PIN_NOT_CONNECTED, NRF_SPIM_PIN_NOT_CONNECTED);
	nrf_spim_tx_buffer_set(nand_id_spim, NULL, 0);
	nrf_spim_rx_buffer_set(nand_id_spim, NULL, 0);
	for (size_t i = 0; i < ARRAY_SIZE(nand_id_bus_pins); ++i) {
		nrf_gpio_cfg_default(nand_id_bus_pins[i]);
	}
	bool released = (nand_id_spim->PSEL.SCK & BIT(31)) != 0 &&
			(nand_id_spim->PSEL.MOSI & BIT(31)) != 0 &&
			(nand_id_spim->PSEL.MISO & BIT(31)) != 0;
	for (size_t i = 0; i < ARRAY_SIZE(nand_id_bus_pins); ++i) {
		released = released && NRF_P1->PIN_CNF[nand_id_bus_pins[i] - 32U] == 2U;
	}
	volatile uint8_t *wipe = nand_id_rx;
	for (size_t i = 0; i < sizeof(nand_id_rx); ++i) {
		wipe[i] = 0;
	}
	atomic_set(&bus_released, released);
	if (!released || !atomic_get(&cs_high)) {
		atomic_set(&fault, 1);
	}
}

static int page0_step(struct nand_page0_result *r, enum nand_page0_operation op,
		      int64_t deadline)
{
	if ((unsigned int)op >= ARRAY_SIZE(nand_page0_transport) ||
	    r->common.attempt_count >= NAND_PAGE0_MAX_TRANSFERS) {
		r->outcome = NAND_PAGE0_TRANSFER_ERROR;
		return r->common.rc = -EINVAL;
	}
	struct nand_page0_attempt *a = &r->attempts[r->common.attempt_count];
	a->operation = op;
	bool started;
	a->rc = transfer_fixed(1U, (enum nand_transport_operation)nand_page0_transport[op],
			       a, deadline, &started);
	if (started) {
		++r->common.attempt_count;
		if (op == NAND_PAGE0_LOAD) { r->page_started = true; r->ready_unknown = true; }
		if (op == NAND_PAGE0_READY) { ++r->polls; }
	}
	if (a->rc != 0) {
		r->outcome = atomic_get(&stopped) ? NAND_PAGE0_TRANSFER_ERROR : NAND_PAGE0_STOP_ERROR;
	}
	return r->common.rc = a->rc;
}

static int page0_fail(struct nand_page0_result *r, enum nand_page0_outcome outcome, int rc)
{
	r->outcome = outcome;
	return r->common.rc = rc;
}

static int page0_run(struct nand_page0_result *r, int64_t deadline)
{
	for (size_t i = 0; i < ARRAY_SIZE(nand_page0_prefix); ++i) {
		enum nand_page0_operation op = (enum nand_page0_operation)nand_page0_prefix[i];
		if (page0_step(r, op, deadline) != 0) { return r->common.rc; }
		const uint8_t *rx = r->attempts[r->common.attempt_count - 1U].rx;
		if (i < 3U) {
			if (!expected(rx)) { return page0_fail(r, NAND_PAGE0_UNKNOWN_ID, -EPROTO); }
			if (i == 2U) { r->id_valid = true; }
		} else if (op == NAND_PAGE0_INITIAL) {
			r->initial_valid = true; r->initial = rx[2];
			if (r->initial != 0) { return page0_fail(r, NAND_PAGE0_PRECHECK_ERROR, -EPROTO); }
		} else if (op == NAND_PAGE0_CONFIG) {
			r->config_valid = true; r->config = rx[2];
			if (r->config != 0x10) { return page0_fail(r, NAND_PAGE0_PRECHECK_ERROR, -EPROTO); }
		} else if (op == NAND_PAGE0_PRE) {
			r->pre_valid = true; r->pre = rx[2];
			if (r->pre != 0) { return page0_fail(r, NAND_PAGE0_PRECHECK_ERROR, -EPROTO); }
		}
	}
	bool ready = false;
	for (uint32_t poll = 0; poll < nand_page0_bounds[5]; ++poll) {
		int64_t poll_deadline = deadline < page0_ready_deadline ? deadline : page0_ready_deadline;
		if (k_uptime_get() >= poll_deadline) {
			return page0_fail(r, NAND_PAGE0_READY_TIMEOUT, -ETIMEDOUT);
		}
		if (page0_step(r, NAND_PAGE0_READY, poll_deadline) != 0) { return r->common.rc; }
		r->ready_valid = true;
		r->ready = r->attempts[r->common.attempt_count - 1U].rx[2];
		if ((r->ready & 0x81U) == 0) { r->ready_unknown = false; }
		if ((r->ready & 0x8eU) != 0) { return page0_fail(r, NAND_PAGE0_POSTCHECK_ERROR, -EPROTO); }
		if ((r->ready & 1U) != 0) {
			if (poll + 1U < nand_page0_bounds[5]) { k_busy_wait(nand_page0_bounds[11]); }
			continue;
		}
		r->ecc_valid = true; r->ecc_code = (r->ready >> 4) & 7U;
		if (r->ecc_code == 2U) { return page0_fail(r, NAND_PAGE0_ECC_UNCORRECTABLE, -EPROTO); }
		if (r->ecc_code == 4U || r->ecc_code == 6U || r->ecc_code == 7U) {
			return page0_fail(r, NAND_PAGE0_ECC_UNSUPPORTED, -EPROTO);
		}
		ready = true;
		break;
	}
	if (!ready) { return page0_fail(r, NAND_PAGE0_READY_TIMEOUT, -ETIMEDOUT); }
	for (size_t i = 0; i < ARRAY_SIZE(nand_page0_suffix); ++i) {
		enum nand_page0_operation op = (enum nand_page0_operation)nand_page0_suffix[i];
		if (page0_step(r, op, deadline) != 0) { return r->common.rc; }
		if (op == NAND_PAGE0_CACHE_FIRST) { r->cache1_valid = true; continue; }
		if (op == NAND_PAGE0_CACHE_SECOND) { r->cache2_valid = true; continue; }
		uint8_t value = r->attempts[r->common.attempt_count - 1U].rx[2];
		if (op == NAND_PAGE0_CONFIG_AFTER) {
			r->config_after_valid = true; r->config_after = value;
			if (value != 0x10) { return page0_fail(r, NAND_PAGE0_POSTCHECK_ERROR, -EPROTO); }
			continue;
		}
		if (op == NAND_PAGE0_MIDDLE) { r->middle_valid = true; r->middle = value; }
		if (op == NAND_PAGE0_POST) { r->post_valid = true; r->post = value; }
		if (op == NAND_PAGE0_FINAL) { r->final_valid = true; r->final = value; }
		if ((value & 0x81U) != 0) { r->ready_unknown = true; }
		if (value != r->ready) { return page0_fail(r, NAND_PAGE0_POSTCHECK_ERROR, -EPROTO); }
	}
	r->outcome = NAND_PAGE0_VERIFIED;
	return r->common.rc = 0;
}

/* CRC32 IEEE is metadata, not authenticity. Always bounded to one main page. */
static uint32_t page0_crc32(const uint8_t *data)
{
	uint32_t crc = 0xffffffffU;
	for (size_t i = 0; i < NAND_PAGE0_MAIN_BYTES; ++i) {
		crc ^= data[i];
		for (unsigned int bit = 0; bit < 8; ++bit) {
			crc = (crc >> 1) ^ ((crc & 1U) ? 0xedb88320U : 0U);
		}
	}
	return crc ^ 0xffffffffU;
}

static void page0_finish(struct nand_page0_result *r)
{
	/* Called only after transport cleanup. Never touch possibly DMA-owned RX. */
	if (r->common.rc == 0 && !atomic_get(&fault) && atomic_get(&stopped) &&
	    atomic_get(&bus_released) && atomic_get(&cs_high) && atomic_get(&aux_high) &&
	    r->outcome == NAND_PAGE0_VERIFIED && r->cache1_valid && r->cache2_valid &&
	    r->config_after_valid && r->config_after == 0x10 && r->final_valid &&
	    r->final == r->ready && !r->ready_unknown) {
		r->crc1 = page0_crc32(nand_page0_rx_first + 4U);
		r->crc2 = page0_crc32(nand_page0_rx_second + 4U);
		r->match = memcmp(nand_page0_rx_first + 4U, nand_page0_rx_second + 4U,
				  NAND_PAGE0_MAIN_BYTES) == 0;
		r->crc_valid = true;
		r->data_valid = r->match && r->crc1 == r->crc2;
		if (!r->data_valid) { (void)page0_fail(r, NAND_PAGE0_CACHE_MISMATCH, -EIO); }
	}
	if (atomic_get(&stopped)) {
		volatile uint8_t *first = nand_page0_rx_first;
		volatile uint8_t *second = nand_page0_rx_second;
		for (size_t i = 0; i < NAND_PAGE0_CACHE_BYTES; ++i) { first[i] = 0; second[i] = 0; }
		atomic_set(&page0_ram_scrubbed, 1);
	}
	r->ram_scrubbed = atomic_get(&page0_ram_scrubbed) != 0;
}

static int marker_fail(struct nand_marker_result *r, enum nand_marker_outcome outcome, int rc)
{
	r->primary_outcome = outcome; r->primary_rc = rc;
	return rc;
}

static int marker_step(struct nand_marker_result *r, enum nand_marker_operation op,
		       int64_t deadline)
{
	if ((unsigned int)op >= ARRAY_SIZE(nand_marker_transport) ||
	    r->common.attempt_count >= NAND_MARKER_MAX_TRANSFERS) { return -EINVAL; }
	struct nand_marker_attempt *a = &r->attempts[r->common.attempt_count];
	struct nand_page0_attempt e = {0};
	bool started;
	a->operation = op;
	enum nand_transport_operation transport = (enum nand_transport_operation)nand_marker_transport[op];
	if (block_active != NULL && transport == TRANSPORT_MARKER_LOAD) { transport = TRANSPORT_BLOCK1024_LOAD; }
	a->rc = transfer_fixed(1U, transport,
			       &e, deadline, &started);
	a->cs_pin = e.cs_pin; a->reg = e.reg; a->expected_length = e.expected_length;
	a->tx_amount = e.tx_amount; a->rx_amount = e.rx_amount; a->raw_valid = e.raw_valid;
	memcpy(a->rx, e.rx, sizeof(a->rx)); /* Control metadata only, never private cache. */
	if (started) {
		++r->common.attempt_count;
		if (block_active != NULL) {
			++block_active->result->end[NB_TRANSFERS];
			if (op == NAND_MARKER_READY1) { ++block_active->result->end[NB_POLLS1]; }
			if (op == NAND_MARKER_READY2) { ++block_active->result->end[NB_POLLS2]; }
		}
		/* START, not successful completion, establishes these obligations.
		 * No restoration/cleanup decision occurs until this transfer returns. */
		if (op == NAND_MARKER_OFF) { r->off_attempted = true; }
		if (op == NAND_MARKER_RESTORE_GUARD) { r->restore_attempted = true; }
		if (op == NAND_MARKER_RESTORE_WRITE) { r->restore_write_attempted = true; }
		if (op == NAND_MARKER_OFF || op == NAND_MARKER_RESTORE_WRITE) {
			r->current_config_valid = false; r->current_config = 0;
		}
		if (op == NAND_MARKER_LOAD1 || op == NAND_MARKER_LOAD2) {
			r->ready_unknown = true;
			if (op == NAND_MARKER_LOAD1) { r->page1_started = true; }
			else { r->page2_started = true; }
		}
		if (op == NAND_MARKER_READY1) { ++r->polls1; }
		if (op == NAND_MARKER_READY2) { ++r->polls2; }
	}
	if (a->rc == 0 && (op == NAND_MARKER_CONFIG || op == NAND_MARKER_RAW_CONFIG ||
			  op == NAND_MARKER_RAW_FINAL || op == NAND_MARKER_RESTORE_CONFIG)) {
		r->current_config_valid = true; r->current_config = a->rx[2];
	}
	return a->rc;
}

static int marker_data_step(struct nand_marker_result *r, enum nand_marker_operation op,
			    int64_t deadline)
{
	int rc = marker_step(r, op, deadline);
	if (rc != 0) {
		if (block_active != NULL && r->primary_outcome >= (enum nand_marker_outcome)NAND_BLOCK1024_USB_ERROR &&
		    r->primary_outcome <= (enum nand_marker_outcome)NAND_BLOCK1024_OVERALL_TIMEOUT) {
			return r->primary_rc;
		}
		return marker_fail(r, atomic_get(&stopped) ? NAND_MARKER_TRANSFER_ERROR :
				   NAND_MARKER_STOP_ERROR, rc);
	}
	return 0;
}

static uint8_t marker_value(const struct nand_marker_result *r)
{
	return r->attempts[r->common.attempt_count - 1U].rx[2];
}

static bool marker_idle(struct nand_marker_result *r, uint8_t value)
{
	if ((value & 0x81U) != 0) { r->ready_unknown = true; }
	/* Raw ECC bits6:4 are undefined/stale, never corrected-data evidence. */
	return (value & 0x8fU) == 0;
}

static int marker_ready(struct nand_marker_result *r, bool second, int64_t deadline)
{
	for (uint32_t i = 0; i < nand_marker_bounds[6]; ++i) {
		int64_t until = deadline < marker_ready_deadline ? deadline : marker_ready_deadline;
		if (k_uptime_get() >= until) { return marker_fail(r, NAND_MARKER_READY_TIMEOUT, -ETIMEDOUT); }
		if (marker_data_step(r, second ? NAND_MARKER_READY2 : NAND_MARKER_READY1, until) != 0) {
			return r->primary_rc;
		}
		uint8_t value = marker_value(r);
		if (second) { r->ready2_valid = true; r->ready2 = value; }
		else { r->ready1_valid = true; r->ready1 = value; }
		if ((value & 0x81U) == 0) { r->ready_unknown = false; }
		if ((value & 0x8eU) != 0) { return marker_fail(r, NAND_MARKER_POSTCHECK_ERROR, -EPROTO); }
		if ((value & 1U) == 0) { return 0; }
		if (i + 1U < nand_marker_bounds[6]) { k_busy_wait(nand_marker_bounds[13]); }
	}
	return marker_fail(r, NAND_MARKER_READY_TIMEOUT, -ETIMEDOUT);
}

static int marker_prefix_run(struct nand_marker_result *r, int64_t deadline)
{
	for (size_t i = 0; i < ARRAY_SIZE(nand_marker_prefix); ++i) {
		enum nand_marker_operation op = (enum nand_marker_operation)nand_marker_prefix[i];
		if (marker_data_step(r, op, deadline) != 0) { return r->primary_rc; }
		uint8_t value = marker_value(r);
		if (op == NAND_MARKER_ID) {
			if (!expected(r->attempts[r->common.attempt_count - 1U].rx)) {
				return marker_fail(r, NAND_MARKER_UNKNOWN_ID, -EPROTO);
			}
			if (i == 2U) { r->id_valid = true; }
		} else if (op == NAND_MARKER_INITIAL) {
			r->initial_valid = true; r->initial = value;
			if (value != 0) { return marker_fail(r, NAND_MARKER_PRECHECK_ERROR, -EPROTO); }
		} else if (op == NAND_MARKER_LOCK) {
			r->lock_valid = true; r->lock = value;
			if (block_active != NULL && value != 0x7c) {
				return marker_fail(r, NAND_MARKER_PRECHECK_ERROR, -EPROTO);
			}
		} else if (op == NAND_MARKER_CONFIG) {
			r->config_valid = true; r->config = value;
			if (value != 0x10) { return marker_fail(r, NAND_MARKER_PRECHECK_ERROR, -EPROTO); }
		} else if (op == NAND_MARKER_PRE) {
			r->pre_valid = true; r->pre = value;
			if (value != 0) { return marker_fail(r, NAND_MARKER_PRECHECK_ERROR, -EPROTO); }
		} else if (op == NAND_MARKER_RAW_CONFIG) {
			r->raw_config_valid = true; r->raw_config = value;
			if (value != 0) { return marker_fail(r, NAND_MARKER_CONFIG_ERROR, -EPROTO); }
			r->off_confirmed = true;
		} else if (op == NAND_MARKER_RAW_PRE) {
			r->raw_pre_valid = true; r->raw_pre = value;
			if (!marker_idle(r, value)) { return marker_fail(r, NAND_MARKER_POSTCHECK_ERROR, -EPROTO); }
		}
	}
	return 0;
}

static int marker_row_run(struct nand_marker_result *r, int64_t deadline)
{
	for (unsigned int pass = 0; pass < 2U; ++pass) {
		for (size_t i = 0; i < (pass == 0 ? ARRAY_SIZE(nand_marker_first) : ARRAY_SIZE(nand_marker_second)); ++i) {
			enum nand_marker_operation op = (enum nand_marker_operation)
				(pass == 0 ? nand_marker_first[i] : nand_marker_second[i]);
			if (marker_data_step(r, op, deadline) != 0) { return r->primary_rc; }
			if (op == NAND_MARKER_LOAD1 || op == NAND_MARKER_LOAD2) {
				if (marker_ready(r, pass != 0, deadline) != 0) { return r->primary_rc; }
			} else if (op == NAND_MARKER_CACHE1) { r->cache1_valid = true; }
			else if (op == NAND_MARKER_CACHE2) { r->cache2_valid = true; }
			else {
				uint8_t value = marker_value(r);
				if (op == NAND_MARKER_RAW_FINAL) {
					r->raw_final_valid = true; r->raw_final = value;
					if (value != 0) { return marker_fail(r, NAND_MARKER_CONFIG_ERROR, -EPROTO); }
				} else {
					if (op == NAND_MARKER_POST1) { r->post1_valid = true; r->post1 = value; }
					else { r->post2_valid = true; r->post2 = value; }
					if (!marker_idle(r, value)) { return marker_fail(r, NAND_MARKER_POSTCHECK_ERROR, -EPROTO); }
				}
			}
		}
	}
	return marker_fail(r, NAND_MARKER_VERIFIED, 0);
}

static int marker_run(struct nand_marker_result *r, int64_t deadline)
{
	if (marker_prefix_run(r, deadline) != 0) { return r->primary_rc; }
	return marker_row_run(r, deadline);
}

static void marker_restore_config(struct nand_marker_result *r)
{
	if (!r->off_attempted) { return; }
	if (!atomic_get(&stopped) || r->ready_unknown || atomic_get(&fault) ||
	    !atomic_get(&cs_high) || !observe_aux()) {
		r->restore_state = NAND_MARKER_RESTORE_SKIPPED_UNSAFE;
		r->restore_rc = -EBUSY;
		return;
	}
	/* Independent cleanup allowance: one fixed attempt, no renewal/retry. */
	int64_t deadline = k_uptime_get() + nand_marker_bounds[12];
	r->restore_state = NAND_MARKER_RESTORE_FAILED;
	for (size_t i = 0; i < ARRAY_SIZE(nand_marker_restore); ++i) {
		enum nand_marker_operation op = (enum nand_marker_operation)nand_marker_restore[i];
		r->restore_rc = marker_step(r, op, deadline);
		if (r->restore_rc != 0) { return; }
		uint8_t value = marker_value(r);
		if (op == NAND_MARKER_RESTORE_GUARD) {
			r->restore_guard_valid = true; r->restore_guard = value;
			if (!marker_idle(r, value)) { r->restore_rc = -EPROTO; return; }
		} else if (op == NAND_MARKER_RESTORE_CONFIG) {
			r->restore_config_valid = true; r->restore_config = value;
			if (value != 0x10) { r->restore_rc = -EPROTO; return; }
		} else if (op == NAND_MARKER_LOCK_AFTER) {
			r->lock_after_valid = true; r->lock_after = value;
			if (!r->lock_valid || value != r->lock) { r->restore_rc = -EPROTO; return; }
		} else if (op == NAND_MARKER_FINAL) {
			r->final_valid = true; r->final = value;
			if (!marker_idle(r, value)) { r->restore_rc = -EPROTO; return; }
		}
	}
	r->restored = true; r->restore_state = NAND_MARKER_RESTORE_VERIFIED;
}

static uint32_t marker_crc32(const uint8_t *data)
{
	uint32_t crc = 0xffffffffU;
	for (size_t i = 0; i < NAND_MARKER_RAW_BYTES; ++i) {
		crc ^= data[i];
		for (unsigned int b = 0; b < 8U; ++b) { crc = (crc >> 1) ^ ((crc & 1U) ? 0xedb88320U : 0U); }
	}
	return crc ^ 0xffffffffU;
}

static void block1024_wipe(void)
{
	if (!atomic_get(&stopped)) { return; }
	volatile uint8_t *a = nand_marker_rx_first, *b = nand_marker_rx_second;
	for (size_t i = 0; i < NAND_MARKER_CACHE_BYTES; ++i) { a[i] = 0; b[i] = 0; }
	atomic_set(&marker_ram_scrubbed, 1);
}

static bool block1024_data_safe(const struct nand_marker_result *r)
{
	return atomic_get(&stopped) && !atomic_get(&fault) && atomic_get(&cs_high) &&
		observe_aux() && !r->ready_unknown && r->off_confirmed &&
		r->current_config_valid && r->current_config == 0;
}

static int block1024_ack(struct nand_block1024_context *b, struct nand_block1024_ack a,
			bool page)
{
	uint32_t *end = b->result->end;
	if (page && a.frame_sent) { ++end[NB_ROWS_SENT]; }
	if (a.status == NAND_BLOCK1024_SINK_OK && a.rc == 0 && a.frame_sent) {
		if (page) { ++end[NB_ROWS_ACKED]; end[NB_RAW_BYTES_ACKED] += NAND_MARKER_RAW_BYTES; }
		return 0;
	}
	if (a.status != NAND_BLOCK1024_USB_ERROR && a.status != NAND_BLOCK1024_HOST_ABORT &&
	    a.status != NAND_BLOCK1024_OVERALL_TIMEOUT) { a.status = NAND_BLOCK1024_USB_ERROR; }
	if (a.rc == 0) { a.rc = -EPROTO; }
	end[NB_USB_RC] = (uint32_t)a.rc;
	return marker_fail(&b->marker, (enum nand_marker_outcome)a.status, a.rc);
}

static int block1024_timeout(struct nand_block1024_context *b)
{
	return marker_fail(&b->marker, (enum nand_marker_outcome)NAND_BLOCK1024_OVERALL_TIMEOUT, -ETIMEDOUT);
}

static void block1024_new_row(struct nand_marker_result *r)
{
	/* Configuration obligations remain live; only bounded per-row evidence goes. */
	r->common.attempt_count = 0;
	memset(r->attempts, 0, sizeof(r->attempts));
	r->polls1 = r->polls2 = 0;
	r->page1_started = r->ready1_valid = r->cache1_valid = r->post1_valid = false;
	r->page2_started = r->ready2_valid = r->cache2_valid = r->post2_valid = false;
	r->ready1 = r->ready2 = r->post1 = r->post2 = 0;
	r->raw_final_valid = false; r->raw_final = 0;
	r->primary_rc = 0; r->primary_outcome = NAND_MARKER_NOT_RUN;
}

static int block1024_run(struct nand_block1024_context *b)
{
	struct nand_marker_result *r = &b->marker;
	/* The first actual START replaces this provisional pre-START allowance. */
	if (marker_prefix_run(r, k_uptime_get() + nand_block1024_bounds[7]) != 0) {
		if (b->clock_started && k_uptime_get() >= b->data_deadline_ms &&
		    r->primary_outcome == NAND_MARKER_TRANSFER_ERROR) { return block1024_timeout(b); }
		return r->primary_rc;
	}
	if (!block1024_data_safe(r)) { return marker_fail(r, NAND_MARKER_POSTCHECK_ERROR, -EIO); }
	if (k_uptime_get() >= b->data_deadline_ms) { return block1024_timeout(b); }
	uint32_t begin_fields[16] = {1024,65536,64,4352,278528,15,0x2c35,3,
		r->initial,r->lock,r->config,r->raw_config,r->raw_pre,10,0,0};
	if (block1024_ack(b, b->sink->begin(b->sink->user, begin_fields, b->data_deadline_ms), false) != 0) {
		return r->primary_rc;
	}
	b->crc_state = 0xffffffffU;
	for (uint32_t row = 0; row < NAND_BLOCK1024_ROWS; ++row) {
		if (k_uptime_get() >= b->data_deadline_ms) { return block1024_timeout(b); }
		if (!block1024_data_safe(r)) { return marker_fail(r, NAND_MARKER_POSTCHECK_ERROR, -EIO); }
		b->row_index = (uint8_t)row;
		block1024_new_row(r);
		int64_t row_deadline = k_uptime_get() + nand_block1024_bounds[8];
		if (row_deadline > b->data_deadline_ms) { row_deadline = b->data_deadline_ms; }
		if (marker_row_run(r, row_deadline) != 0) {
			if (k_uptime_get() >= b->data_deadline_ms &&
			    (r->primary_outcome == NAND_MARKER_TRANSFER_ERROR ||
			     r->primary_outcome == NAND_MARKER_READY_TIMEOUT)) { return block1024_timeout(b); }
			return r->primary_rc;
		}
		if (!block1024_data_safe(r) || !r->cache1_valid || !r->cache2_valid ||
		    !r->post1_valid || !r->post2_valid || !r->raw_final_valid || r->raw_final != 0) {
			return marker_fail(r, NAND_MARKER_POSTCHECK_ERROR, -EIO);
		}
		/* STOP proven, neither buffer can be restarted while this synchronous
		 * owner compares/exports. Raw data is deliberately not ECC-corrected. */
		if (memcmp(nand_marker_rx_first + 4U, nand_marker_rx_second + 4U, NAND_MARKER_RAW_BYTES) != 0) {
			return marker_fail(r, NAND_MARKER_COPY_MISMATCH, -EIO);
		}
		uint32_t crc1 = marker_crc32(nand_marker_rx_first + 4U);
		uint32_t crc2 = marker_crc32(nand_marker_rx_second + 4U);
		if (crc1 != crc2) { return marker_fail(r, NAND_MARKER_COPY_MISMATCH, -EIO); }
		++b->result->end[NB_ROWS_COMPARED];
		if (k_uptime_get() >= b->data_deadline_ms) { return block1024_timeout(b); }
		uint32_t fields[16] = {crc1,crc2,1,7,r->polls1,r->polls2,r->ready1,r->ready2,
			r->post1,r->post2,r->raw_final,r->common.attempt_count,4356,4356,4356,4356};
		if (block1024_ack(b, b->sink->page(b->sink->user, (uint16_t)(row + 1U), fields,
						 nand_marker_rx_first + 4U, b->data_deadline_ms), true) != 0) {
			block1024_wipe();
			return r->primary_rc;
		}
		/* Incremental CRC covers only acknowledged rows and is never exposed on
		 * partial/restoration/cleanup failure. No concatenated payload buffer. */
		for (size_t i = 4U; i < NAND_MARKER_CACHE_BYTES; ++i) {
			b->crc_state ^= nand_marker_rx_first[i];
			for (unsigned int bit = 0; bit < 8U; ++bit) {
				b->crc_state = (b->crc_state >> 1) ^ ((b->crc_state & 1U) ? 0xedb88320U : 0U);
			}
		}
		block1024_wipe();
		if (k_uptime_get() >= b->data_deadline_ms) { return block1024_timeout(b); }
	}
	return marker_fail(r, NAND_MARKER_VERIFIED, 0);
}

static void block1024_result(struct nand_block1024_context *b)
{
	struct nand_marker_result *r = &b->marker;
	uint32_t *e = b->result->end;
	nand_id_get_state(&b->result->state);
	const struct nand_id_state *s = &b->result->state;
	e[NB_RC] = (uint32_t)r->common.rc; e[NB_OUTCOME] = r->outcome;
	e[NB_PRIMARY_RC] = (uint32_t)r->primary_rc; e[NB_PRIMARY_OUTCOME] = r->primary_outcome;
	e[NB_RESTORE_RC] = (uint32_t)r->restore_rc; e[NB_RESTORE_STATE] = r->restore_state;
	e[NB_OFF_ATTEMPTED] = r->off_attempted; e[NB_OFF_CONFIRMED] = r->off_confirmed;
	e[NB_RESTORE_ATTEMPTED] = r->restore_attempted; e[NB_RESTORE_WRITE_ATTEMPTED] = r->restore_write_attempted;
	e[NB_RESTORE_CONFIG_VALID] = r->restore_config_valid; e[NB_RESTORE_CONFIG] = r->restore_config;
	e[NB_LOCK_BEFORE_VALID] = r->lock_valid; e[NB_LOCK_BEFORE] = r->lock;
	e[NB_LOCK_AFTER_VALID] = r->lock_after_valid; e[NB_LOCK_AFTER] = r->lock_after;
	e[NB_FINAL_VALID] = r->final_valid; e[NB_FINAL] = r->final;
	e[NB_RESTORED] = r->restored; e[NB_RESTORE_REQUIRED] = r->restore_required;
	e[NB_CURRENT_CONFIG_VALID] = r->current_config_valid; e[NB_CURRENT_CONFIG] = r->current_config;
	e[NB_CONFIG_UNKNOWN] = r->config_unknown; e[NB_RAM_SCRUBBED] = r->ram_scrubbed;
	e[NB_READY_UNKNOWN] = r->ready_unknown; e[NB_FAULT] = s->fault_latched;
	e[NB_STOPPED] = s->clock_stopped; e[NB_BUS_RELEASED] = s->bus_released;
	e[NB_CS_CONFIGURED] = s->cs_configured; e[NB_CS_HIGH] = s->cs_high;
	e[NB_AUX_CONFIGURED] = s->aux_configured; e[NB_AUX_HIGH] = s->aux_high;
	e[NB_ELAPSED_MS] = b->clock_started ? (uint32_t)(k_uptime_get() - b->first_start_ms) : 0;
	e[NB_TRANSPORT_QUARANTINED] = 1; /* Adapter alone may clear after clean END ACK. */
	if (r->common.rc == 0 && r->outcome == NAND_MARKER_VERIFIED && r->restored &&
	    e[NB_ROWS_ACKED] == NAND_BLOCK1024_ROWS && !s->fault_latched && s->clock_stopped &&
	    s->bus_released && r->ram_scrubbed && !r->ready_unknown) {
		e[NB_BLOCK_CRC_VALID] = 1; e[NB_BLOCK_CRC32] = b->crc_state ^ 0xffffffffU;
	}
}

static void marker_finish(struct nand_marker_result *r)
{
	if (r->common.rc == 0 && r->primary_outcome == NAND_MARKER_VERIFIED && r->restored &&
	    r->cache1_valid && r->cache2_valid && !r->ready_unknown && !atomic_get(&fault) &&
	    atomic_get(&stopped) && atomic_get(&bus_released) && atomic_get(&cs_high) && atomic_get(&aux_high)) {
		r->crc1 = marker_crc32(nand_marker_rx_first + 4U);
		r->crc2 = marker_crc32(nand_marker_rx_second + 4U);
		r->match = memcmp(nand_marker_rx_first + 4U, nand_marker_rx_second + 4U, NAND_MARKER_RAW_BYTES) == 0;
		r->crc_valid = true;
		r->marker1 = ((uint16_t)nand_marker_rx_first[4100] << 8) | nand_marker_rx_first[4101];
		r->marker2 = ((uint16_t)nand_marker_rx_second[4100] << 8) | nand_marker_rx_second[4101];
		r->markers_valid = true;
		r->data_valid = r->match && r->crc1 == r->crc2;
		r->marker_ff = r->data_valid && r->marker1 == 0xffffU && r->marker2 == 0xffffU;
		if (!r->data_valid) { r->outcome = NAND_MARKER_COPY_MISMATCH; r->common.rc = -EIO; }
	}
	if (atomic_get(&stopped)) {
		volatile uint8_t *a = nand_marker_rx_first, *b = nand_marker_rx_second;
		for (size_t i = 0; i < NAND_MARKER_CACHE_BYTES; ++i) { a[i] = 0; b[i] = 0; }
		atomic_set(&marker_ram_scrubbed, 1);
	}
	r->ram_scrubbed = atomic_get(&marker_ram_scrubbed) != 0;
}

/* One shared admission/setup/cleanup owner; mode is not exposed as a shell or
 * general transfer API. A feature result selects its single fixed program. */
#include "nand_qualify.inc"
#ifdef OPENPENDANT_PUBLIC_OBJECT
#include "nand_public_object_adapter.inc"
#endif
#ifdef OPENPENDANT_WRITE_RATE
#include "nand_write_rate_adapter.inc"
#endif

static int probe(struct nand_id_result *result, struct nand_feature_result *features,
		 struct nand_page0_result *page, struct nand_marker_result *marker,
		 struct nand_block1024_context *block, struct nand_qualify_context *qualification,
		 bool public_object)
{
	if (result == NULL) {
		return -EINVAL;
	}
	memset(result, 0, sizeof(*result));
	result->selected_index = NAND_ID_NO_SELECTION;
	result->selected_pin = NAND_ID_NO_SELECTION;
	if (!atomic_cas(&busy, 0, 1)) {
		result->rc = -EBUSY;
		result_state(result);
		return result->rc;
	}
	block_active = block;
	qualify_active = qualification;
	int64_t begin = k_uptime_get();
	int64_t deadline = begin + (public_object ? NPO_DATA_MS : qualification != NULL ? NAND_QUALIFY_DATA_MS : marker != NULL ? nand_marker_bounds[11] :
		page != NULL ? nand_page0_bounds[10] : NAND_ID_TOTAL_MS);
	bool public_recovery = false;
#ifdef OPENPENDANT_PUBLIC_OBJECT
	public_recovery = public_object && npo_active_mode == NPO_RECOVER_ONLY && npo_previous_clean;
#endif
	if ((atomic_get(&attempted) && !public_recovery) || atomic_get(&fault)) {
		result->rc = -EALREADY;
		goto done;
	}
	bool pins_available = pins_idle();
#ifdef OPENPENDANT_PUBLIC_OBJECT
	if (public_recovery) { pins_available = npo_reentry_pins(); }
#endif
	if (!peripheral_idle() || !pins_available) {
		result->rc = -EPERM;
		result->outcome = NAND_ID_PRECONDITION_ERROR;
		/* Unexpected ownership is not ours to repair. No hardware writes,
		 * no optimistic idle claims, and no second attempt without reset. */
		atomic_set(&fault, 1);
		atomic_clear(&stopped);
		atomic_clear(&bus_released);
		goto done;
	}
	atomic_set(&attempted, 1);
	/* Set EVERY CS output latch high BEFORE changing ANY CS direction and
	 * before auxiliary configuration or routing/enabling the bus. This avoids
	 * selecting a chip if the auxiliary line happens to enable a supply. */
	if (!all_cs_high()) {
		result->rc = -EIO;
		result->outcome = NAND_ID_PRECONDITION_ERROR;
		atomic_set(&fault, 1);
		goto done;
	}
	for (size_t i = 0; i < ARRAY_SIZE(nand_id_cs_pins); ++i) {
		nrf_gpio_cfg_output(nand_id_cs_pins[i]);
	}
	atomic_set(&cs_configured, 1);
	for (size_t i = 0; i < ARRAY_SIZE(nand_id_cs_pins); ++i) {
		if (nrf_gpio_pin_dir_get(nand_id_cs_pins[i]) != NRF_GPIO_PIN_DIR_OUTPUT) {
			result->rc = -EIO;
			result->outcome = NAND_ID_PRECONDITION_ERROR;
			atomic_set(&fault, 1);
			goto finish;
		}
	}
	/* Preload HIGH before enabling the output. No LOW write, pull change,
	 * or cleanup release is allowed on this factory-confirmed auxiliary pin. */
	nrf_gpio_pin_set(nand_id_aux_pin);
	if (nrf_gpio_pin_out_read(nand_id_aux_pin) != 1U) {
		result->rc = -EIO;
		result->outcome = NAND_ID_PRECONDITION_ERROR;
		atomic_set(&fault, 1);
		goto finish;
	}
	nrf_gpio_cfg_output(nand_id_aux_pin);
	if (!observe_aux()) {
		result->rc = -EIO;
		result->outcome = NAND_ID_PRECONDITION_ERROR;
		atomic_set(&fault, 1);
		goto finish;
	}
	k_busy_wait(NAND_ID_AUX_SETTLE_US);
	if (k_uptime_get() >= deadline) {
		result->rc = -ETIMEDOUT;
		result->outcome = NAND_ID_TRANSFER_ERROR;
		goto finish;
	}
	nrf_gpio_pin_clear(NAND_ID_SCK);
	nrf_gpio_pin_clear(NAND_ID_MOSI);
	nrf_gpio_cfg_output(NAND_ID_SCK);
	nrf_gpio_cfg_output(NAND_ID_MOSI);
	nrf_gpio_cfg_input(NAND_ID_MISO, NRF_GPIO_PIN_NOPULL);
	atomic_clear(&bus_released);
	nrf_spim_frequency_set(nand_id_spim, NRF_SPIM_FREQ_125K);
	nrf_spim_configure(nand_id_spim, NRF_SPIM_MODE_0, NRF_SPIM_BIT_ORDER_MSB_FIRST);
	nrf_spim_orc_set(nand_id_spim, 0);
	nrf_spim_iftiming_set(nand_id_spim, 0);
	nrf_spim_pins_set(nand_id_spim, NAND_ID_SCK, NAND_ID_MOSI, NAND_ID_MISO);
	if (nand_id_spim->FREQUENCY != (uint32_t)NRF_SPIM_FREQ_125K ||
	    nand_id_spim->CONFIG != 0 ||
	    nand_id_spim->PSEL.SCK != NAND_ID_SCK ||
	    nand_id_spim->PSEL.MOSI != NAND_ID_MOSI ||
	    nand_id_spim->PSEL.MISO != NAND_ID_MISO ||
	    NRF_P1->PIN_CNF[8] != 3U || NRF_P1->PIN_CNF[9] != 3U ||
	    NRF_P1->PIN_CNF[10] != 0U) {
		result->rc = -EIO;
		result->outcome = NAND_ID_PRECONDITION_ERROR;
		atomic_set(&fault, 1);
		goto finish;
	}
	erratum135(true);
	nrf_spim_enable(nand_id_spim);
	if (!nrf_spim_enable_check(nand_id_spim)) {
		result->rc = -EIO;
		result->outcome = NAND_ID_PRECONDITION_ERROR;
		atomic_set(&fault, 1);
		goto finish;
	}
#ifdef OPENPENDANT_PUBLIC_OBJECT
	if (public_object) { result->rc = npo_adapter_run(); goto finish; }
#endif
#ifdef OPENPENDANT_WRITE_RATE
	if (nwr_active_result != NULL) { result->rc = nwr_adapter_run(); goto finish; }
#endif
	if (qualification != NULL) { (void)qualify_run(qualification); goto finish; }
	if (block != NULL) { (void)block1024_run(block); goto finish; }
	if (marker != NULL) { (void)marker_run(marker, deadline); goto finish; }
	if (page != NULL) { (void)page0_run(page, deadline); goto finish; }
	if (features != NULL) {
		for (uint8_t i = 0; i < ARRAY_SIZE(nand_feature_program); ++i) {
			struct nand_id_attempt *a = &result->attempts[result->attempt_count];
			bool started;
			a->rc = transfer(1U, nand_feature_program[i], a,
					 begin + NAND_ID_TOTAL_MS, &started);
			if (started) {
				++result->attempt_count;
			}
			if (a->rc != 0) {
				result->rc = a->rc;
				features->outcome = atomic_get(&stopped) ?
					NAND_FEATURE_TRANSFER_ERROR : NAND_FEATURE_STOP_ERROR;
				goto finish;
			}
			if (i < 3U) {
				if (!expected(a->rx)) {
					result->rc = -EPROTO;
					features->outcome = NAND_FEATURE_UNKNOWN_ID;
					goto finish;
				}
				if (i == 2U) { features->id_valid = true; }
			} else if (i == 3U) {
				features->initial = a->rx[2];
				features->initial_valid = true;
				features->initial_busy = (a->rx[2] & 1U) != 0;
				if (features->initial_busy) {
					result->rc = -EBUSY;
					features->outcome = NAND_FEATURE_INITIAL_BUSY;
					goto finish;
				}
			} else if (i == 4U) {
				features->config = a->rx[2];
				features->config_valid = true;
			} else {
				features->final = a->rx[2];
				features->final_valid = true;
				features->final_busy = (a->rx[2] & 1U) != 0;
				features->changed = features->final != features->initial;
				features->outcome = features->changed || features->final_busy ?
					NAND_FEATURE_ANOMALOUS_FINAL : NAND_FEATURE_SNAPSHOT;
				result->rc = features->outcome == NAND_FEATURE_SNAPSHOT ? 0 : -EAGAIN;
			}
		}
		goto finish;
	}
	result->rc = -ENODATA;
	result->outcome = NAND_ID_INCONCLUSIVE;
	for (uint8_t index = 0; index < 4; ++index) {
		for (uint8_t confirmation = 0; confirmation < 3; ++confirmation) {
			struct nand_id_attempt *a = &result->attempts[result->attempt_count];
			bool started;
			a->rc = transfer(index, NAND_FEATURE_OP_ID, a,
					 begin + NAND_ID_TOTAL_MS, &started);
			if (started) {
				++result->attempt_count;
			}
			if (a->rc != 0) {
				result->rc = a->rc;
				result->outcome = atomic_get(&stopped) ?
					NAND_ID_TRANSFER_ERROR : NAND_ID_STOP_ERROR;
				goto finish;
			}
			if (expected(a->rx)) {
				if (confirmation == 2) {
					result->rc = 0;
					result->outcome = NAND_ID_CONFIRMED;
					result->selected_index = index;
					result->selected_pin = a->cs_pin;
					goto finish;
				}
				continue;
			}
			if (confirmation == 0 && blank(a->rx)) {
				break;
			}
			result->rc = -EPROTO;
			result->outcome = NAND_ID_UNEXPECTED;
			goto finish;
		}
	}
finish:
	if (qualification != NULL) {
		if (result->rc != 0 && qualification->result->words[NQ_PRIMARY_OUTCOME] == NQ_NOT_RUN) {
			(void)qualify_fail(result->outcome == NAND_ID_PRECONDITION_ERROR ? NQ_REFUSED : NQ_TRANSFER_ERROR, result->rc);
		}
		qualify_restore(qualification);
	}
	if (marker != NULL) {
		if (marker->primary_outcome == NAND_MARKER_NOT_RUN) {
			marker->primary_outcome = result->outcome == NAND_ID_PRECONDITION_ERROR ?
				NAND_MARKER_PRECONDITION_ERROR : NAND_MARKER_TRANSFER_ERROR;
			marker->primary_rc = result->rc;
		}
		marker_restore_config(marker); /* Before transport routing is released. */
		marker->restore_required = marker->off_attempted && !marker->restored;
		marker->config_unknown = marker->off_attempted && !marker->current_config_valid;
		if (marker->restore_required || marker->ready_unknown) { atomic_set(&fault, 1); }
		marker->outcome = !atomic_get(&stopped) ? NAND_MARKER_STOP_ERROR :
			marker->restore_required ? NAND_MARKER_RESTORE_ERROR : marker->primary_outcome;
		result->rc = marker->primary_rc != 0 ? marker->primary_rc : marker->restore_rc;
	}
	if (page != NULL) {
		if (page->outcome == NAND_PAGE0_NOT_RUN) {
			page->outcome = result->outcome == NAND_ID_PRECONDITION_ERROR ?
				NAND_PAGE0_PRECONDITION_ERROR : NAND_PAGE0_TRANSFER_ERROR;
		}
		if (page->ready_unknown) { atomic_set(&fault, 1); }
	}
	if (features != NULL && features->outcome == NAND_FEATURE_NOT_RUN) {
		features->outcome = result->outcome == NAND_ID_PRECONDITION_ERROR ?
			NAND_FEATURE_PRECONDITION_ERROR : NAND_FEATURE_TRANSFER_ERROR;
	}
#ifdef OPENPENDANT_PUBLIC_OBJECT
	nrr_cleanup_check();
#endif
#ifdef OPENPENDANT_WRITE_RATE
	nwr_cleanup_check();
#endif
	cleanup();
	if (marker != NULL && atomic_get(&fault) && marker->outcome != NAND_MARKER_STOP_ERROR &&
	    marker->outcome != NAND_MARKER_RESTORE_ERROR && marker->outcome != NAND_MARKER_PRECONDITION_ERROR) {
		marker->outcome = NAND_MARKER_CLEANUP_ERROR;
		if (result->rc == 0) { result->rc = -EIO; }
	}
	if (page != NULL && atomic_get(&fault) &&
	    page->outcome != NAND_PAGE0_STOP_ERROR && page->outcome != NAND_PAGE0_PRECONDITION_ERROR &&
	    !page->ready_unknown) {
		page->outcome = NAND_PAGE0_CLEANUP_ERROR;
		if (result->rc == 0) { result->rc = -EIO; }
	}
	if (features != NULL && atomic_get(&fault)) {
		if (features->outcome != NAND_FEATURE_STOP_ERROR &&
		    features->outcome != NAND_FEATURE_PRECONDITION_ERROR) {
			features->outcome = NAND_FEATURE_CLEANUP_ERROR;
		}
		if (result->rc == 0) { result->rc = -EIO; }
	}
	if (atomic_get(&fault) && result->rc == 0) {
		result->rc = -EIO;
		result->outcome = NAND_ID_TRANSFER_ERROR;
		result->selected_index = NAND_ID_NO_SELECTION;
		result->selected_pin = NAND_ID_NO_SELECTION;
	}
	if (block != NULL) { block1024_wipe(); }
	else if (marker != NULL) { marker_finish(marker); }
	if (page != NULL) { page0_finish(page); }
	if (qualification != NULL) { qualify_finish(qualification); }
done:
	if (marker != NULL) {
		if (marker->primary_outcome == NAND_MARKER_NOT_RUN && result->outcome == NAND_ID_PRECONDITION_ERROR) {
			marker->primary_outcome = marker->outcome = NAND_MARKER_PRECONDITION_ERROR;
			marker->primary_rc = result->rc;
		}
		marker->ram_scrubbed = atomic_get(&marker_ram_scrubbed) != 0;
	}
	if (page != NULL) {
		if (page->outcome == NAND_PAGE0_NOT_RUN && result->outcome == NAND_ID_PRECONDITION_ERROR) {
			page->outcome = NAND_PAGE0_PRECONDITION_ERROR;
		}
		page->ram_scrubbed = atomic_get(&page0_ram_scrubbed) != 0;
	}
	if (features != NULL && features->outcome == NAND_FEATURE_NOT_RUN &&
	    result->outcome == NAND_ID_PRECONDITION_ERROR) {
		features->outcome = NAND_FEATURE_PRECONDITION_ERROR;
	}
	if (features != NULL && features->outcome == NAND_FEATURE_NOT_RUN &&
	    result->outcome == NAND_ID_TRANSFER_ERROR) {
		features->outcome = NAND_FEATURE_TRANSFER_ERROR;
	}
	result->elapsed_ms = (uint32_t)(k_uptime_get() - begin);
	result_state(result);
	block_active = NULL;
	qualify_active = NULL;
	atomic_clear(&busy);
	return result->rc;
}

int nand_id_probe(struct nand_id_result *result)
{
	return probe(result, NULL, NULL, NULL, NULL, NULL, false);
}

int nand_feature_probe(struct nand_feature_result *result)
{
	if (result == NULL) { return -EINVAL; }
	memset(result, 0, sizeof(*result));
	return probe(&result->common, result, NULL, NULL, NULL, NULL, false);
}

int nand_page0_probe(struct nand_page0_result *result)
{
	if (result == NULL) { return -EINVAL; }
	memset(result, 0, sizeof(*result));
	result->ram_scrubbed = atomic_get(&page0_ram_scrubbed) != 0;
	return probe(&result->common, NULL, result, NULL, NULL, NULL, false);
}

int nand_marker_probe(struct nand_marker_result *result)
{
	if (result == NULL) { return -EINVAL; }
	memset(result, 0, sizeof(*result));
	result->ram_scrubbed = atomic_get(&marker_ram_scrubbed) != 0;
	return probe(&result->common, NULL, NULL, result, NULL, NULL, false);
}

int nand_block1024_probe(struct nand_block1024_result *result,
			 const struct nand_block1024_sink *sink)
{
	if (result == NULL || sink == NULL || sink->start == NULL ||
	    sink->begin == NULL || sink->page == NULL) { return -EINVAL; }
	if (!atomic_cas(&block_entry, 0, 1)) {
		/* A reentrant caller may have passed the active result itself. */
		return -EBUSY;
	}
	memset(result, 0, sizeof(*result));
	memset(&block_context, 0, sizeof(block_context));
	block_context.result = result; block_context.sink = sink;
	block_context.marker.ram_scrubbed = atomic_get(&marker_ram_scrubbed) != 0;
	int rc = probe(&block_context.marker.common, NULL, NULL, &block_context.marker, &block_context, NULL, false);
	if (rc != 0 && block_context.marker.outcome == NAND_MARKER_NOT_RUN) {
		/* Internal admission refusal, not proof of a hardware fault. Caller
		 * must not serialize this ordinary busy/already-used result as END. */
		block_context.marker.outcome = NAND_MARKER_PRECONDITION_ERROR;
		block_context.marker.primary_outcome = NAND_MARKER_PRECONDITION_ERROR;
		block_context.marker.primary_rc = rc;
	}
	block1024_result(&block_context);
	/* Retain only bounded metadata, not pointers to caller-owned transport. */
	block_context.sink = NULL; block_context.result = NULL;
	atomic_clear(&block_entry);
	return rc;
}

int nand_block1024_qualify(struct nand_qualify_result *result,
			 const struct nand_qualify_observer *observer)
{
	if (result == NULL || observer == NULL || observer->start == NULL ||
	    observer->check == NULL || observer->event == NULL) { return -EINVAL; }
	if (!atomic_cas(&qualify_entry, 0, 1)) { return -EBUSY; }
	memset(result, 0, sizeof(*result)); memset(&qualify_context, 0, sizeof(qualify_context));
	qualify_context.whole_hash = (psa_hash_operation_t)PSA_HASH_OPERATION_INIT;
	qualify_context.row_hash = (psa_hash_operation_t)PSA_HASH_OPERATION_INIT;
	qualify_context.result = result; qualify_context.observer = observer;
	result->words[NQ_ROW_INDEX] = UINT32_MAX; result->words[NQ_LAST_OPERATION] = UINT32_MAX;
	result->words[NQ_BLOCK_QUARANTINED] = 1;
	int rc = probe(&qualify_context.common, NULL, NULL, NULL, NULL, &qualify_context, false);
	/* Busy before ownership/one-shot refusal is not a hardware fault or END
	 * authorization. Root adapter must use actual rc plus start-arm state. */
	if (result->words[NQ_OUTCOME] == NQ_NOT_RUN && rc != 0) {
		result->words[NQ_OUTCOME] = result->words[NQ_PRIMARY_OUTCOME] = NQ_REFUSED;
		result->words[NQ_RC] = result->words[NQ_PRIMARY_RC] = (uint32_t)rc;
	}
	qualify_snapshot(&qualify_context);
	qualify_context.result = NULL; qualify_context.observer = NULL;
	atomic_clear(&qualify_entry);
	return rc;
}

#ifdef OPENPENDANT_PUBLIC_OBJECT
static int npo_run_admitted(struct npo_result *result, enum npo_mode mode,
                            const struct npo_observer *observer)
{
 memset(result,0,sizeof(*result));result->words[NP_MODE]=mode;
 result->words[NP_BLOCK_QUARANTINED]=1;
 npo_active_result=result;npo_active_observer=observer;npo_active_mode=mode;
 struct nand_id_result common;
 int rc=probe(&common,NULL,NULL,NULL,NULL,NULL,true);
 struct nand_id_state s;nand_id_get_state(&s);
 result->words[NP_STOPPED]=s.clock_stopped;result->words[NP_BUS_RELEASED]=s.bus_released;
 result->words[NP_FAULT]=s.fault_latched;
 result->words[NP_RAM_SCRUBBED]=atomic_get(&marker_ram_scrubbed)!=0;
 /* A refused call must never turn another diagnostic's prior ownership into
  * public-object recovery admission. Preserve only our own proven state. */
 if(result->words[NP_TRANSFERS])
  npo_previous_clean=result->words[NP_RESTORED]&&s.clock_stopped&&s.bus_released&&!s.fault_latched;
 else if(s.fault_latched)npo_previous_clean=false;
 if(rc){result->words[NP_RC]=(uint32_t)rc;result->words[NP_COMMITTED]=0;
  if(!result->words[NP_OUTCOME])result->words[NP_OUTCOME]=NPO_REFUSED;
  else if(result->words[NP_OUTCOME]==NPO_VERIFIED)result->words[NP_OUTCOME]=NPO_CLEANUP_ERROR;
 }
 npo_active_result=NULL;npo_active_observer=NULL;return rc;
}
int nand_public_object_run(struct npo_result *result, enum npo_mode mode,
                          const struct npo_observer *observer)
{
 if(!result||!observer||!observer->check||!observer->event||
    (mode!=NPO_WRITE_ONCE&&mode!=NPO_RECOVER_ONLY))return -EINVAL;
 if(!atomic_cas(&npo_entry,0,1))return -EBUSY; /* Active result untouched. */
 int rc=npo_run_admitted(result,mode,observer);atomic_clear(&npo_entry);return rc;
}
#endif
