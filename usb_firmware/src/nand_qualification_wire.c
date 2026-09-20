#include "nand_qualification_wire.h"
#include "nand_qualify.h"
#include <string.h>

enum { HELLO = 1, PROGRESS = 2, END = 3, ACK = 4, ABORT = 5 };
#if defined(__GNUC__)
#define WIRE_AUDIT_RETAIN __attribute__((used, retain))
#else
#define WIRE_AUDIT_RETAIN
#endif
static const uint8_t nand_qualification_wire_magic[8] WIRE_AUDIT_RETAIN = {'O','P','N','D','Q','L','1',0};
static const uint8_t nand_qualification_wire_ready[] WIRE_AUDIT_RETAIN =
    "NAND_QUALIFY_READY protocol=1 block=1024 erase_max=1 program_max=1\r\n";
/* Retained release-audit pins: protocol/header/nonce/body/events/prefix/
 * attempts/frame/data/no-progress bounds. No payload or target parameters. */
static const uint32_t nand_qualification_wire_bounds[10] WIRE_AUDIT_RETAIN = {
 1U,48U,16U,392U,256U,64U,8192U,2000U,180000U,250U
};
struct span { const uint8_t *bytes; size_t length; };
struct budget { uint32_t deadline; unsigned int attempts; bool data; };

