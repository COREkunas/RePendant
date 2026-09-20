#include "nand_backup_wire.h"
#include <string.h>

enum { HELLO = 1, BEGIN = 2, PAGE = 3, END = 4, ACK = 5, ABORT = 6 };
#if defined(__GNUC__)
#define WIRE_AUDIT_RETAIN __attribute__((used, retain))
#else
#define WIRE_AUDIT_RETAIN
#endif
static const uint8_t nand_backup_wire_magic[8] WIRE_AUDIT_RETAIN = {'O','P','N','D','B','K','1',0};
static const uint8_t nand_backup_wire_ready[] WIRE_AUDIT_RETAIN =
    "NAND_BACKUP_READY protocol=1 block=1024 rows=64 raw_bytes=278528\r\n";
/* Protocol, header, nonce, BEGIN/PAGE metadata, raw page, END body, maximum
 * prefix, attempts, frame milliseconds, data milliseconds, no-progress usec.
 * Retained immutable words let the release auditor pin the actual ELF. */
static const uint32_t nand_backup_wire_bounds[11] WIRE_AUDIT_RETAIN = {
    1U, NAND_BACKUP_WIRE_HEADER, 16U, 64U, NAND_BACKUP_WIRE_RAW, 176U,
    NAND_BACKUP_WIRE_IO_MAX, NAND_BACKUP_WIRE_ATTEMPTS, NAND_BACKUP_WIRE_FRAME_MS,
    NAND_BACKUP_WIRE_DATA_MS, 250U,
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
uint32_t nand_backup_wire_crc32(const uint8_t *bytes, size_t length)
{
    if (bytes == NULL && length != 0U) { return 0; }
    return crc_update(UINT32_MAX, bytes, length) ^ UINT32_MAX;
}
static bool before(uint32_t now, uint32_t deadline)
{
    uint32_t remaining = deadline - now;
    return remaining != 0U && remaining <= INT32_MAX;
}
static int fail(struct nand_backup_wire *w, int rc, bool usable)
{
    w->last_rc = rc;
    w->data_stopped = true;
    w->quarantined = true;
    w->console_reuse_allowed = false;
    if (!usable) { w->transport_usable = false; }
    return rc;
}
static struct budget new_budget(struct nand_backup_wire *w, bool data)
{
    uint32_t now = w->io.now_ms(w->io.user);
    struct budget b = {now + NAND_BACKUP_WIRE_FRAME_MS, 0, data};
    if (data && before(now, w->data_deadline_ms) &&
        w->data_deadline_ms - now < NAND_BACKUP_WIRE_FRAME_MS) { b.deadline = w->data_deadline_ms; }
    return b;
}
static int deadline_check(struct nand_backup_wire *w, const struct budget *b)
{
    uint32_t now = w->io.now_ms(w->io.user);
    if (b->data && !before(now, w->data_deadline_ms)) {
        return fail(w, NAND_BACKUP_WIRE_GLOBAL_TIMEOUT, false);
    }
    if (!before(now, b->deadline)) {
        return fail(w, NAND_BACKUP_WIRE_TIMEOUT, false);
    }
    return 0;
}
static int permit(struct nand_backup_wire *w, struct budget *b)
{
    int rc = deadline_check(w, b);
    if (rc != 0) { return rc; }
    if (b->attempts >= NAND_BACKUP_WIRE_ATTEMPTS) {
        return fail(w, NAND_BACKUP_WIRE_TIMEOUT, false);
    }
    ++b->attempts;
    return 0;
}
static int no_pending(struct nand_backup_wire *w)
{
    bool pending = true;
    if (w->io.pending_rx(w->io.user, &pending) != 0) { return fail(w, NAND_BACKUP_WIRE_IO, false); }
    if (pending) { return fail(w, NAND_BACKUP_WIRE_PROTOCOL, false); }
    return 0;
}
static int write_spans(struct nand_backup_wire *w, const struct span *spans, size_t count, struct budget *b)
{
    w->output_frame_boundary = false;
    for (size_t part = 0; part < count; ++part) {
        size_t offset = 0;
        while (offset < spans[part].length) {
            int rc = permit(w, b);
            if (rc != 0) { return rc; }
            size_t requested = spans[part].length - offset, accepted = 0;
            if (requested > NAND_BACKUP_WIRE_IO_MAX) { requested = NAND_BACKUP_WIRE_IO_MAX; }
            rc = w->io.write(w->io.user, spans[part].bytes + offset, requested, &accepted);
            if (accepted > requested) { return fail(w, NAND_BACKUP_WIRE_IO, false); }
            offset += accepted;
            if (part + 1U == count && offset == spans[part].length) { w->output_frame_boundary = true; }
            if (rc != 0) { return fail(w, NAND_BACKUP_WIRE_IO, false); }
            rc = deadline_check(w, b);
            if (rc != 0) { return rc; }
            if (accepted == 0U) { w->io.yield_us(w->io.user, 250U); }
        }
    }
    return 0;
}
static int read_exact(struct nand_backup_wire *w, uint8_t *bytes, size_t length, struct budget *b)
{
    size_t offset = 0;
    while (offset < length) {
        int rc = permit(w, b);
        if (rc != 0) { return rc; }
        size_t requested = length - offset, received = 0;
        if (requested > NAND_BACKUP_WIRE_IO_MAX) { requested = NAND_BACKUP_WIRE_IO_MAX; }
        rc = w->io.read(w->io.user, bytes + offset, requested, &received);
        if (received > requested || rc != 0) { return fail(w, NAND_BACKUP_WIRE_IO, false); }
        offset += received;
        rc = deadline_check(w, b);
        if (rc != 0) { return rc; }
        if (received == 0U) { w->io.yield_us(w->io.user, 250U); }
    }
    return 0;
}
static bool header_common(const uint8_t *p)
{
    return memcmp(p, nand_backup_wire_magic, sizeof(nand_backup_wire_magic)) == 0 && get16(p + 8) == 1U && get16(p + 12) == 48U &&
           get16(p + 14) == 0U && get32(p + 44) == nand_backup_wire_crc32(p, 44U);
}
static void serialize(struct nand_backup_wire *w, const uint32_t *fields, size_t count)
{
    for (size_t i = 0; i < count; ++i) { put32(w->tx_metadata + 4U * i, fields[i]); }
}
static uint32_t frame_header(struct nand_backup_wire *w, uint16_t kind, uint16_t seq,
                             const struct span *bodies, size_t count, uint16_t body_length)
{
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < count; ++i) { crc = crc_update(crc, bodies[i].bytes, bodies[i].length); }
    crc ^= UINT32_MAX;
    memcpy(w->tx_header, nand_backup_wire_magic, 8U); put16(w->tx_header + 8, 1U);
    put16(w->tx_header + 10, kind); put16(w->tx_header + 12, 48U); put16(w->tx_header + 14, 0U);
    memcpy(w->tx_header + 16, w->nonce, 16U);
    put16(w->tx_header + 32, seq); put16(w->tx_header + 34, body_length);
    put32(w->tx_header + 36, kind == PAGE ? 65535U + seq : 65536U);
    put32(w->tx_header + 40, crc); put32(w->tx_header + 44, nand_backup_wire_crc32(w->tx_header, 44U));
    return crc;
}
static int receive_ack(struct nand_backup_wire *w, uint16_t sequence, uint32_t row,
                       uint32_t body_crc, uint32_t pages, struct budget *budget)
{
    int rc = read_exact(w, w->rx_control, 48U, budget);
    if (rc != 0) { return rc; }
    uint16_t kind = get16(w->rx_control + 10), size = get16(w->rx_control + 34);
    if (!header_common(w->rx_control) || memcmp(w->rx_control + 16, w->nonce, 16U) != 0 ||
        get16(w->rx_control + 32) != sequence || get32(w->rx_control + 36) != row ||
        !((kind == ACK && size == 8U) || (kind == ABORT && size == 4U))) {
        return fail(w, NAND_BACKUP_WIRE_PROTOCOL, false);
    }
    rc = read_exact(w, w->rx_control + 48, size, budget);
    if (rc != 0) { return rc; }
    if (get32(w->rx_control + 40) != nand_backup_wire_crc32(w->rx_control + 48, size)) {
        return fail(w, NAND_BACKUP_WIRE_PROTOCOL, false);
    }
    rc = no_pending(w);
    if (rc != 0) { return rc; }
    rc = deadline_check(w, budget);
    if (rc != 0) { return rc; }
    if (kind == ABORT) {
        uint32_t reason = get32(w->rx_control + 48);
        if (reason < 1U || reason > 4U) { return fail(w, NAND_BACKUP_WIRE_PROTOCOL, false); }
        w->host_abort_reason = (uint8_t)reason;
        return fail(w, NAND_BACKUP_WIRE_HOST_ABORT, true);
    }
    if (get32(w->rx_control + 48) != body_crc || get32(w->rx_control + 52) != pages) {
        return fail(w, NAND_BACKUP_WIRE_PROTOCOL, false);
    }
    return 0;
}
int nand_backup_wire_init(struct nand_backup_wire *w, const struct nand_backup_wire_io *io)
{
    if (w == NULL || io == NULL || io->write == NULL || io->read == NULL || io->now_ms == NULL ||
        io->yield_us == NULL || io->pending_rx == NULL) { return NAND_BACKUP_WIRE_ARGUMENT; }
    memset(w, 0, sizeof(*w)); w->io = *io;
    w->initialized = w->output_frame_boundary = w->transport_usable = w->quarantined = true;
    w->block_crc_state = UINT32_MAX;
    return 0;
}
int nand_backup_wire_handshake(struct nand_backup_wire *w)
{
    if (w == NULL || !w->initialized) { return NAND_BACKUP_WIRE_ARGUMENT; }
    if (w->handshake_started) { return fail(w, NAND_BACKUP_WIRE_STATE, false); }
    w->handshake_started = true;
    struct budget b = new_budget(w, false);
    const struct span line = {nand_backup_wire_ready, sizeof(nand_backup_wire_ready) - 1U};
    int rc = write_spans(w, &line, 1U, &b);
    if (rc != 0) { return rc; }
    rc = read_exact(w, w->rx_control, 48U, &b);
    if (rc != 0) { return rc; }
    uint8_t nonzero = 0;
    for (unsigned int i = 16; i < 32U; ++i) { nonzero |= w->rx_control[i]; }
    if (!header_common(w->rx_control) || get16(w->rx_control + 10) != HELLO ||
        get16(w->rx_control + 32) != UINT16_MAX || get16(w->rx_control + 34) != 0U ||
        get32(w->rx_control + 36) != UINT32_MAX || get32(w->rx_control + 40) != 0U || !nonzero) {
        return fail(w, NAND_BACKUP_WIRE_PROTOCOL, false);
    }
    rc = no_pending(w);
    if (rc != 0) { return rc; }
    rc = deadline_check(w, &b);
    if (rc != 0) { return rc; }
    memcpy(w->nonce, w->rx_control + 16, 16U); w->hello_received = true;
    return 0;
}
int nand_backup_wire_arm_data_deadline(struct nand_backup_wire *w, uint32_t absolute_ms)
{
    if (w == NULL || !w->initialized) { return NAND_BACKUP_WIRE_ARGUMENT; }
    if (!w->hello_received || w->deadline_armed || w->data_stopped || w->end_attempted) {
        return fail(w, NAND_BACKUP_WIRE_STATE, false);
    }
    uint32_t now = w->io.now_ms(w->io.user);
    if (!before(now, absolute_ms) || absolute_ms - now > NAND_BACKUP_WIRE_DATA_MS) {
        return fail(w, NAND_BACKUP_WIRE_GLOBAL_TIMEOUT, true);
    }
    w->data_deadline_ms = absolute_ms; w->deadline_armed = true;
    return 0;
}
static bool data_admitted(const struct nand_backup_wire *w)
{
    return w->hello_received && w->deadline_armed && w->transport_usable && w->output_frame_boundary &&
           !w->data_stopped && !w->end_attempted;
}
int nand_backup_wire_send_begin(struct nand_backup_wire *w, const uint32_t fields[16])
{
    if (w == NULL || !w->initialized) { return NAND_BACKUP_WIRE_ARGUMENT; }
    if (!data_admitted(w) || w->begin_acked) { return fail(w, NAND_BACKUP_WIRE_STATE, false); }
    static const uint32_t fixed[12] = {1024,65536,64,4352,278528,15,0x2c35,3,0,0x7c,0x10,0};
    if (fields == NULL || memcmp(fields, fixed, sizeof(fixed)) != 0 || (fields[12] & 0x8fU) != 0U ||
        fields[12] > 255U || fields[13] != 10U || fields[14] || fields[15]) {
        return fail(w, NAND_BACKUP_WIRE_ARGUMENT, true);
    }
    struct budget budget = new_budget(w, true);
    serialize(w, fields, 16U);
    const struct span body = {w->tx_metadata, 64U};
    uint32_t crc = frame_header(w, BEGIN, 0U, &body, 1U, 64U);
    const struct span spans[2] = {{w->tx_header, 48U}, body};
    int rc = write_spans(w, spans, 2U, &budget);
    if (w->output_frame_boundary) { w->begin_sent = true; }
    if (rc != 0) { return rc; }
    rc = receive_ack(w, 0U, 65536U, crc, 0U, &budget);
    if (rc == 0) { w->begin_acked = true; }
    return rc;
}
int nand_backup_wire_send_page(struct nand_backup_wire *w, const uint32_t fields[16], const uint8_t raw[4352])
{
    if (w == NULL || !w->initialized) { return NAND_BACKUP_WIRE_ARGUMENT; }
    if (!data_admitted(w) || !w->begin_acked || w->rows_sent != w->rows_acked || w->rows_acked >= 64U) {
        return fail(w, NAND_BACKUP_WIRE_STATE, false);
    }
    if (fields == NULL || raw == NULL || fields[0] != fields[1] || fields[2] != 1U || fields[3] != 7U ||
        fields[4] < 1U || fields[4] > 8U || fields[5] < 1U || fields[5] > 8U || fields[10] != 0U ||
        fields[11] != 7U + fields[4] + fields[5] || fields[12] != 4356U || fields[13] != 4356U ||
        fields[14] != 4356U || fields[15] != 4356U) { return fail(w, NAND_BACKUP_WIRE_ARGUMENT, true); }
    for (unsigned int i = 6; i < 10U; ++i) {
        if (fields[i] > 255U || (fields[i] & 0x8fU) != 0U) { return fail(w, NAND_BACKUP_WIRE_ARGUMENT, true); }
    }
    struct budget budget = new_budget(w, true);
    if (fields[0] != nand_backup_wire_crc32(raw, 4352U)) { return fail(w, NAND_BACKUP_WIRE_ARGUMENT, true); }
    serialize(w, fields, 16U);
    uint16_t sequence = (uint16_t)(w->rows_acked + 1U);
    const struct span bodies[2] = {{w->tx_metadata, 64U}, {raw, 4352U}};
    uint32_t crc = frame_header(w, PAGE, sequence, bodies, 2U, 4416U);
    const struct span spans[3] = {{w->tx_header, 48U}, bodies[0], bodies[1]};
    int rc = write_spans(w, spans, 3U, &budget);
    /* Accepted bytes were synchronously copied even if the callback then
     * reported an error or the clock crossed a deadline. Not a durable ACK. */
    if (w->output_frame_boundary) { ++w->rows_sent; }
    if (rc != 0) { return rc; }
    rc = receive_ack(w, sequence, 65535U + sequence, crc, sequence, &budget);
    if (rc == 0) {
        ++w->rows_acked;
        w->block_crc_state = crc_update(w->block_crc_state, raw, 4352U);
    }
    return rc;
}
static bool clean_end(const struct nand_backup_wire *w, const uint32_t *f)
{
    return f[0] == 0U && f[1] == 1U && f[2] == 0U && f[3] == 1U && f[4] == 0U && f[5] == 1U &&
        f[6] == 64U && f[7] == 64U && f[8] == 64U && f[9] == 278528U &&
        w->rows_sent == 64U && w->rows_acked == 64U &&
        f[11] >= 64U && f[11] <= 512U && f[12] >= 64U && f[12] <= 512U &&
        f[10] == 463U + f[11] + f[12] &&
        f[13] == 1U && f[14] == 1U && f[15] == 1U && f[16] == 1U && f[17] == 1U &&
        f[18] == 16U && f[19] == 1U && f[20] == 124U && f[21] == 1U && f[22] == 124U &&
        f[23] == 1U && f[24] <= 255U && (f[24] & 0x8fU) == 0U && f[25] == 1U && f[26] == 0U &&
        f[27] == 1U && f[28] == 16U && f[29] == 0U && f[30] == 1U &&
        f[31] == (w->block_crc_state ^ UINT32_MAX) && f[32] == 1U && f[33] == 0U && f[34] == 0U &&
        f[35] == 1U && f[36] == 1U && f[37] == 1U && f[38] == 1U && f[39] == 1U && f[40] == 1U &&
        f[42] == 0U && f[43] == 1U && !w->data_stopped;
}
int nand_backup_wire_finish(struct nand_backup_wire *w, const uint32_t fields[44], bool clean_nand)
{
    if (w == NULL || !w->initialized) { return NAND_BACKUP_WIRE_ARGUMENT; }
    if (!w->hello_received || !w->transport_usable || !w->output_frame_boundary || w->end_attempted) {
        return fail(w, NAND_BACKUP_WIRE_STATE, false);
    }
    w->end_attempted = true;
    if (fields == NULL || (fields[0] != 0U && fields[0] <= INT32_MAX) ||
        (fields[2] != 0U && fields[2] <= INT32_MAX) || (fields[4] != 0U && fields[4] <= INT32_MAX) ||
        (fields[42] != 0U && fields[42] <= INT32_MAX) ||
        fields[1] > 15U || fields[3] > 15U || fields[5] > 3U || fields[6] > 64U ||
        fields[7] != w->rows_sent || fields[8] != w->rows_acked || fields[9] != (uint32_t)w->rows_acked * 4352U ||
        fields[10] > 1487U || fields[11] > 512U || fields[12] > 512U || fields[43] != 1U ||
        (clean_nand ? !clean_end(w, fields) : fields[0] == 0U || fields[1] == 1U)) {
        return fail(w, NAND_BACKUP_WIRE_ARGUMENT, true);
    }
    struct budget budget = new_budget(w, false); /* Independent after NAND restore. */
    serialize(w, fields, 44U);
    const struct span body = {w->tx_metadata, 176U};
    uint32_t crc = frame_header(w, END, 65U, &body, 1U, 176U);
    const struct span spans[2] = {{w->tx_header, 48U}, body};
    int rc = write_spans(w, spans, 2U, &budget);
    if (w->output_frame_boundary) { w->end_sent = true; }
    if (rc != 0) { return rc; }
    if (!clean_nand) { w->data_stopped = true; return 0; }
    rc = receive_ack(w, 65U, 65536U, crc, 64U, &budget);
    if (rc == 0) {
        w->clean_end_acknowledged = true;
        w->console_reuse_allowed = true;
        w->quarantined = false;
    }
    return rc;
}
