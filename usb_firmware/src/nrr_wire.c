#include "nrr_wire.h"
#include "nand_read_rate.h"
#include <string.h>

enum { HELLO = 1, PROGRESS = 2, END = 3, ACK = 4, ABORT = 5 };
#if defined(__GNUC__)
#define WIRE_AUDIT_RETAIN __attribute__((used, retain))
#else
#define WIRE_AUDIT_RETAIN
#endif
static const uint8_t nrr_wire_magic[8] WIRE_AUDIT_RETAIN = {'O','P','N','D','R','R','1',0};
static const uint8_t nrr_wire_ready[] WIRE_AUDIT_RETAIN =
    "NAND_READ_RATE_READY protocol=1 block=1024 runs=13 write_max=0\r\n";
/* Retained release-audit pins: protocol/header/nonce/body/events/prefix/
 * attempts/frame/data/no-progress bounds. No payload or target parameters. */
static const uint32_t nrr_wire_bounds[10] WIRE_AUDIT_RETAIN = {
 1U,48U,16U,288U,13U,64U,8192U,2000U,20000U,250U
};
static const uint32_t nrr_wire_rates[4][2] WIRE_AUDIT_RETAIN = {
 {125000U,0x02000000U},{500000U,0x08000000U},
 {1000000U,0x10000000U},{2000000U,0x20000000U}
};
static const uint8_t nrr_wire_schedule[13] WIRE_AUDIT_RETAIN =
 {0,0,0,1,1,1,2,2,2,3,3,3,0};