static uint16_t get16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8));
}
static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void put16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)value; p[1] = (uint8_t)(value >> 8);
}
static void put32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value; p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16); p[3] = (uint8_t)(value >> 24);
}
static uint32_t crc_update(uint32_t crc, const uint8_t *bytes, size_t length)
{
    for (size_t i = 0; i < length; ++i) {
        crc ^= bytes[i];
        for (unsigned int bit = 0; bit < 8U; ++bit) {
            crc = (crc >> 1) ^ ((crc & 1U) ? UINT32_C(0xedb88320) : 0U);
        }
    }
    return crc;
}
uint32_t nand_qualification_wire_crc32(const uint8_t *bytes, size_t length)
{
    if (bytes == NULL && length != 0U) { return 0; }
    return crc_update(UINT32_MAX, bytes, length) ^ UINT32_MAX;
}
static bool before(uint32_t now, uint32_t deadline)
{
    uint32_t remaining = deadline - now;
    return remaining != 0U && remaining <= INT32_MAX;
}
static int fail(struct nand_qualification_wire *w, int rc, bool usable)
{
    w->last_rc = rc;
    w->data_stopped = true;
    w->quarantined = true;
    w->console_reuse_allowed = false;
    if (!usable) { w->transport_usable = false; }
    return rc;
}
static struct budget new_budget(struct nand_qualification_wire *w, bool data)
{
    uint32_t now = w->io.now_ms(w->io.user);
    struct budget b = {now + NAND_QUALIFICATION_WIRE_FRAME_MS, 0, data};
    if (data && before(now, w->data_deadline_ms) &&
        w->data_deadline_ms - now < NAND_QUALIFICATION_WIRE_FRAME_MS) { b.deadline = w->data_deadline_ms; }
    return b;
}
static int deadline_check(struct nand_qualification_wire *w, const struct budget *b)
{
    uint32_t now = w->io.now_ms(w->io.user);
    if (b->data && !before(now, w->data_deadline_ms)) {
        return fail(w, NAND_QUALIFICATION_WIRE_GLOBAL_TIMEOUT, false);
    }
    if (!before(now, b->deadline)) {
        return fail(w, NAND_QUALIFICATION_WIRE_TIMEOUT, false);
    }
    return 0;
}
static int permit(struct nand_qualification_wire *w, struct budget *b)
{
    int rc = deadline_check(w, b);
    if (rc != 0) { return rc; }
    if (b->attempts >= NAND_QUALIFICATION_WIRE_ATTEMPTS) {
        return fail(w, NAND_QUALIFICATION_WIRE_TIMEOUT, false);
    }
    ++b->attempts;
    return 0;
}
static int no_pending(struct nand_qualification_wire *w)
{
    bool pending = true;
    if (w->io.pending_rx(w->io.user, &pending) != 0) { return fail(w, NAND_QUALIFICATION_WIRE_IO, false); }
    if (pending) { return fail(w, NAND_QUALIFICATION_WIRE_PROTOCOL, false); }
    return 0;
}
static int write_spans(struct nand_qualification_wire *w, const struct span *spans, size_t count, struct budget *b)
{
    w->output_frame_boundary = false;
    for (size_t part = 0; part < count; ++part) {
        size_t offset = 0;
        while (offset < spans[part].length) {
            int rc = permit(w, b);
            if (rc != 0) { return rc; }
            size_t requested = spans[part].length - offset, accepted = 0;
            if (requested > NAND_QUALIFICATION_WIRE_IO_MAX) { requested = NAND_QUALIFICATION_WIRE_IO_MAX; }
            rc = w->io.write(w->io.user, spans[part].bytes + offset, requested, &accepted);
            if (accepted > requested) { return fail(w, NAND_QUALIFICATION_WIRE_IO, false); }
            offset += accepted;
            if (part + 1U == count && offset == spans[part].length) { w->output_frame_boundary = true; }
            if (rc != 0) { return fail(w, NAND_QUALIFICATION_WIRE_IO, false); }
            rc = deadline_check(w, b);
            if (rc != 0) { return rc; }
            if (accepted == 0U) { w->io.yield_us(w->io.user, 250U); }
        }
    }
    return 0;
}
static int read_exact(struct nand_qualification_wire *w, uint8_t *bytes, size_t length, struct budget *b)
{
    size_t offset = 0;
    while (offset < length) {
        int rc = permit(w, b);
        if (rc != 0) { return rc; }
        size_t requested = length - offset, received = 0;
        if (requested > NAND_QUALIFICATION_WIRE_IO_MAX) { requested = NAND_QUALIFICATION_WIRE_IO_MAX; }
        rc = w->io.read(w->io.user, bytes + offset, requested, &received);
        if (received > requested || rc != 0) { return fail(w, NAND_QUALIFICATION_WIRE_IO, false); }
        offset += received;
        rc = deadline_check(w, b);
        if (rc != 0) { return rc; }
        if (received == 0U) { w->io.yield_us(w->io.user, 250U); }
    }
    return 0;
}
static bool header_common(const uint8_t *p)
{
    return memcmp(p, nand_qualification_wire_magic, sizeof(nand_qualification_wire_magic)) == 0 && get16(p + 8) == 1U && get16(p + 12) == 48U &&
           get16(p + 14) == 0U && get32(p + 44) == nand_qualification_wire_crc32(p, 44U);
}
static void serialize(struct nand_qualification_wire *w, const uint32_t *fields, size_t count)
{
    for (size_t i = 0; i < count; ++i) { put32(w->tx_metadata + 4U * i, fields[i]); }
}
static uint32_t frame_header(struct nand_qualification_wire *w, uint16_t kind, uint16_t seq,
                             const struct span *bodies, size_t count, uint16_t body_length)
{
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < count; ++i) { crc = crc_update(crc, bodies[i].bytes, bodies[i].length); }
    crc ^= UINT32_MAX;
    memcpy(w->tx_header, nand_qualification_wire_magic, 8U); put16(w->tx_header + 8, 1U);
    put16(w->tx_header + 10, kind); put16(w->tx_header + 12, 48U); put16(w->tx_header + 14, 0U);
    memcpy(w->tx_header + 16, w->nonce, 16U);
    put16(w->tx_header + 32, seq); put16(w->tx_header + 34, body_length);
    put32(w->tx_header + 36, 65536U);
    put32(w->tx_header + 40, crc); put32(w->tx_header + 44, nand_qualification_wire_crc32(w->tx_header, 44U));
    return crc;
}
static int receive_ack(struct nand_qualification_wire *w, uint16_t sequence, uint32_t row,
                       uint32_t body_crc, uint32_t pages, struct budget *budget)
{
    int rc = read_exact(w, w->rx_control, 48U, budget);
    if (rc != 0) { return rc; }
    uint16_t kind = get16(w->rx_control + 10), size = get16(w->rx_control + 34);
    if (!header_common(w->rx_control) || memcmp(w->rx_control + 16, w->nonce, 16U) != 0 ||
        get16(w->rx_control + 32) != sequence || get32(w->rx_control + 36) != row ||
        !((kind == ACK && size == 8U) || (kind == ABORT && size == 4U))) {
        return fail(w, NAND_QUALIFICATION_WIRE_PROTOCOL, false);
    }
    rc = read_exact(w, w->rx_control + 48, size, budget);
    if (rc != 0) { return rc; }
    if (get32(w->rx_control + 40) != nand_qualification_wire_crc32(w->rx_control + 48, size)) {
        return fail(w, NAND_QUALIFICATION_WIRE_PROTOCOL, false);
    }
    rc = no_pending(w);
    if (rc != 0) { return rc; }
    rc = deadline_check(w, budget);
    if (rc != 0) { return rc; }
    if (kind == ABORT) {
        uint32_t reason = get32(w->rx_control + 48);
        if (reason < 1U || reason > 4U) { return fail(w, NAND_QUALIFICATION_WIRE_PROTOCOL, false); }
        w->host_abort_reason = (uint8_t)reason;
        return fail(w, NAND_QUALIFICATION_WIRE_HOST_ABORT, true);
    }
    if (get32(w->rx_control + 48) != body_crc || get32(w->rx_control + 52) != pages) {
        return fail(w, NAND_QUALIFICATION_WIRE_PROTOCOL, false);
    }
    return 0;
}
int nand_qualification_wire_init(struct nand_qualification_wire *w, const struct nand_qualification_wire_io *io)
{
    if (w == NULL || io == NULL || io->write == NULL || io->read == NULL || io->now_ms == NULL ||
        io->yield_us == NULL || io->pending_rx == NULL) { return NAND_QUALIFICATION_WIRE_ARGUMENT; }
    memset(w, 0, sizeof(*w)); w->io = *io;
    w->initialized = w->output_frame_boundary = w->transport_usable = w->quarantined = true;
    return 0;
}
int nand_qualification_wire_handshake(struct nand_qualification_wire *w)
{
    if (w == NULL || !w->initialized) { return NAND_QUALIFICATION_WIRE_ARGUMENT; }
    if (w->handshake_started) { return fail(w, NAND_QUALIFICATION_WIRE_STATE, false); }
    w->handshake_started = true;
    struct budget b = new_budget(w, false);
    const struct span line = {nand_qualification_wire_ready, sizeof(nand_qualification_wire_ready) - 1U};
    int rc = write_spans(w, &line, 1U, &b);
    if (rc != 0) { return rc; }
    rc = read_exact(w, w->rx_control, 48U, &b);
    if (rc != 0) { return rc; }
    uint8_t nonzero = 0;
    for (unsigned int i = 16; i < 32U; ++i) { nonzero |= w->rx_control[i]; }
    if (!header_common(w->rx_control) || get16(w->rx_control + 10) != HELLO ||
        get16(w->rx_control + 32) != UINT16_MAX || get16(w->rx_control + 34) != 0U ||
        get32(w->rx_control + 36) != UINT32_MAX || get32(w->rx_control + 40) != 0U || !nonzero) {
        return fail(w, NAND_QUALIFICATION_WIRE_PROTOCOL, false);
    }
    rc = no_pending(w);
    if (rc != 0) { return rc; }
    rc = deadline_check(w, &b);
    if (rc != 0) { return rc; }
    memcpy(w->nonce, w->rx_control + 16, 16U); w->hello_received = true;
    return 0;
}
int nand_qualification_wire_arm_data_deadline(struct nand_qualification_wire *w, uint32_t absolute_ms)
{
    if (w == NULL || !w->initialized) { return NAND_QUALIFICATION_WIRE_ARGUMENT; }
    if (!w->hello_received || w->deadline_armed || w->data_stopped || w->end_attempted) {
        return fail(w, NAND_QUALIFICATION_WIRE_STATE, false);
    }
    uint32_t now = w->io.now_ms(w->io.user);
    if (!before(now, absolute_ms) || absolute_ms - now > NAND_QUALIFICATION_WIRE_DATA_MS) {
        return fail(w, NAND_QUALIFICATION_WIRE_GLOBAL_TIMEOUT, true);
    }
    w->data_deadline_ms = absolute_ms; w->deadline_armed = true;
    return 0;
}
static bool data_admitted(const struct nand_qualification_wire *w)
{
    return w->hello_received && w->deadline_armed && w->transport_usable && w->output_frame_boundary &&
           !w->data_stopped && !w->end_attempted;
}

