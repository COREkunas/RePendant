#ifndef OPENPENDANT_NAND_BACKUP_WIRE_H
#define OPENPENDANT_NAND_BACKUP_WIRE_H

/* Pure-C fixed block1024 transport. No heap, OS, shell, driver or payload log.
 * Caller owns this context and stable stopped-DMA raw page buffers. Callbacks
 * must be nonblocking and serialize the sole raw USB producer/consumer.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NAND_BACKUP_WIRE_HEADER 48U
#define NAND_BACKUP_WIRE_RAW 4352U
#define NAND_BACKUP_WIRE_BEGIN_WORDS 16U
#define NAND_BACKUP_WIRE_PAGE_WORDS 16U
#define NAND_BACKUP_WIRE_END_WORDS 44U
#define NAND_BACKUP_WIRE_IO_MAX 64U
#define NAND_BACKUP_WIRE_ATTEMPTS 8192U
#define NAND_BACKUP_WIRE_FRAME_MS 2000U
#define NAND_BACKUP_WIRE_DATA_MS 120000U

enum nand_backup_wire_rc {
    NAND_BACKUP_WIRE_OK = 0, NAND_BACKUP_WIRE_ARGUMENT = -1,
    NAND_BACKUP_WIRE_IO = -2, NAND_BACKUP_WIRE_TIMEOUT = -3,
    NAND_BACKUP_WIRE_PROTOCOL = -4, NAND_BACKUP_WIRE_HOST_ABORT = -5,
    NAND_BACKUP_WIRE_STATE = -6, NAND_BACKUP_WIRE_GLOBAL_TIMEOUT = -7
};

struct nand_backup_wire_io {
    /* Return0 on transport acceptance, nonzero on error. accepted/received
     * must always be set and <=requested, including on error. write() copies
     * the accepted PREFIX synchronously and retains no caller pointer.
     */
    int (*write)(void *user, const uint8_t *bytes, size_t requested, size_t *accepted);
    int (*read)(void *user, uint8_t *bytes, size_t requested, size_t *received);
    uint32_t (*now_ms)(void *user);
    void (*yield_us)(void *user, uint32_t microseconds);
    int (*pending_rx)(void *user, bool *pending);
    void *user;
};

struct nand_backup_wire {
    struct nand_backup_wire_io io;
    uint8_t nonce[16], tx_header[48], tx_metadata[176], rx_control[56];
    uint32_t data_deadline_ms, block_crc_state;
    uint16_t rows_sent, rows_acked;
    int last_rc;
    uint8_t host_abort_reason;
    bool initialized, handshake_started, hello_received, deadline_armed, begin_sent, begin_acked;
    bool output_frame_boundary, transport_usable, data_stopped, end_attempted, end_sent;
    bool clean_end_acknowledged, console_reuse_allowed, quarantined;
};

/* CRC is public metadata only; this function never logs or retains its input. */
uint32_t nand_backup_wire_crc32(const uint8_t *bytes, size_t length);
int nand_backup_wire_init(struct nand_backup_wire *wire, const struct nand_backup_wire_io *io);
/* Raw READY line only: the existing shell owns the preceding command echo.
 * Installs no bypass itself; caller must install terminal discard BEFORE this.
 * Exact HELLO only, no NAND activity. One2s deadline covers READY and HELLO.
 */
int nand_backup_wire_handshake(struct nand_backup_wire *wire);
/* Call once at first NAND START; absolute now_ms()+120000, no renewal.
 * uint32 millisecond wrap is accepted within the <=120000ms horizon.
 */
int nand_backup_wire_arm_data_deadline(struct nand_backup_wire *wire, uint32_t absolute_ms);
/* Fixed ordered u32 metadata from the reviewed protocol; explicitly serialized
 * LE. PAGE raw memory must stay immutable until return; no stack/payload copy.
 * Each frame enqueue + exact durable ACK shares one2s/global-limited budget.
 */
int nand_backup_wire_send_begin(struct nand_backup_wire *wire, const uint32_t fields[16]);
int nand_backup_wire_send_page(struct nand_backup_wire *wire, const uint32_t fields[16], const uint8_t raw[4352]);
/* END quarantine field must be1. Failed END is sent without waiting for an ACK.
 * A clean END additionally requires matching complete counters/CRC/NAND fields,
 * exact final ACK, and no pending RX before console_reuse_allowed becomes true.
 * No bypass removal or ownership release occurs here: caller decides and must
 * recheck pending input atomically with any actual shell transition.
 */
int nand_backup_wire_finish(struct nand_backup_wire *wire, const uint32_t fields[44], bool clean_nand);

#endif
