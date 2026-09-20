#include "nwr_wire.h"
#include "nand_write_rate.h"
#include <string.h>

enum { HELLO = 1, PROGRESS = 2, END = 3, ACK = 4, ABORT = 5 };
#if defined(__GNUC__)
#define WIRE_AUDIT_RETAIN __attribute__((used, retain))
#else
#define WIRE_AUDIT_RETAIN
#endif
static const uint8_t nwr_wire_magic[8] WIRE_AUDIT_RETAIN = {'O','P','N','D','W','R','1',0};
static const uint8_t nwr_wire_ready[] WIRE_AUDIT_RETAIN =
    "NAND_WRITE_RATE_READY protocol=1 block=1024 rows=65539,65540 erase_max=0\r\n";
/* Retained release-audit pins: protocol/header/nonce/body/events/prefix/
 * attempts/frame/data/no-progress bounds. No payload or target parameters. */
static const uint32_t nwr_wire_bounds[10] WIRE_AUDIT_RETAIN = {
 1U,48U,16U,384U,32U,64U,8192U,2000U,120000U,250U
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
uint32_t nwr_wire_crc32(const uint8_t *bytes, size_t length)
{
    if (bytes == NULL && length != 0U) { return 0; }
    return crc_update(UINT32_MAX, bytes, length) ^ UINT32_MAX;
}
static bool before(uint32_t now, uint32_t deadline)
{
    uint32_t remaining = deadline - now;
    return remaining != 0U && remaining <= INT32_MAX;
}
static int fail(struct nwr_wire *w, int rc, bool usable)
{
    w->last_rc = rc;
    w->data_stopped = true;
    w->quarantined = true;
    w->console_reuse_allowed = false;
    if (!usable) { w->transport_usable = false; }
    return rc;
}
static struct budget new_budget(struct nwr_wire *w, bool data)
{
    uint32_t now = w->io.now_ms(w->io.user);
    struct budget b = {now + NWR_WIRE_FRAME_MS, 0, data};
    if (data && before(now, w->data_deadline_ms) &&
        w->data_deadline_ms - now < NWR_WIRE_FRAME_MS) { b.deadline = w->data_deadline_ms; }
    return b;
}
static int deadline_check(struct nwr_wire *w, const struct budget *b)
{
    uint32_t now = w->io.now_ms(w->io.user);
    if (b->data && !before(now, w->data_deadline_ms)) {
        return fail(w, NWR_WIRE_GLOBAL_TIMEOUT, false);
    }
    if (!before(now, b->deadline)) {
        return fail(w, NWR_WIRE_TIMEOUT, false);
    }
    return 0;
}
static int permit(struct nwr_wire *w, struct budget *b)
{
    int rc = deadline_check(w, b);
    if (rc != 0) { return rc; }
    if (b->attempts >= NWR_WIRE_ATTEMPTS) {
        return fail(w, NWR_WIRE_TIMEOUT, false);
    }
    ++b->attempts;
    return 0;
}
static int no_pending(struct nwr_wire *w)
{
    bool pending = true;
    if (w->io.pending_rx(w->io.user, &pending) != 0) { return fail(w, NWR_WIRE_IO, false); }
    if (pending) { return fail(w, NWR_WIRE_PROTOCOL, false); }
    return 0;
}
static int write_spans(struct nwr_wire *w, const struct span *spans, size_t count, struct budget *b)
{
    w->output_frame_boundary = false;
    for (size_t part = 0; part < count; ++part) {
        size_t offset = 0;
        while (offset < spans[part].length) {
            int rc = permit(w, b);
            if (rc != 0) { return rc; }
            size_t requested = spans[part].length - offset, accepted = 0;
            if (requested > NWR_WIRE_IO_MAX) { requested = NWR_WIRE_IO_MAX; }
            rc = w->io.write(w->io.user, spans[part].bytes + offset, requested, &accepted);
            if (accepted > requested) { return fail(w, NWR_WIRE_IO, false); }
            offset += accepted;
            if (part + 1U == count && offset == spans[part].length) { w->output_frame_boundary = true; }
            if (rc != 0) { return fail(w, NWR_WIRE_IO, false); }
            rc = deadline_check(w, b);
            if (rc != 0) { return rc; }
            if (accepted == 0U) { w->io.yield_us(w->io.user, 250U); }
        }
    }
    return 0;
}
static int read_exact(struct nwr_wire *w, uint8_t *bytes, size_t length, struct budget *b)
{
    size_t offset = 0;
    while (offset < length) {
        int rc = permit(w, b);
        if (rc != 0) { return rc; }
        size_t requested = length - offset, received = 0;
        if (requested > NWR_WIRE_IO_MAX) { requested = NWR_WIRE_IO_MAX; }
        rc = w->io.read(w->io.user, bytes + offset, requested, &received);
        if (received > requested || rc != 0) { return fail(w, NWR_WIRE_IO, false); }
        offset += received;
        rc = deadline_check(w, b);
        if (rc != 0) { return rc; }
        if (received == 0U) { w->io.yield_us(w->io.user, 250U); }
    }
    return 0;
}
static bool header_common(const uint8_t *p)
{
    return memcmp(p, nwr_wire_magic, sizeof(nwr_wire_magic)) == 0 && get16(p + 8) == 1U && get16(p + 12) == 48U &&
           get16(p + 14) == 0U && get32(p + 44) == nwr_wire_crc32(p, 44U);
}
static void serialize(struct nwr_wire *w, const uint32_t *fields, size_t count)
{
    for (size_t i = 0; i < count; ++i) { put32(w->tx_metadata + 4U * i, fields[i]); }
}
static uint32_t frame_header(struct nwr_wire *w, uint16_t kind, uint16_t seq,
                             const struct span *bodies, size_t count, uint16_t body_length)
{
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < count; ++i) { crc = crc_update(crc, bodies[i].bytes, bodies[i].length); }
    crc ^= UINT32_MAX;
    memcpy(w->tx_header, nwr_wire_magic, 8U); put16(w->tx_header + 8, 1U);
    put16(w->tx_header + 10, kind); put16(w->tx_header + 12, 48U); put16(w->tx_header + 14, 0U);
    memcpy(w->tx_header + 16, w->nonce, 16U);
    put16(w->tx_header + 32, seq); put16(w->tx_header + 34, body_length);
    put32(w->tx_header + 36, 65536U);
    put32(w->tx_header + 40, crc); put32(w->tx_header + 44, nwr_wire_crc32(w->tx_header, 44U));
    return crc;
}
static int receive_ack(struct nwr_wire *w, uint16_t sequence, uint32_t row,
                       uint32_t body_crc, uint32_t pages, struct budget *budget)
{
    int rc = read_exact(w, w->rx_control, 48U, budget);
    if (rc != 0) { return rc; }
    uint16_t kind = get16(w->rx_control + 10), size = get16(w->rx_control + 34);
    if (!header_common(w->rx_control) || memcmp(w->rx_control + 16, w->nonce, 16U) != 0 ||
        get16(w->rx_control + 32) != sequence || get32(w->rx_control + 36) != row ||
        !((kind == ACK && size == 8U) || (kind == ABORT && size == 4U))) {
        return fail(w, NWR_WIRE_PROTOCOL, false);
    }
    rc = read_exact(w, w->rx_control + 48, size, budget);
    if (rc != 0) { return rc; }
    if (get32(w->rx_control + 40) != nwr_wire_crc32(w->rx_control + 48, size)) {
        return fail(w, NWR_WIRE_PROTOCOL, false);
    }
    rc = no_pending(w);
    if (rc != 0) { return rc; }
    rc = deadline_check(w, budget);
    if (rc != 0) { return rc; }
    if (kind == ABORT) {
        uint32_t reason = get32(w->rx_control + 48);
        if (reason < 1U || reason > 4U) { return fail(w, NWR_WIRE_PROTOCOL, false); }
        w->host_abort_reason = (uint8_t)reason;
        return fail(w, NWR_WIRE_HOST_ABORT, true);
    }
    if (get32(w->rx_control + 48) != body_crc || get32(w->rx_control + 52) != pages) {
        return fail(w, NWR_WIRE_PROTOCOL, false);
    }
    return 0;
}
int nwr_wire_init(struct nwr_wire *w, const struct nwr_wire_io *io)
{
    if (w == NULL || io == NULL || io->write == NULL || io->read == NULL || io->now_ms == NULL ||
        io->yield_us == NULL || io->pending_rx == NULL) { return NWR_WIRE_ARGUMENT; }
    memset(w, 0, sizeof(*w)); w->io = *io;
    w->initialized = w->output_frame_boundary = w->transport_usable = w->quarantined = true;
    return 0;
}
int nwr_wire_handshake(struct nwr_wire *w)
{
    if (w == NULL || !w->initialized) { return NWR_WIRE_ARGUMENT; }
    if (w->handshake_started) { return fail(w, NWR_WIRE_STATE, false); }
    w->handshake_started = true;
    struct budget b = new_budget(w, false);
    const struct span line = {nwr_wire_ready, sizeof(nwr_wire_ready) - 1U};
    int rc = write_spans(w, &line, 1U, &b);
    if (rc != 0) { return rc; }
    rc = read_exact(w, w->rx_control, 48U, &b);
    if (rc != 0) { return rc; }
    uint8_t nonzero = 0;
    for (unsigned int i = 16; i < 32U; ++i) { nonzero |= w->rx_control[i]; }
    if (!header_common(w->rx_control) || get16(w->rx_control + 10) != HELLO ||
        get16(w->rx_control + 32) != UINT16_MAX || get16(w->rx_control + 34) != 0U ||
        get32(w->rx_control + 36) != UINT32_MAX || get32(w->rx_control + 40) != 0U || !nonzero) {
        return fail(w, NWR_WIRE_PROTOCOL, false);
    }
    rc = no_pending(w);
    if (rc != 0) { return rc; }
    rc = deadline_check(w, &b);
    if (rc != 0) { return rc; }
    memcpy(w->nonce, w->rx_control + 16, 16U); w->hello_received = true;
    return 0;
}
int nwr_wire_arm_data_deadline(struct nwr_wire *w, uint32_t absolute_ms)
{
    if (w == NULL || !w->initialized) { return NWR_WIRE_ARGUMENT; }
    if (!w->hello_received || w->deadline_armed || w->data_stopped || w->end_attempted) {
        return fail(w, NWR_WIRE_STATE, false);
    }
    uint32_t now = w->io.now_ms(w->io.user);
    if (!before(now, absolute_ms) || absolute_ms - now > NWR_WIRE_DATA_MS) {
        return fail(w, NWR_WIRE_GLOBAL_TIMEOUT, true);
    }
    w->data_deadline_ms = absolute_ms; w->deadline_armed = true;
    return 0;
}
static bool data_admitted(const struct nwr_wire *w)
{
    return w->hello_received && w->deadline_armed && w->transport_usable && w->output_frame_boundary &&
           !w->data_stopped && !w->end_attempted;
}

