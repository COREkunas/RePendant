#include "opus_packet_decode.h"
#include "opus.h"
#include <stdlib.h>
#include <string.h>

void opd_wipe(void *pointer,size_t bytes) {
    volatile uint8_t *p=pointer; while(bytes--) *p++=0;
}
static int disjoint(const void *a,size_t an,const void *b,size_t bn) {
    uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;
    return x<=UINTPTR_MAX-an && y<=UINTPTR_MAX-bn && (x+an<=y || y+bn<=x);
}
int opd_decode_packets(const uint8_t *packets,size_t bytes,int16_t *out,size_t samples) {
    if(!packets || !out || (uintptr_t)out%_Alignof(int16_t) || bytes<60u || bytes>OPD_MAX_PACKETS*60u || bytes%60u ||
       samples!=(bytes/60u)*320u || !disjoint(packets,bytes,out,samples*sizeof(*out))) return -1;
    opd_wipe(out,samples*sizeof(*out));
    const char *version=opus_get_version_string();
    if(!version || strcmp(version,"libopus 1.6.1-fixed")) return -2;
    int state_bytes=opus_decoder_get_size(1);
    if(state_bytes<=0 || state_bytes>65536) return -2;
    OpusDecoder *decoder=(OpusDecoder*)calloc(1,(size_t)state_bytes);
    if(!decoder) return -3;
    int rc=-4;
    if(opus_decoder_init(decoder,16000,1)!=OPUS_OK) goto done;
    for(size_t i=0;i<bytes/60u;++i) {
        const uint8_t *packet=packets+i*60u;
        opus_int16 sizes[48];
        if(opus_packet_get_nb_channels(packet)!=1 || opus_packet_get_nb_samples(packet,60,16000)!=320 ||
           opus_packet_parse(packet,60,NULL,NULL,sizes,NULL)<=0) goto done;
        if(opus_decode(decoder,packet,60,out+i*320u,320,0)!=320) goto done;
    }
    rc=0;
done:
    opd_wipe(decoder,(size_t)state_bytes); free(decoder);
    if(rc) opd_wipe(out,samples*sizeof(*out));
    return rc;
}