_Static_assert(NRR_WORDS==NRR_WIRE_WORDS && NR_EVENT_COUNT==71 &&
 NRR_RUNS==NRR_WIRE_EVENTS && NRR_DATA_MS==NRR_WIRE_DATA_MS,"Read-rate schema differs");
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
uint32_t nrr_wire_crc32(const uint8_t *bytes, size_t length)
{
    if (bytes == NULL && length != 0U) { return 0; }
    return crc_update(UINT32_MAX, bytes, length) ^ UINT32_MAX;
}
static bool before(uint32_t now, uint32_t deadline)
{
    uint32_t remaining = deadline - now;
    return remaining != 0U && remaining <= INT32_MAX;
}
static int fail(struct nrr_wire *w, int rc, bool usable)
{
    w->last_rc = rc;
    w->data_stopped = true;
    w->quarantined = true;
    w->console_reuse_allowed = false;
    if (!usable) { w->transport_usable = false; }
    return rc;
}
static struct budget new_budget(struct nrr_wire *w, bool data)
{
    uint32_t now = w->io.now_ms(w->io.user);
    struct budget b = {now + NRR_WIRE_FRAME_MS, 0, data};
    if (data && before(now, w->data_deadline_ms) &&
        w->data_deadline_ms - now < NRR_WIRE_FRAME_MS) { b.deadline = w->data_deadline_ms; }
    return b;
}
static int deadline_check(struct nrr_wire *w, const struct budget *b)
{
    uint32_t now = w->io.now_ms(w->io.user);
    if (b->data && !before(now, w->data_deadline_ms)) {
        return fail(w, NRR_WIRE_GLOBAL_TIMEOUT, false);
    }
    if (!before(now, b->deadline)) {
        return fail(w, NRR_WIRE_TIMEOUT, false);
    }
    return 0;
}
static int permit(struct nrr_wire *w, struct budget *b)
{
    int rc = deadline_check(w, b);
    if (rc != 0) { return rc; }
    if (b->attempts >= NRR_WIRE_ATTEMPTS) {
        return fail(w, NRR_WIRE_TIMEOUT, false);
    }
    ++b->attempts;
    return 0;
}
static int no_pending(struct nrr_wire *w)
{
    bool pending = true;
    if (w->io.pending_rx(w->io.user, &pending) != 0) { return fail(w, NRR_WIRE_IO, false); }
    if (pending) { return fail(w, NRR_WIRE_PROTOCOL, false); }
    return 0;
}
static int write_spans(struct nrr_wire *w, const struct span *spans, size_t count, struct budget *b)
{
    w->output_frame_boundary = false;
    for (size_t part = 0; part < count; ++part) {
        size_t offset = 0;
        while (offset < spans[part].length) {
            int rc = permit(w, b);
            if (rc != 0) { return rc; }
            size_t requested = spans[part].length - offset, accepted = 0;
            if (requested > NRR_WIRE_IO_MAX) { requested = NRR_WIRE_IO_MAX; }
            rc = w->io.write(w->io.user, spans[part].bytes + offset, requested, &accepted);
            if (accepted > requested) { return fail(w, NRR_WIRE_IO, false); }
            offset += accepted;
            if (part + 1U == count && offset == spans[part].length) { w->output_frame_boundary = true; }
            if (rc != 0) { return fail(w, NRR_WIRE_IO, false); }
            rc = deadline_check(w, b);
            if (rc != 0) { return rc; }
            if (accepted == 0U) { w->io.yield_us(w->io.user, 250U); }
        }
    }
    return 0;
}
static int read_exact(struct nrr_wire *w, uint8_t *bytes, size_t length, struct budget *b)
{
    size_t offset = 0;
    while (offset < length) {
        int rc = permit(w, b);
        if (rc != 0) { return rc; }
        size_t requested = length - offset, received = 0;
        if (requested > NRR_WIRE_IO_MAX) { requested = NRR_WIRE_IO_MAX; }
        rc = w->io.read(w->io.user, bytes + offset, requested, &received);
        if (received > requested || rc != 0) { return fail(w, NRR_WIRE_IO, false); }
        offset += received;
        rc = deadline_check(w, b);
        if (rc != 0) { return rc; }
        if (received == 0U) { w->io.yield_us(w->io.user, 250U); }
    }
    return 0;
}
static bool header_common(const uint8_t *p)
{
    return memcmp(p, nrr_wire_magic, sizeof(nrr_wire_magic)) == 0 && get16(p + 8) == 1U && get16(p + 12) == 48U &&
           get16(p + 14) == 0U && get32(p + 44) == nrr_wire_crc32(p, 44U);
}
static void serialize(struct nrr_wire *w, const uint32_t *fields, size_t count)
{
    for (size_t i = 0; i < count; ++i) { put32(w->tx_metadata + 4U * i, fields[i]); }
}
static uint32_t frame_header(struct nrr_wire *w, uint16_t kind, uint16_t seq,
                             const struct span *bodies, size_t count, uint16_t body_length)
{
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < count; ++i) { crc = crc_update(crc, bodies[i].bytes, bodies[i].length); }
    crc ^= UINT32_MAX;
    memcpy(w->tx_header, nrr_wire_magic, 8U); put16(w->tx_header + 8, 1U);
    put16(w->tx_header + 10, kind); put16(w->tx_header + 12, 48U); put16(w->tx_header + 14, 0U);
    memcpy(w->tx_header + 16, w->nonce, 16U);
    put16(w->tx_header + 32, seq); put16(w->tx_header + 34, body_length);
    put32(w->tx_header + 36, 65536U);
    put32(w->tx_header + 40, crc); put32(w->tx_header + 44, nrr_wire_crc32(w->tx_header, 44U));
    return crc;
}
static int receive_ack(struct nrr_wire *w, uint16_t sequence, uint32_t row,
                       uint32_t body_crc, uint32_t pages, struct budget *budget)
{
    int rc = read_exact(w, w->rx_control, 48U, budget);
    if (rc != 0) { return rc; }
    uint16_t kind = get16(w->rx_control + 10), size = get16(w->rx_control + 34);
    if (!header_common(w->rx_control) || memcmp(w->rx_control + 16, w->nonce, 16U) != 0 ||
        get16(w->rx_control + 32) != sequence || get32(w->rx_control + 36) != row ||
        !((kind == ACK && size == 8U) || (kind == ABORT && size == 4U))) {
        return fail(w, NRR_WIRE_PROTOCOL, false);
    }
    rc = read_exact(w, w->rx_control + 48, size, budget);
    if (rc != 0) { return rc; }
    if (get32(w->rx_control + 40) != nrr_wire_crc32(w->rx_control + 48, size)) {
        return fail(w, NRR_WIRE_PROTOCOL, false);
    }
    rc = no_pending(w);
    if (rc != 0) { return rc; }
    rc = deadline_check(w, budget);
    if (rc != 0) { return rc; }
    if (kind == ABORT) {
        uint32_t reason = get32(w->rx_control + 48);
        if (reason < 1U || reason > 4U) { return fail(w, NRR_WIRE_PROTOCOL, false); }
        w->host_abort_reason = (uint8_t)reason;
        return fail(w, NRR_WIRE_HOST_ABORT, true);
    }
    if (get32(w->rx_control + 48) != body_crc || get32(w->rx_control + 52) != pages) {
        return fail(w, NRR_WIRE_PROTOCOL, false);
    }
    return 0;
}
int nrr_wire_init(struct nrr_wire *w, const struct nrr_wire_io *io)
{
    if (w == NULL || io == NULL || io->write == NULL || io->read == NULL || io->now_ms == NULL ||
        io->yield_us == NULL || io->pending_rx == NULL) { return NRR_WIRE_ARGUMENT; }
    memset(w, 0, sizeof(*w)); w->io = *io;
    w->initialized = w->output_frame_boundary = w->transport_usable = w->quarantined = true;
    return 0;
}
int nrr_wire_handshake(struct nrr_wire *w)
{
    if (w == NULL || !w->initialized) { return NRR_WIRE_ARGUMENT; }
    if (w->handshake_started) { return fail(w, NRR_WIRE_STATE, false); }
    w->handshake_started = true;
    struct budget b = new_budget(w, false);
    const struct span line = {nrr_wire_ready, sizeof(nrr_wire_ready) - 1U};
    int rc = write_spans(w, &line, 1U, &b);
    if (rc != 0) { return rc; }
    rc = read_exact(w, w->rx_control, 48U, &b);
    if (rc != 0) { return rc; }
    uint8_t nonzero = 0;
    for (unsigned int i = 16; i < 32U; ++i) { nonzero |= w->rx_control[i]; }
    if (!header_common(w->rx_control) || get16(w->rx_control + 10) != HELLO ||
        get16(w->rx_control + 32) != UINT16_MAX || get16(w->rx_control + 34) != 0U ||
        get32(w->rx_control + 36) != UINT32_MAX || get32(w->rx_control + 40) != 0U || !nonzero) {
        return fail(w, NRR_WIRE_PROTOCOL, false);
    }
    rc = no_pending(w);
    if (rc != 0) { return rc; }
    rc = deadline_check(w, &b);
    if (rc != 0) { return rc; }
    memcpy(w->nonce, w->rx_control + 16, 16U); w->hello_received = true;
    return 0;
}
int nrr_wire_arm_data_deadline(struct nrr_wire *w, uint32_t absolute_ms)
{
    if (w == NULL || !w->initialized) { return NRR_WIRE_ARGUMENT; }
    if (!w->hello_received || w->deadline_armed || w->data_stopped || w->end_attempted) {
        return fail(w, NRR_WIRE_STATE, false);
    }
    uint32_t now = w->io.now_ms(w->io.user);
    if (!before(now, absolute_ms) || absolute_ms - now > NRR_WIRE_DATA_MS) {
        return fail(w, NRR_WIRE_GLOBAL_TIMEOUT, true);
    }
    w->data_deadline_ms = absolute_ms; w->deadline_armed = true;
    return 0;
}
static bool data_admitted(const struct nrr_wire *w)
{
    return w->hello_received && w->deadline_armed && w->transport_usable && w->output_frame_boundary &&
           !w->data_stopped && !w->end_attempted;
}