/* Finite metadata fields only. No raw array bytes or unpinned hashes exist in
 * this API. Semantic phase/operation validation remains the driver's duty;
 * the host independently validates its closed state machine before ACK. */
static bool metadata_valid(const uint32_t *f)
{
    static const uint8_t booleans[] = {
        17,18,20,22,24,25,26,28,29,30,32,33,34,40,42,43,44,49,50,51,52,
        54,55,56,58,59,60,63,64,65,67,69,71,72,74,75,76,77,78,79,80,81,82,
        85,86,87,89,94
    };
    static const uint8_t signed_values[] = {0,2,37,47,53,84,93};
    static const uint8_t byte_values[] = {19,21,23,31,35,41,45,57,66,73};
    if (f == NULL) { return false; }
    for (size_t i=0;i<sizeof(booleans);++i) { if(f[booleans[i]]>1U) return false; }
    for (size_t i=0;i<sizeof(signed_values);++i) {
        if(f[signed_values[i]]!=0U && f[signed_values[i]]<=INT32_MAX) return false;
    }
    for (size_t i=0;i<sizeof(byte_values);++i) { if(f[byte_values[i]]>255U) return false; }
    return f[NQ_OUTCOME]<=NQ_CLEANUP_ERROR && f[NQ_PRIMARY_OUTCOME]<=NQ_CLEANUP_ERROR &&
        f[NQ_PHASE]<=NQ_PHASE_DONE && f[NQ_FAILED_PHASE]<=NQ_PHASE_DONE &&
        (f[NQ_ROW_INDEX]==UINT32_MAX || f[NQ_ROW_INDEX]<64U) &&
        f[NQ_TRANSFERS]<=NAND_QUALIFY_MAX_STARTS &&
        f[NQ_PREIMAGE_ROWS]<=64U && f[NQ_BLANK_ROWS]<=64U && f[NQ_VERIFY_READS]<=65U &&
        f[NQ_PREIMAGE_POLLS]<=1024U && f[NQ_BLANK_POLLS]<=512U &&
        f[NQ_VERIFY_POLLS]<=520U && f[NQ_MARKER_POLLS]<=8U &&
        f[NQ_ERASE_POLLS]<=128U && f[NQ_PROGRAM_POLLS]<=32U &&
        f[NQ_MARKER_BEFORE]<=65535U && f[NQ_MARKER_AFTER]<=65535U &&
        f[NQ_A0_RESTORE_STATE]<=3U && f[NQ_B0_RESTORE_STATE]<=3U &&
        f[NQ_B0_OFF_STARTS]<=2U && f[NQ_B0_ON_STARTS]<=2U && f[NQ_WREN_STARTS]<=2U &&
        f[NQ_PROGRAM_LOAD_TX]<=4099U && f[NQ_PROGRAM_LOAD_RX]<=4099U &&
        f[NQ_PATTERN_READS]<=2U && f[NQ_BLOCK_QUARANTINED]==1U &&
        (f[NQ_LAST_OPERATION]==UINT32_MAX || f[NQ_LAST_OPERATION]<=21U) &&
        f[NQ_LAST_EXPECTED_LENGTH]<=4356U && f[NQ_LAST_TX_AMOUNT]<=4356U &&
        f[NQ_LAST_RX_AMOUNT]<=4356U && f[NQ_LAST_CONTROL]<=255U &&
        f[NQ_EVENT]<=NQ_EVENT_AFTER_PROGRAM && f[NQ_EVENT_COUNT]<=256U;
}
bool nand_qualification_wire_clean_result(const uint32_t f[98])
{
    return metadata_valid(f) && f[NQ_RC]==0U && f[NQ_OUTCOME]==NQ_VERIFIED &&
        f[NQ_PRIMARY_RC]==0U && f[NQ_PRIMARY_OUTCOME]==NQ_VERIFIED &&
        f[NQ_PHASE]==NQ_PHASE_DONE && f[NQ_FAILED_PHASE]==0U && f[NQ_ROW_INDEX]==0U &&
        f[NQ_PREIMAGE_ROWS]==64U && f[NQ_BLANK_ROWS]==64U && f[NQ_VERIFY_READS]==65U &&
        f[NQ_PREIMAGE_POLLS]>=128U && f[NQ_BLANK_POLLS]>=64U && f[NQ_VERIFY_POLLS]>=65U &&
        f[NQ_MARKER_POLLS]>=1U && f[NQ_ERASE_POLLS]>=1U && f[NQ_PROGRAM_POLLS]>=1U &&
        f[NQ_TRANSFERS]==1003U+2U*f[NQ_WRDI_ATTEMPTED]+f[NQ_PREIMAGE_POLLS]+f[NQ_BLANK_POLLS]+f[NQ_VERIFY_POLLS]+
            f[NQ_MARKER_POLLS]+f[NQ_ERASE_POLLS]+f[NQ_PROGRAM_POLLS] &&
        f[NQ_ID_VALID]==1U && f[NQ_INITIAL_VALID]==1U && f[NQ_INITIAL_C0]==0U &&
        f[NQ_INITIAL_A0_VALID]==1U && f[NQ_INITIAL_A0]==0x7cU &&
        f[NQ_INITIAL_B0_VALID]==1U && f[NQ_INITIAL_B0]==0x10U &&
        f[NQ_PREIMAGE_HASH_VALID]==1U && f[NQ_PREIMAGE_MATCH]==1U &&
        f[NQ_MARKER_BEFORE_VALID]==1U && f[NQ_MARKER_BEFORE]==0xffffU &&
        f[NQ_A0_CHANGE_ATTEMPTED]==1U && f[NQ_A0_CHANGE_CONFIRMED]==1U &&
        f[NQ_A0_CURRENT_VALID]==1U && f[NQ_A0_CURRENT]==0x7cU &&
        f[NQ_A0_RESTORE_REQUIRED]==0U && f[NQ_A0_RESTORE_ATTEMPTED]==1U &&
        f[NQ_A0_RESTORE_VALID]==1U && f[NQ_A0_RESTORE_VALUE]==0x7cU &&
        f[NQ_A0_RESTORE_STATE]==NQ_RESTORE_VERIFIED && f[NQ_A0_RESTORE_RC]==0U &&
        f[NQ_B0_CURRENT_VALID]==1U && f[NQ_B0_CURRENT]==0x10U &&
        f[NQ_B0_RESTORE_REQUIRED]==0U && f[NQ_B0_RESTORE_ATTEMPTED]==1U &&
        f[NQ_B0_RESTORE_VALID]==1U && f[NQ_B0_RESTORE_VALUE]==0x10U &&
        f[NQ_B0_RESTORE_STATE]==NQ_RESTORE_VERIFIED && f[NQ_B0_RESTORE_RC]==0U &&
        f[NQ_B0_OFF_STARTS]==2U && f[NQ_B0_ON_STARTS]==2U &&
        f[NQ_WREN_STARTS]==2U && f[NQ_WEL_VALID]==1U && f[NQ_WEL]==0U &&
        f[NQ_WRDI_ATTEMPTED]==f[NQ_WRDI_VERIFIED] &&
        f[NQ_DISARM_RC]==0U && f[NQ_ERASE_STARTED]==1U && f[NQ_ERASE_TRANSFER_VALID]==1U &&
        f[NQ_ERASE_READY_VALID]==1U && (f[NQ_ERASE_STATUS]&0x8fU)==0U && f[NQ_ERASE_COMPLETED]==1U &&
        f[NQ_PROGRAM_LOAD_STARTED]==1U && f[NQ_PROGRAM_LOAD_VALID]==1U &&
        f[NQ_PROGRAM_LOAD_TX]==4099U && f[NQ_PROGRAM_LOAD_RX]==4099U &&
        f[NQ_PROGRAM_STARTED]==1U && f[NQ_PROGRAM_TRANSFER_VALID]==1U &&
        f[NQ_PROGRAM_READY_VALID]==1U && (f[NQ_PROGRAM_STATUS]&0x8fU)==0U && f[NQ_PROGRAM_COMPLETED]==1U &&
        f[NQ_PATTERN_READS]==2U && f[NQ_MARKER_AFTER_VALID]==1U && f[NQ_MARKER_AFTER]==0xffffU &&
        f[NQ_ARRAY_MAY_HAVE_CHANGED]==1U && f[NQ_FINAL_VALID]==1U && (f[NQ_FINAL_C0]&0x8fU)==0U &&
        f[NQ_STOPPED]==1U && f[NQ_BUS_RELEASED]==1U && f[NQ_FAULT]==0U &&
        f[NQ_RAM_SCRUBBED]==1U && f[NQ_READY_UNKNOWN]==0U &&
        f[NQ_CS_CONFIGURED]==1U && f[NQ_CS_HIGH]==1U &&
        f[NQ_AUX_CONFIGURED]==1U && f[NQ_AUX_HIGH]==1U &&
        f[NQ_OBSERVER_RC]==0U && f[NQ_ATTEMPTED]==1U && f[NQ_BUSY]==0U &&
        f[NQ_LAST_OPERATION]==3U && f[NQ_LAST_STARTED]==1U &&
        f[NQ_LAST_EXPECTED_LENGTH]==3U && f[NQ_LAST_TX_AMOUNT]==3U && f[NQ_LAST_RX_AMOUNT]==3U &&
        f[NQ_LAST_RC]==0U && f[NQ_LAST_CONTROL_VALID]==1U && f[NQ_LAST_CONTROL]==f[NQ_FINAL_C0] &&
        f[NQ_EVENT]==NQ_EVENT_PHASE && f[NQ_EVENT_COUNT]>0U;
}
static bool progress_valid(const uint32_t *f)
{
    if(!metadata_valid(f) || f[NQ_RC] || f[NQ_OUTCOME] || f[NQ_PRIMARY_RC] ||
       f[NQ_PRIMARY_OUTCOME] || f[NQ_FAILED_PHASE] || f[NQ_OBSERVER_RC] ||
       f[NQ_EVENT]<1U || f[NQ_STOPPED]!=1U || f[NQ_READY_UNKNOWN] || f[NQ_FAULT] ||
       f[NQ_ATTEMPTED]!=1U || f[NQ_BUSY]!=1U || f[NQ_CS_CONFIGURED]!=1U ||
       f[NQ_CS_HIGH]!=1U || f[NQ_AUX_CONFIGURED]!=1U || f[NQ_AUX_HIGH]!=1U ||
       f[NQ_PHASE]<NQ_PHASE_PREIMAGE || f[NQ_PHASE]>NQ_PHASE_MARKER ||
       f[NQ_ID_VALID]!=1U || f[NQ_INITIAL_VALID]!=1U || f[NQ_INITIAL_C0]!=0U ||
       f[NQ_INITIAL_A0_VALID]!=1U || f[NQ_INITIAL_A0]!=124U ||
       f[NQ_INITIAL_B0_VALID]!=1U || f[NQ_INITIAL_B0]!=16U ||
       f[NQ_A0_CURRENT_VALID]!=1U || f[NQ_B0_CURRENT_VALID]!=1U || f[NQ_TRANSFERS]<10U)
        return false;
    if(f[NQ_PHASE]>=NQ_PHASE_PROTECTION &&
       (f[NQ_PREIMAGE_ROWS]!=64U || f[NQ_PREIMAGE_HASH_VALID]!=1U || f[NQ_PREIMAGE_MATCH]!=1U ||
        f[NQ_MARKER_BEFORE_VALID]!=1U || f[NQ_MARKER_BEFORE]!=65535U)) return false;
    if(f[NQ_EVENT]==NQ_EVENT_PHASE) return true;
    if(f[NQ_EVENT]==NQ_EVENT_PROGRESS)
        return f[NQ_PHASE]==NQ_PHASE_PREIMAGE || f[NQ_PHASE]==NQ_PHASE_BLANK || f[NQ_PHASE]==NQ_PHASE_VERIFY;
    if(f[NQ_A0_CURRENT]!=0x48U || f[NQ_A0_CHANGE_CONFIRMED]!=1U || f[NQ_WEL_VALID]!=1U) return false;
    if(f[NQ_EVENT]>=NQ_EVENT_BEFORE_ERASE_WREN && f[NQ_EVENT]<=NQ_EVENT_AFTER_ERASE) {
        if(f[NQ_PHASE]!=NQ_PHASE_ERASE || f[NQ_B0_CURRENT]!=0U || f[NQ_PROGRAM_STARTED]) return false;
        if(f[NQ_EVENT]==NQ_EVENT_BEFORE_ERASE_WREN)
            return f[NQ_WREN_STARTS]==0U && f[NQ_WEL]==0U && !f[NQ_ERASE_STARTED];
        if(f[NQ_EVENT]==NQ_EVENT_BEFORE_ERASE_EXECUTE)
            return f[NQ_WREN_STARTS]==1U && f[NQ_WEL]==1U && !f[NQ_ERASE_STARTED];
        return f[NQ_WREN_STARTS]==1U && f[NQ_WEL]==0U && f[NQ_ERASE_STARTED]==1U &&
            f[NQ_ERASE_TRANSFER_VALID]==1U && f[NQ_ERASE_READY_VALID]==1U &&
            f[NQ_ERASE_COMPLETED]==1U && (f[NQ_ERASE_STATUS]&0x8fU)==0U;
    }
    if(f[NQ_PHASE]!=NQ_PHASE_PROGRAM || f[NQ_B0_CURRENT]!=16U ||
       f[NQ_ERASE_COMPLETED]!=1U || f[NQ_BLANK_ROWS]!=64U) return false;
    if(f[NQ_EVENT]==NQ_EVENT_BEFORE_PROGRAM_WREN)
        return f[NQ_WREN_STARTS]==1U && f[NQ_WEL]==0U && !f[NQ_PROGRAM_LOAD_STARTED] && !f[NQ_PROGRAM_STARTED];
    if(f[NQ_EVENT]==NQ_EVENT_BEFORE_PROGRAM_LOAD)
        return f[NQ_WREN_STARTS]==2U && f[NQ_WEL]==1U && !f[NQ_PROGRAM_LOAD_STARTED] && !f[NQ_PROGRAM_STARTED];
    if(f[NQ_PROGRAM_LOAD_STARTED]!=1U || f[NQ_PROGRAM_LOAD_VALID]!=1U ||
       f[NQ_PROGRAM_LOAD_TX]!=4099U || f[NQ_PROGRAM_LOAD_RX]!=4099U || f[NQ_WREN_STARTS]!=2U) return false;
    if(f[NQ_EVENT]==NQ_EVENT_BEFORE_PROGRAM_EXECUTE) return f[NQ_WEL]==1U && !f[NQ_PROGRAM_STARTED];
    return f[NQ_EVENT]==NQ_EVENT_AFTER_PROGRAM && f[NQ_WEL]==0U && f[NQ_PROGRAM_STARTED]==1U &&
        f[NQ_PROGRAM_TRANSFER_VALID]==1U && f[NQ_PROGRAM_READY_VALID]==1U &&
        f[NQ_PROGRAM_COMPLETED]==1U && (f[NQ_PROGRAM_STATUS]&0x8fU)==0U;
}
int nand_qualification_wire_check(struct nand_qualification_wire *w)
{
    if(w==NULL || !w->initialized) return NAND_QUALIFICATION_WIRE_ARGUMENT;
    if(!data_admitted(w)) return fail(w,NAND_QUALIFICATION_WIRE_STATE,false);
    struct budget b=new_budget(w,true);
    int rc=deadline_check(w,&b);
    if(rc==0) rc=no_pending(w);
    if(rc==0) rc=deadline_check(w,&b);
    return rc;
}
int nand_qualification_wire_progress(struct nand_qualification_wire *w,const uint32_t fields[98])
{
    if(w==NULL || !w->initialized) return NAND_QUALIFICATION_WIRE_ARGUMENT;
    if(!data_admitted(w) || w->events_sent!=w->events_acked ||
       w->events_sent>=NAND_QUALIFICATION_WIRE_EVENTS)
        return fail(w,NAND_QUALIFICATION_WIRE_STATE,false);
    if(!progress_valid(fields) || fields[NQ_EVENT_COUNT]!=(uint32_t)w->events_sent+1U)
        return fail(w,NAND_QUALIFICATION_WIRE_ARGUMENT,true);
    struct budget budget=new_budget(w,true);
    int rc=deadline_check(w,&budget);
    if(rc==0) rc=no_pending(w);
    if(rc==0) rc=deadline_check(w,&budget);
    if(rc!=0) return rc;
    serialize(w,fields,NAND_QUALIFICATION_WIRE_WORDS);
    uint16_t sequence=(uint16_t)(w->events_sent+1U);
    const struct span body={w->tx_metadata,NAND_QUALIFICATION_WIRE_BODY};
    uint32_t crc=frame_header(w,PROGRESS,sequence,&body,1U,NAND_QUALIFICATION_WIRE_BODY);
    const struct span spans[2]={{w->tx_header,48U},body};
    rc=write_spans(w,spans,2U,&budget);
    if(w->output_frame_boundary) ++w->events_sent;
    if(rc!=0) return rc;
    rc=receive_ack(w,sequence,65536U,crc,sequence,&budget);
    if(rc==0) ++w->events_acked;
    return rc;
}
int nand_qualification_wire_finish(struct nand_qualification_wire *w,const uint32_t fields[98],bool clean)
{
    if(w==NULL || !w->initialized) return NAND_QUALIFICATION_WIRE_ARGUMENT;
    if(!w->hello_received || !w->transport_usable || !w->output_frame_boundary || w->end_attempted)
        return fail(w,NAND_QUALIFICATION_WIRE_STATE,false);
    w->end_attempted=true;
    if(!metadata_valid(fields) || fields[NQ_EVENT_COUNT]<w->events_sent ||
       fields[NQ_EVENT_COUNT]>(uint32_t)w->events_sent+1U ||
       (clean ? (!nand_qualification_wire_clean_result(fields) || w->data_stopped ||
                 w->events_sent!=w->events_acked || fields[NQ_EVENT_COUNT]!=w->events_acked) :
                (fields[NQ_RC]==0U || fields[NQ_OUTCOME]==NQ_VERIFIED)))
        return fail(w,NAND_QUALIFICATION_WIRE_ARGUMENT,true);
    /* NAND restoration owns its separate budget. This final frame never
     * renews the expired data allowance or grants another NAND operation. */
    struct budget budget=new_budget(w,false);
    int rc=deadline_check(w,&budget);
    if(rc==0) rc=no_pending(w);
    if(rc==0) rc=deadline_check(w,&budget);
    if(rc!=0) return rc;
    serialize(w,fields,NAND_QUALIFICATION_WIRE_WORDS);
    uint16_t sequence=(uint16_t)(w->events_sent+1U);
    const struct span body={w->tx_metadata,NAND_QUALIFICATION_WIRE_BODY};
    uint32_t crc=frame_header(w,END,sequence,&body,1U,NAND_QUALIFICATION_WIRE_BODY);
    const struct span spans[2]={{w->tx_header,48U},body};
    rc=write_spans(w,spans,2U,&budget);
    if(w->output_frame_boundary) w->end_sent=true;
    if(rc!=0) return rc;
    if(!clean) { w->data_stopped=true; return 0; }
    rc=receive_ack(w,sequence,65536U,crc,w->events_acked,&budget);
    if(rc==0) {
        w->clean_end_acknowledged=true; w->console_reuse_allowed=true;
        w->quarantined=false;
    }
    return rc;
}
