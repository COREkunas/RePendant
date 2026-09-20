#ifndef OPENPENDANT_OPUS_PACKET_DECODE_H
#define OPENPENDANT_OPUS_PACKET_DECODE_H
#include <stddef.h>
#include <stdint.h>
#define OPD_MAX_PACKETS 501u
#define OPD_PACKET_BYTES 60u
#define OPD_FRAME_SAMPLES 320u
/* Fresh fixed16k/mono decoder for each call, no PLC/FEC, no retained pointers.
 * Exact1..501 packets and exact output capacity. Invalid spans/alias => no write;
 * once admitted all output is zeroed on failure. Output on success is untrimmed;
 * authenticated segment parser/assembler owns pre-skip/end-trim. No auth claim.
 * Stack and decoder heap lifetime are bounded; volatile wiping is best effort
 * and does not prove erasure of compiler registers or the complete C stack.
 */
int opd_decode_packets(const uint8_t*,size_t,int16_t*,size_t);
void opd_wipe(void*,size_t);
#endif