/* Closed metadata only; no page bytes, arbitrary hashes or address parameters.
 * Failed measurements retain actual uint32 elapsed/register/cycle observations,
 * never relabel them as valid timing. Host independently validates before ACK. */
static bool metadata_valid(const uint32_t *f)
{
    static const uint8_t bits[]={8,9,10,11,17,18,19,20,22,24,26,28,29,30,31,32,36,38,39,
        NR_RUN_DEFAULT_VALID,NR_ATTEMPTED,NR_FAULT,NR_STOPPED,NR_BUS_RELEASED,
        NR_RAM_SCRUBBED,NR_DEFAULT_VALID,NR_QUARANTINED};
    static const uint8_t errors[]={0,2,27,34,NR_RC,NR_OBSERVER_RC};
    static const uint8_t readonly_zero[]={NP_BLANK_ROWS,NP_PROGRAM_LOADS,NP_PROGRAM_EXECUTES,
        NP_PROGRAM_COMPLETED,NP_WREN_STARTS,NP_WRDI_STARTS,NP_ARRAY_MAY_CHANGE,
        NP_A0_DIRTY,NP_B0_DIRTY,NP_EVENT_COUNT,NP_WRITE_INTENT};
    if(!f) return false;
    for(size_t i=0;i<sizeof(bits);++i) if(f[bits[i]]>1) return false;
    for(size_t i=0;i<sizeof(errors);++i) if(f[errors[i]] && f[errors[i]]<0xfffff001U) return false;
    for(size_t i=0;i<sizeof(readonly_zero);++i) if(f[readonly_zero[i]]) return false;
    if(f[NR_OUTCOME]>NRR_CLEANUP_ERROR || f[NR_RUNS_STARTED]>13 ||
       f[NR_RUNS_VERIFIED]>f[NR_RUNS_STARTED] || f[NR_RUNS_ACKED]>f[NR_RUNS_VERIFIED] ||
       f[NR_RUNS_STARTED]-f[NR_RUNS_VERIFIED]>1 || f[NR_RUNS_VERIFIED]-f[NR_RUNS_ACKED]>1 ||
       f[NR_EVENT_COUNT]!=f[NR_RUNS_VERIFIED] || f[NR_STARTS]>NRR_MAX_STARTS ||
       f[NR_STARTS]>46U*f[NR_RUNS_STARTED] || f[NR_STARTS]<25U*f[NR_RUNS_VERIFIED] ||
       f[NR_QUARANTINED]!=1 || f[NR_CACHE_STARTS]>3 ||
       f[NR_CACHE_COMPLETED]>f[NR_CACHE_STARTS] || f[NR_TIMING_VALID]>7 ||
       f[NR_CACHE_STARTS]-f[NR_CACHE_COMPLETED]>1 ||
       f[NR_TIMING_VALID]!=(1U<<f[NR_CACHE_COMPLETED])-1U ||
       f[NR_RATE_SETS]>3 || f[NR_RATE_RESTORES]>f[NR_RATE_SETS] ||
       f[NR_DEFAULT_VALID]!=f[NR_RUN_DEFAULT_VALID]) return false;
    if(!f[NR_RUNS_STARTED]) {
        if(f[NR_RUN_INDEX]!=UINT32_MAX || f[NR_RATE_INDEX]!=UINT32_MAX) return false;
        for(size_t i=0;i<40;++i) if(f[i]) return false;
        for(size_t i=NR_RATE_HZ;i<=NR_RATE_RESTORES;++i) if(f[i]) return false;
        return !f[NR_STARTS];
    }
    if(!f[NR_ATTEMPTED] || f[NR_RUN_INDEX]!=f[NR_RUNS_STARTED]-1U ||
       f[NR_RATE_INDEX]!=nrr_wire_schedule[f[NR_RUN_INDEX]] ||
       f[NR_RATE_HZ]!=nrr_wire_rates[f[NR_RATE_INDEX]][0] || !f[NR_CYCLE_HZ] ||
       f[NP_MODE]!=NPO_RECOVER_ONLY || f[NP_OUTCOME]>NPO_CLEANUP_ERROR ||
       f[NP_PRIMARY_OUTCOME]>NPO_CLEANUP_ERROR || f[NP_PHASE]>NPO_DONE ||
       (f[NP_PHASE]>=NPO_TAIL && f[NP_PHASE]<=NPO_COMMIT_PROGRAM) ||
       f[NP_TRANSFERS]>46 || f[NP_TRANSFERS]>f[NR_STARTS] ||
       f[NP_A0]>255 || f[NP_B0]>255 || f[NP_C0]>255 || f[NP_BLOCK_QUARANTINED]!=1 ||
       !(f[NP_LAST_OPERATION]<=3 || f[NP_LAST_OPERATION]==8 || f[NP_LAST_OPERATION]==10)) return false;
    if(!f[NR_RATE_INDEX] && (f[NR_RATE_SETS] || f[NR_RATE_RESTORES])) return false;
    unsigned int count=0;
    for(unsigned int i=0;i<3;++i) {
        if(f[NR_TIMING_VALID]&(1U<<i)) { ++count; if(!f[NR_CACHE0_CYCLES+i]) return false; }
        else if(f[NR_CACHE0_CYCLES+i]) return false;
    }
    return count==f[NR_CACHE_COMPLETED];
}
static bool clean_run(const uint32_t *f)
{
    if(!metadata_valid(f) || f[NP_RC] || f[NP_OUTCOME]!=NPO_VERIFIED ||
       f[NP_PRIMARY_RC] || f[NP_PRIMARY_OUTCOME] || f[NP_PHASE]!=NPO_DONE ||
       !f[NP_PATTERN_VALID] || !f[NP_DATA_VALID] || !f[NP_COMMIT_VALID] || !f[NP_COMMITTED] ||
       !f[NP_A0_VALID] || f[NP_A0]!=0x7c ||
       !f[NP_B0_VALID] || f[NP_B0]!=0x10 || !f[NP_C0_VALID] || f[NP_C0] ||
       f[NP_READY_UNKNOWN] || f[NP_RESTORE_RC] || !f[NP_RESTORED] || f[NP_FAULT] ||
       !f[NP_STOPPED] || !f[NP_BUS_RELEASED] || !f[NP_RAM_SCRUBBED] || f[NP_OBSERVER_RC] ||
       f[NP_LAST_OPERATION]!=1 || !f[NP_LAST_STARTED] || f[NP_TRANSFERS]<25 ||
       !f[NR_ATTEMPTED] || f[NR_OBSERVER_RC] || f[NR_FAULT] || !f[NR_STOPPED] ||
       !f[NR_BUS_RELEASED] || !f[NR_RAM_SCRUBBED] || !f[NR_DEFAULT_VALID] ||
       !f[NR_RUN_DEFAULT_VALID] || f[NR_CACHE_STARTS]!=3 || f[NR_CACHE_COMPLETED]!=3 ||
       f[NR_TIMING_VALID]!=7 || f[NR_FINAL_REGISTER]!=0x02000000U ||
       f[NR_RATE_REGISTER]!=nrr_wire_rates[f[NR_RATE_INDEX]][1] ||
       f[NR_RATE_SETS]!=(f[NR_RATE_INDEX]?3U:0U) || f[NR_RATE_RESTORES]!=f[NR_RATE_SETS] ||
       f[NR_ELAPSED_MS]>NRR_WIRE_DATA_MS || f[NR_RUN_WALL_MS]>f[NR_ELAPSED_MS] ||
       f[NP_ELAPSED_MS]>f[NR_RUN_WALL_MS]) return false;
    /* Fixed target is below 1GHz. Three serial transfer intervals must fit the
     * enclosing millisecond measurement, allowing quantization by 1ms+3ticks.
     * Not an SCK/real-time claim; actual cycles include polling/service delay. */
    uint64_t cycles=(uint64_t)f[NR_CACHE0_CYCLES]+f[NR_CACHE1_CYCLES]+f[NR_CACHE2_CYCLES];
    return f[NR_CYCLE_HZ]<=1000000000U && cycles*1000U<=
        ((uint64_t)f[NR_RUN_WALL_MS]+1U)*f[NR_CYCLE_HZ]+3000U;
}
bool nrr_wire_clean_result(const uint32_t f[72])
{
    return clean_run(f) && !f[NR_RC] && f[NR_OUTCOME]==NRR_VERIFIED &&
        f[NR_RUN_INDEX]==12 && f[NR_RATE_INDEX]==0 && f[NR_RUNS_STARTED]==13 &&
        f[NR_RUNS_VERIFIED]==13 && f[NR_RUNS_ACKED]==13 && f[NR_EVENT_COUNT]==13 &&
        f[NR_STARTS]>=325;
}
static bool progress_valid(const uint32_t *f)
{
    return clean_run(f) && !f[NR_RC] && !f[NR_OUTCOME] &&
        f[NR_RUNS_STARTED]==f[NR_RUNS_VERIFIED] &&
        f[NR_RUNS_ACKED]+1U==f[NR_RUNS_VERIFIED];
}
int nrr_wire_check(struct nrr_wire *w)
{
    if(w==NULL || !w->initialized) return NRR_WIRE_ARGUMENT;
    if(!data_admitted(w)) return fail(w,NRR_WIRE_STATE,false);
    struct budget b=new_budget(w,true);
    int rc=deadline_check(w,&b);
    if(rc==0) rc=no_pending(w);
    if(rc==0) rc=deadline_check(w,&b);
    return rc;
}
int nrr_wire_progress(struct nrr_wire *w,const uint32_t fields[72],uint32_t absolute_event_deadline)
{
    if(w==NULL || !w->initialized) return NRR_WIRE_ARGUMENT;
    if(!data_admitted(w) || w->events_sent!=w->events_acked ||
       w->events_sent>=NRR_WIRE_EVENTS)
        return fail(w,NRR_WIRE_STATE,false);
    if(!progress_valid(fields) || fields[NR_EVENT_COUNT]!=(uint32_t)w->events_sent+1U ||
       fields[NR_STARTS]!=w->last_starts+fields[NP_TRANSFERS] ||
       fields[NR_ELAPSED_MS]<w->last_elapsed_ms ||
       (w->cycle_hz && fields[NR_CYCLE_HZ]!=w->cycle_hz))
        return fail(w,NRR_WIRE_ARGUMENT,true);
    struct budget budget=new_budget(w,true);
    uint32_t now=w->io.now_ms(w->io.user);
    if(!before(now,absolute_event_deadline) || absolute_event_deadline-now>NRR_WIRE_FRAME_MS ||
       !before(now,w->data_deadline_ms) || absolute_event_deadline-now>w->data_deadline_ms-now)
        return fail(w,NRR_WIRE_TIMEOUT,false);
    budget.deadline=absolute_event_deadline;
    int rc=deadline_check(w,&budget);
    if(rc==0) rc=no_pending(w);
    if(rc==0) rc=deadline_check(w,&budget);
    if(rc!=0) return rc;
    serialize(w,fields,NRR_WIRE_WORDS);
    uint16_t sequence=(uint16_t)(w->events_sent+1U);
    const struct span body={w->tx_metadata,NRR_WIRE_BODY};
    uint32_t crc=frame_header(w,PROGRESS,sequence,&body,1U,NRR_WIRE_BODY);
    const struct span spans[2]={{w->tx_header,48U},body};
    rc=write_spans(w,spans,2U,&budget);
    if(w->output_frame_boundary) {
        ++w->events_sent; memcpy(w->last_run,fields,sizeof(w->last_run));
        w->last_starts=fields[NR_STARTS]; w->last_elapsed_ms=fields[NR_ELAPSED_MS];
        w->cycle_hz=fields[NR_CYCLE_HZ];
    }
    if(rc!=0) return rc;
    rc=receive_ack(w,sequence,65536U,crc,sequence,&budget);
    if(rc==0) ++w->events_acked;
    return rc;
}
int nrr_wire_finish(struct nrr_wire *w,const uint32_t fields[72],bool clean)
{
    if(w==NULL || !w->initialized) return NRR_WIRE_ARGUMENT;
    if(!w->hello_received || !w->transport_usable || !w->output_frame_boundary || w->end_attempted)
        return fail(w,NRR_WIRE_STATE,false);
    w->end_attempted=true;
    if(!metadata_valid(fields) || fields[NR_EVENT_COUNT]<w->events_sent ||
       fields[NR_EVENT_COUNT]>(uint32_t)w->events_sent+1U ||
       fields[NR_RUNS_ACKED]>w->events_acked ||
       (uint32_t)w->events_acked-fields[NR_RUNS_ACKED]>1U ||
       fields[NR_STARTS]<w->last_starts || fields[NR_ELAPSED_MS]<w->last_elapsed_ms ||
       (clean ? (!nrr_wire_clean_result(fields) || w->data_stopped ||
                 !w->deadline_armed || w->events_sent!=13 || w->events_acked!=13 ||
                 fields[NR_STARTS]!=w->last_starts ||
                 memcmp(fields,w->last_run,sizeof(w->last_run))!=0) :
                (fields[NR_RC]==0U || fields[NR_OUTCOME]<NRR_REFUSED)))
        return fail(w,NRR_WIRE_ARGUMENT,true);
    /* NAND restoration owns its separate budget. This final frame never
     * renews the expired data allowance or grants another NAND operation. */
    struct budget budget=new_budget(w,false);
    int rc=deadline_check(w,&budget);
    if(rc==0) rc=no_pending(w);
    if(rc==0) rc=deadline_check(w,&budget);
    if(rc!=0) return rc;
    serialize(w,fields,NRR_WIRE_WORDS);
    uint16_t sequence=(uint16_t)(w->events_sent+1U);
    const struct span body={w->tx_metadata,NRR_WIRE_BODY};
    uint32_t crc=frame_header(w,END,sequence,&body,1U,NRR_WIRE_BODY);
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