/* Finite metadata fields only. No raw array bytes or unpinned hashes exist in
 * this API. Semantic phase/operation validation remains the driver's duty;
 * the host independently validates its closed state machine before ACK. */
static bool metadata_valid(const uint32_t *f)
{
    static const uint8_t bits[]={NW_BINDING_VALID,NW_WHOLE_VALID,NW_ARRAY_MAY_CHANGE,
        NW_A0_DIRTY,NW_B0_DIRTY,NW_A0_VALID,NW_B0_VALID,NW_C0_VALID,NW_READY_UNKNOWN,
        NW_RESTORED,NW_STOPPED,NW_BUS_RELEASED,NW_RAM_SCRUBBED,NW_FAULT,
        NW_ATTEMPTED,NW_QUARANTINED,NW_DEFAULT_VALID,NW_LAST_STARTED};
    static const uint8_t errors[]={NW_RC,NW_PRIMARY_RC,NW_RESTORE_RC,NW_OBSERVER_RC,NW_HASH_RC};
    if(!f) return false;
    for(size_t i=0;i<sizeof(bits);++i) if(f[bits[i]]>1) return false;
    for(size_t i=0;i<sizeof(errors);++i) if(f[errors[i]] && f[errors[i]]<0xfffff001U) return false;
    if(f[NW_OUTCOME]>NWR_CLEANUP_ERROR || f[NW_PRIMARY_OUTCOME]>NWR_CLEANUP_ERROR ||
       f[NW_PHASE]>NWR_DONE || f[NW_EVENT]>NE_PRESERVED || f[NW_STARTS]>NWR_MAX_STARTS ||
       (f[NW_ROW_INDEX]>=64 && f[NW_ROW_INDEX]!=UINT32_MAX) ||
       f[NW_PREIMAGE_ROWS]>64 || f[NW_BLANK_TAIL]>61 ||
       f[NW_OLD_PRE_VALID]>7 || f[NW_OLD_POST_VALID]>7 ||
       (f[NW_MARKER_PRE]!=0 && f[NW_MARKER_PRE]!=0xffff) ||
       (f[NW_MARKER_POST]!=0 && f[NW_MARKER_POST]!=0xffff) ||
       f[NW_WREN]>2 || f[NW_WRDI]>1 || f[NW_LOADS]>2 || f[NW_EXECUTES]>2 ||
       f[NW_COMPLETED]>2 || f[NW_A0]>255 || f[NW_B0]>255 || f[NW_C0]>255 ||
       f[NW_EVENT_COUNT]>32 ||
       f[NW_RATE_SETS]>11 || f[NW_RATE_RESTORES]>11 ||
       (f[NW_LAST_OPERATION]>=17 && f[NW_LAST_OPERATION]!=UINT32_MAX) ||
       f[NW_LAST_TX]>4356 || f[NW_LAST_RX]>4356 || f[NW_VERIFIED_MASK]>3 ||
       f[NW_RESERVED0] || f[NW_RESERVED1] || f[NW_QUARANTINED]!=1) return false;
    for(unsigned s=0;s<2;++s) {
        const uint32_t *v=f+(s?NW_SAMPLE1:NW_SAMPLE0);
        if(v[NS_ROW]!=65539+s || v[NS_RATE_HZ]!=(s?2000000U:125000U) ||
           (v[NS_RATE_REGISTER] && v[NS_RATE_REGISTER]!=(s?0x20000000U:0x02000000U)) ||
           v[NS_LOAD_STARTED]>1 || v[NS_LOAD_VALID]>1 || v[NS_LOAD_TX]>4099 ||
           v[NS_LOAD_RX]>4099 || v[NS_EXEC_STARTED]>1 || v[NS_EXEC_VALID]>1 ||
           v[NS_PROGRAM_POLLS]>32 || v[NS_READ0_VALID]>1 || v[NS_READ1_VALID]>1 ||
           v[NS_SHA_VALID]>1 || v[NS_RESERVED]) return false;
    }
    return true;
}
bool nwr_wire_clean_result(const uint32_t f[96])
{
    if(!metadata_valid(f) || f[NW_RC] || f[NW_OUTCOME]!=NWR_VERIFIED ||
       f[NW_PRIMARY_RC] || f[NW_PRIMARY_OUTCOME] || f[NW_PHASE]!=NWR_DONE ||
       f[NW_PREIMAGE_ROWS]!=64 || f[NW_BLANK_TAIL]!=61 || f[NW_ROW_INDEX]!=0 ||
       f[NW_OLD_PRE_VALID]!=7 || f[NW_OLD_POST_VALID]!=7 ||
       f[NW_MARKER_PRE]!=0xffff || f[NW_MARKER_POST]!=0xffff ||
       !f[NW_BINDING_VALID] || !f[NW_WHOLE_VALID] ||
       f[NW_WREN]!=2 || f[NW_LOADS]!=2 || f[NW_EXECUTES]!=2 ||
       f[NW_COMPLETED]!=2 || !f[NW_ARRAY_MAY_CHANGE] ||
       f[NW_A0_DIRTY] || f[NW_B0_DIRTY] || !f[NW_A0_VALID] || f[NW_A0]!=0x7c ||
       !f[NW_B0_VALID] || f[NW_B0]!=0x10 || !f[NW_C0_VALID] || (f[NW_C0]&0x8fU) ||
       f[NW_READY_UNKNOWN] || f[NW_RESTORE_RC] || !f[NW_RESTORED] || f[NW_FAULT] ||
       !f[NW_STOPPED] || !f[NW_BUS_RELEASED] || !f[NW_RAM_SCRUBBED] ||
       !f[NW_ATTEMPTED] || f[NW_OBSERVER_RC] || f[NW_HASH_RC] ||
       !f[NW_DEFAULT_VALID] || f[NW_RATE_REGISTER]!=0x02000000U ||
       f[NW_RATE_SETS]!=11 || f[NW_RATE_RESTORES]!=11 || !f[NW_CYCLE_HZ] ||
       f[NW_EVENT_COUNT]!=19 || f[NW_EVENT]!=NE_PRESERVED || f[NW_VERIFIED_MASK]!=3 ||
       f[NW_ELAPSED_MS]>120100 || f[NW_ACK_MS]>120000 ||
       f[NW_STARTS]<670U+2U*f[NW_WRDI] ||
       f[NW_LAST_OPERATION]!=1 || !f[NW_LAST_STARTED] ||
       f[NW_LAST_TX]!=3 || f[NW_LAST_RX]!=3) return false;
    for(unsigned s=0;s<2;++s) {
        const uint32_t *v=f+(s?NW_SAMPLE1:NW_SAMPLE0);
        if(!v[NS_LOAD_STARTED] || !v[NS_LOAD_VALID] || v[NS_LOAD_TX]!=4099 ||
           v[NS_LOAD_RX]!=4099 || !v[NS_EXEC_STARTED] || !v[NS_EXEC_VALID] ||
           !v[NS_PROGRAM_POLLS] || !v[NS_READ0_VALID] || !v[NS_READ1_VALID] ||
           !v[NS_SHA_VALID] || v[NS_CRC32]!=(s?841828657U:3609480481U) ||
           v[NS_RATE_REGISTER]!=(s?0x20000000U:0x02000000U)) return false;
    }
    return true;
}
static bool progress_valid(const uint32_t *f)
{
    return metadata_valid(f) && !f[NW_RC] && !f[NW_OUTCOME] &&
        !f[NW_PRIMARY_RC] && !f[NW_PRIMARY_OUTCOME] && !f[NW_OBSERVER_RC] &&
        f[NW_STOPPED] && !f[NW_READY_UNKNOWN] && !f[NW_FAULT] &&
        f[NW_BINDING_VALID] && f[NW_ATTEMPTED] && f[NW_DEFAULT_VALID] &&
        f[NW_PHASE]>=NWR_PREFLIGHT && f[NW_PHASE]<=NWR_MARKER &&
        !f[NW_BUS_RELEASED] && f[NW_EVENT_COUNT]>=1 && f[NW_ELAPSED_MS]<120000 &&
        f[NW_ACK_MS]<120000;
}
int nwr_wire_check(struct nwr_wire *w)
{
    if(w==NULL || !w->initialized) return NWR_WIRE_ARGUMENT;
    if(!data_admitted(w)) return fail(w,NWR_WIRE_STATE,false);
    struct budget b=new_budget(w,true);
    int rc=deadline_check(w,&b);
    if(rc==0) rc=no_pending(w);
    if(rc==0) rc=deadline_check(w,&b);
    return rc;
}
int nwr_wire_progress(struct nwr_wire *w,const uint32_t fields[96])
{
    if(w==NULL || !w->initialized) return NWR_WIRE_ARGUMENT;
    if(!data_admitted(w) || w->events_sent!=w->events_acked ||
       w->events_sent>=NWR_WIRE_EVENTS)
        return fail(w,NWR_WIRE_STATE,false);
    if(!progress_valid(fields) || fields[NW_EVENT_COUNT]!=(uint32_t)w->events_sent+1U)
        return fail(w,NWR_WIRE_ARGUMENT,true);
    struct budget budget=new_budget(w,true);
    int rc=deadline_check(w,&budget);
    if(rc==0) rc=no_pending(w);
    if(rc==0) rc=deadline_check(w,&budget);
    if(rc!=0) return rc;
    serialize(w,fields,NWR_WIRE_WORDS);
    uint16_t sequence=(uint16_t)(w->events_sent+1U);
    const struct span body={w->tx_metadata,NWR_WIRE_BODY};
    uint32_t crc=frame_header(w,PROGRESS,sequence,&body,1U,NWR_WIRE_BODY);
    const struct span spans[2]={{w->tx_header,48U},body};
    rc=write_spans(w,spans,2U,&budget);
    if(w->output_frame_boundary) ++w->events_sent;
    if(rc!=0) return rc;
    rc=receive_ack(w,sequence,65536U,crc,sequence,&budget);
    if(rc==0) ++w->events_acked;
    return rc;
}
int nwr_wire_finish(struct nwr_wire *w,const uint32_t fields[96],bool clean)
{
    if(w==NULL || !w->initialized) return NWR_WIRE_ARGUMENT;
    if(!w->hello_received || !w->transport_usable || !w->output_frame_boundary || w->end_attempted)
        return fail(w,NWR_WIRE_STATE,false);
    w->end_attempted=true;
    if(!metadata_valid(fields) || fields[NW_EVENT_COUNT]<w->events_sent ||
       fields[NW_EVENT_COUNT]>(uint32_t)w->events_sent+1U ||
       (clean ? (!nwr_wire_clean_result(fields) || w->data_stopped ||
                 w->events_sent!=w->events_acked || fields[NW_EVENT_COUNT]!=w->events_acked) :
                (fields[NW_RC]==0U || fields[NW_OUTCOME]==NWR_VERIFIED)))
        return fail(w,NWR_WIRE_ARGUMENT,true);
    /* NAND restoration owns its separate budget. This final frame never
     * renews the expired data allowance or grants another NAND operation. */
    struct budget budget=new_budget(w,false);
    int rc=deadline_check(w,&budget);
    if(rc==0) rc=no_pending(w);
    if(rc==0) rc=deadline_check(w,&budget);
    if(rc!=0) return rc;
    serialize(w,fields,NWR_WIRE_WORDS);
    uint16_t sequence=(uint16_t)(w->events_sent+1U);
    const struct span body={w->tx_metadata,NWR_WIRE_BODY};
    uint32_t crc=frame_header(w,END,sequence,&body,1U,NWR_WIRE_BODY);
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
