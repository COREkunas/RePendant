#include "recording_pipeline.h"
#include "opus.h"
#include <stdbool.h>
#include <string.h>
#include <zephyr/sys/atomic.h>

static atomic_t rp_active,rp_initialized;
static struct rp_sink sink;
static struct rp_status status;
static struct es_binding session;
static union { uint64_t align; uint8_t bytes[RP_ENCODER_CAPACITY]; } encoder_state;
static uint8_t plaintext[RP_PLAINTEXT_CAPACITY],packet[1275];
static int16_t flush_pcm[RP_FRAME_SAMPLES];
static uint32_t packet_count,segment_flags;
static uint64_t segment_first,committed_next;
static int (*async_poll)(void*,const struct es_binding*,struct rp_segment_receipt*);
static struct es_binding pending_binding;
static uint64_t pending_next;
static uint32_t drain_action,drain_reason; /* 1=pause,2=end; no implicit capture. */
_Static_assert(RP_PLAINTEXT_CAPACITY<=ES_MAX_PLAINTEXT_BYTES,"Encrypted segment bound");
_Static_assert(sizeof(opus_int16)==sizeof(int16_t),"PCM16 required");

static void wipe(void *pointer,size_t count)
{ volatile uint8_t *p=pointer;while(count--)*p++=0; }
static void clear_scratch(void)
{
 wipe(encoder_state.bytes,sizeof(encoder_state.bytes));wipe(plaintext,sizeof(plaintext));
 wipe(packet,sizeof(packet));wipe(flush_pcm,sizeof(flush_pcm));status.buffers_wiped=1;
 packet_count=0;status.segment_frames=0;
}
static bool zero(const void *p,size_t n)
{ const uint8_t *b=p;uint8_t value=0;for(size_t i=0;i<n;++i)value|=b[i];return value==0; }
static void put(uint8_t *p,uint64_t value,size_t n)
{ for(size_t i=0;i<n;++i)p[i]=(uint8_t)(value>>(8U*i)); }
static uint64_t get(const uint8_t *p,size_t n)
{ uint64_t value=0;for(size_t i=0;i<n;++i)value|=(uint64_t)p[i]<<(8U*i);return value; }
static int lock(void){return atomic_cas(&rp_active,0,1)?RP_OK:RP_BUSY;}
static int unlock(int rc){atomic_clear(&rp_active);return rc;}
static bool complete_state(void)
{ return status.state==RP_STOPPED || status.state==RP_FULL || status.state==RP_OVERFLOW || status.state==RP_LOW_POWER; }
static bool can_start(void){return !status.pending_segments && (status.state==RP_IDLE || (complete_state()&&(status.manifest_finalized || !status.session_reserved)));}
static bool same_identity(const struct es_binding *a,const struct es_binding *b)
{
 return !memcmp(a->key_fingerprint,b->key_fingerprint,32) && !memcmp(a->device_id,b->device_id,16) &&
  !memcmp(a->volume_id,b->volume_id,16) && !memcmp(a->recording_id,b->recording_id,16) &&
  a->generation==b->generation && a->segment_sequence==b->segment_sequence && a->plaintext_bytes==b->plaintext_bytes;
}
static bool same_completion(const struct rp_completion *a,const struct rp_completion *b)
{
 return same_identity(&a->session,&b->session) && a->mode==b->mode && a->reason==b->reason &&
  a->segments==b->segments && a->gaps==b->gaps && a->captured_samples==b->captured_samples &&
  a->committed_samples==b->committed_samples && a->next_sample==b->next_sample;
}
static int fault(int rc,uint32_t reason)
{
 if(status.pending_segments)status.commit_uncertain=1;
 status.discarded_samples+=(uint64_t)status.segment_frames*RP_FRAME_SAMPLES;
 status.error=rc;status.reason=reason;status.state=RP_FAULT;clear_scratch();return rc;
}
static bool valid_packet(const uint8_t *p,uint32_t profile)
{
 opus_int16 sizes[48];
 return (profile==RP_PROFILE_LEGACY_VOIP ||
         (profile==RP_PROFILE_CELT_LOW_DELAY && (p[0]&0x80U)!=0U)) &&
  opus_packet_get_nb_samples(p,RP_PACKET_BYTES,16000)==320 &&
  opus_packet_get_nb_channels(p)==1 &&
  opus_packet_parse(p,RP_PACKET_BYTES,NULL,NULL,sizes,NULL)>0;
}
static int codec_init(void)
{
 clear_scratch();status.buffers_wiped=0;
 const char *version=opus_get_version_string();
 if(!version || strcmp(version,"libopus 1.6.1-fixed")!=0)return fault(RP_CODEC_ERROR,RP_REASON_CODEC);
 int bytes=opus_encoder_get_size(1);
 if(bytes<=0 || (uint32_t)bytes>sizeof(encoder_state.bytes))return fault(RP_CODEC_ERROR,RP_REASON_CODEC);
 OpusEncoder *encoder=(OpusEncoder*)(void*)encoder_state.bytes;
 if(status.codec_profile!=RP_PROFILE_LEGACY_VOIP && status.codec_profile!=RP_PROFILE_CELT_LOW_DELAY)
  return fault(RP_CODEC_ERROR,RP_REASON_CODEC);
 int application=status.codec_profile==RP_PROFILE_CELT_LOW_DELAY?
  OPUS_APPLICATION_RESTRICTED_LOWDELAY:OPUS_APPLICATION_VOIP;
#define CODEC(expression) do{status.codec_error=(expression);if(status.codec_error!=OPUS_OK)return fault(RP_CODEC_ERROR,RP_REASON_CODEC);}while(0)
 CODEC(opus_encoder_init(encoder,16000,1,application));
 CODEC(opus_encoder_ctl(encoder,OPUS_SET_BITRATE(24000)));
 CODEC(opus_encoder_ctl(encoder,OPUS_SET_COMPLEXITY(3)));
 CODEC(opus_encoder_ctl(encoder,OPUS_SET_VBR(0)));
 CODEC(opus_encoder_ctl(encoder,OPUS_SET_DTX(0)));
 CODEC(opus_encoder_ctl(encoder,OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE)));
 CODEC(opus_encoder_ctl(encoder,OPUS_SET_INBAND_FEC(0)));
 CODEC(opus_encoder_ctl(encoder,OPUS_SET_PACKET_LOSS_PERC(0)));
 opus_int32 delay=-1;CODEC(opus_encoder_ctl(encoder,OPUS_GET_LOOKAHEAD(&delay)));
#undef CODEC
 if(delay!=(status.codec_profile==RP_PROFILE_CELT_LOW_DELAY?40:104))
  return fault(RP_CODEC_ERROR,RP_REASON_CODEC);
 status.lookahead=(uint32_t)delay;segment_first=status.next_sample;
 segment_flags=status.next_sample==(status.pending_segments?pending_next:committed_next)?1U:3U;
 return RP_OK;
}
static int encode(const int16_t *pcm,bool flushing)
{
 if(packet_count>=RP_SEGMENT_FRAMES+1U)return fault(RP_LIMIT,RP_REASON_LIMIT);
 int bytes=opus_encode((OpusEncoder*)(void*)encoder_state.bytes,pcm,RP_FRAME_SAMPLES,packet,sizeof(packet));
 status.codec_error=bytes<0?bytes:0;
 if(bytes!=(int)RP_PACKET_BYTES || !valid_packet(packet,status.codec_profile))return fault(RP_CODEC_ERROR,RP_REASON_CODEC);
 uint8_t *record=plaintext+RP_HEADER_BYTES+packet_count*RP_RECORD_BYTES;
 put(record,RP_PACKET_BYTES,2);put(record+2,flushing?1U:0U,2);put(record+4,packet_count,4);
 memcpy(record+8,packet,RP_PACKET_BYTES);++packet_count;wipe(packet,sizeof(packet));return RP_OK;
}
int rp_segment_validate(const uint8_t *p,size_t n)
{
 static const uint8_t magic[8]={'O','P','N','D','O','P','1',0};
 if(!p || n<RP_HEADER_BYTES || n>RP_PLAINTEXT_CAPACITY)return RP_ARGUMENT;
 uint32_t frames=(uint32_t)get(p+48,4),packets=(uint32_t)get(p+52,4),delay=(uint32_t)get(p+56,4);
 uint32_t profile=(uint32_t)get(p+72,4);
 if(memcmp(p,magic,8) || get(p+8,2)!=1 || get(p+10,2)!=80 || get(p+12,4)!=16000 ||
    get(p+16,2)!=1 || get(p+18,2)!=320 || get(p+20,4)!=24000 || get(p+24,4)!=3 ||
    (get(p+28,4)!=1 && get(p+28,4)!=3) || frames==0 || frames>RP_SEGMENT_FRAMES || delay>320 ||
    packets!=frames+(delay?1U:0U) || get(p+60,4)!=(delay?320U-delay:0U) ||
    get(p+64,4)!=packets*RP_RECORD_BYTES || get(p+68,4)!=frames*320U ||
    (profile!=RP_PROFILE_LEGACY_VOIP && profile!=RP_PROFILE_CELT_LOW_DELAY) ||
    (profile==RP_PROFILE_CELT_LOW_DELAY && delay!=40) ||
    get(p+76,4)!=0 || n!=RP_HEADER_BYTES+(size_t)packets*RP_RECORD_BYTES ||
    get(p+32,8)>UINT64_MAX-(uint64_t)frames*320U || get(p+40,8)!=get(p+32,8)+(uint64_t)frames*320U)return RP_ARGUMENT;
 for(uint32_t i=0;i<packets;++i){
  const uint8_t *record=p+RP_HEADER_BYTES+i*RP_RECORD_BYTES;
  if(get(record,2)!=60 || get(record+2,2)!=(i>=frames?1U:0U) || get(record+4,4)!=i ||
     !valid_packet(record+8,profile))return RP_ARGUMENT;
 }
 return RP_OK;
}
static int poll_pending(void)
{
 if(!status.pending_segments)return RP_OK;
 if(!async_poll || status.pending_segments!=1 || !status.pending_samples)
  return fault(RP_RECEIPT_ERROR,RP_REASON_RECEIPT);
 struct rp_segment_receipt receipt;memset(&receipt,0,sizeof(receipt));
 int rc=async_poll(sink.user,&pending_binding,&receipt);status.store_error=rc;
 if(rc==RP_IO_PENDING && zero(&receipt,sizeof(receipt)))return RP_PENDING;
 if(rc!=RP_IO_OK)return fault(RP_STORE_ERROR,RP_REASON_STORE);
 if(receipt.durable!=1 || es_header_validate(receipt.es_header,sizeof(receipt.es_header),&pending_binding))
  return fault(RP_RECEIPT_ERROR,RP_REASON_RECEIPT);
 if(status.segments>=INT32_MAX || status.committed_samples>UINT64_MAX-status.pending_samples)
  return fault(RP_LIMIT,RP_REASON_LIMIT);
 ++status.segments;status.committed_samples+=status.pending_samples;committed_next=pending_next;
 status.pending_segments=0;status.pending_samples=0;pending_next=0;wipe(&pending_binding,sizeof(pending_binding));
 return RP_OK;
}
static int seal(void)
{
 if(status.segment_frames==0)return RP_OK;
 int polled=poll_pending();if(polled<0)return polled;
 if(polled==RP_PENDING)return fault(RP_STORE_ERROR,RP_REASON_STORE);
 if(status.segments>=INT32_MAX)return fault(RP_LIMIT,RP_REASON_LIMIT);
 if(status.lookahead){int rc=encode(flush_pcm,true);if(rc)return rc;}
 memcpy(plaintext,"OPNDOP1\0",8);put(plaintext+8,1,2);put(plaintext+10,80,2);
 put(plaintext+12,16000,4);put(plaintext+16,1,2);put(plaintext+18,320,2);
 put(plaintext+20,24000,4);put(plaintext+24,3,4);put(plaintext+28,segment_flags,4);
 put(plaintext+32,segment_first,8);put(plaintext+40,status.next_sample,8);
 put(plaintext+48,status.segment_frames,4);put(plaintext+52,packet_count,4);put(plaintext+56,status.lookahead,4);
 put(plaintext+60,status.lookahead?320U-status.lookahead:0U,4);put(plaintext+64,packet_count*RP_RECORD_BYTES,4);
 put(plaintext+68,status.segment_frames*320U,4);put(plaintext+72,status.codec_profile,4);put(plaintext+76,0,4);
 size_t size=RP_HEADER_BYTES+(size_t)packet_count*RP_RECORD_BYTES;
 if(rp_segment_validate(plaintext,size))return fault(RP_CODEC_ERROR,RP_REASON_CODEC);
 struct es_binding binding=session;binding.segment_sequence=status.segments;binding.plaintext_bytes=(uint32_t)size;
 struct rp_segment_receipt receipt;memset(&receipt,0,sizeof(receipt));
 status.state=RP_FINALIZING;
 status.store_error=sink.seal_commit(sink.user,&binding,plaintext,size,&receipt);
 if(async_poll && status.store_error==RP_IO_PENDING && zero(&receipt,sizeof(receipt))) {
  pending_binding=binding;pending_next=status.next_sample;
  status.pending_segments=1;status.pending_samples=(uint64_t)status.segment_frames*320U;
  clear_scratch();return RP_OK;
 }
 if(status.store_error==RP_IO_FULL && zero(&receipt,sizeof(receipt))) {
  status.discarded_samples+=(uint64_t)status.segment_frames*320U;clear_scratch();return RP_STORAGE_FULL;
 }
 if(status.store_error!=RP_IO_OK){status.commit_uncertain=status.store_error!=RP_IO_FULL && status.store_error!=RP_IO_FAILED;
  return fault(RP_STORE_ERROR,RP_REASON_STORE);}
 if(receipt.durable!=1 || es_header_validate(receipt.es_header,sizeof(receipt.es_header),&binding)) {
  status.commit_uncertain=1;return fault(RP_RECEIPT_ERROR,RP_REASON_RECEIPT);
 }
 ++status.segments;status.committed_samples+=(uint64_t)status.segment_frames*320U;committed_next=status.next_sample;
 clear_scratch();return RP_OK;
}
static int finish(uint32_t reason)
{
 if(status.pending_segments){drain_action=2;drain_reason=reason;status.state=RP_DRAINING;return RP_PENDING;}
 int rc=seal();if(rc==RP_STORAGE_FULL)reason=RP_REASON_FULL;else if(rc)return rc;
 if(status.pending_segments){drain_action=2;drain_reason=reason;status.state=RP_DRAINING;return RP_PENDING;}
 status.state=RP_FINALIZING;status.reason=reason;
 struct rp_completion completion={0};completion.session=session;completion.mode=status.mode;completion.reason=reason;
 completion.segments=status.segments;completion.gaps=status.gaps;completion.captured_samples=status.captured_samples;
 completion.committed_samples=status.committed_samples;completion.next_sample=status.next_sample;
 struct rp_final_receipt receipt;memset(&receipt,0,sizeof(receipt));
 status.store_error=sink.finalize(sink.user,&completion,&receipt);
 if(status.store_error!=RP_IO_OK){status.commit_uncertain=status.store_error!=RP_IO_FAILED && status.store_error!=RP_IO_FULL;
  return fault(RP_STORE_ERROR,RP_REASON_STORE);}
 if(receipt.durable!=1 || !same_completion(&receipt.completion,&completion)) {
  status.commit_uncertain=1;return fault(RP_RECEIPT_ERROR,RP_REASON_RECEIPT);
 }
 status.manifest_finalized=1;clear_scratch();status.reason=reason;
 status.state=reason==RP_REASON_FULL?RP_FULL:reason==RP_REASON_OVERFLOW?RP_OVERFLOW:
              reason==RP_REASON_LOW_POWER?RP_LOW_POWER:RP_STOPPED;
 status.error=reason==RP_REASON_FULL?RP_STORAGE_FULL:0;return status.error;
}
static int pause_finish(void)
{
 if(status.pending_segments){drain_action=1;status.state=RP_DRAINING;return RP_PENDING;}
 int rc=seal();if(rc==RP_STORAGE_FULL)return finish(RP_REASON_FULL);if(rc)return rc;
 if(status.pending_segments){drain_action=1;status.state=RP_DRAINING;return RP_PENDING;}
 drain_action=0;clear_scratch();status.state=RP_PAUSED;return RP_OK;
}
int rp_init(const struct rp_sink *callbacks,enum rp_mode mode,uint32_t stack)
{ return rp_init_profile(callbacks,mode,stack,RP_PROFILE_LEGACY_VOIP); }
static int init_core(const struct rp_sink *callbacks,enum rp_mode mode,uint32_t stack,enum rp_profile profile,
 int (*poll)(void*,const struct es_binding*,struct rp_segment_receipt*))
{
 if(!callbacks || !callbacks->reserve || !callbacks->seal_commit || !callbacks->finalize ||
    (mode!=RP_MANUAL && mode!=RP_CONTINUOUS) || stack<RP_WORKER_STACK_MIN ||
    (profile!=RP_PROFILE_LEGACY_VOIP && profile!=RP_PROFILE_CELT_LOW_DELAY))return RP_ARGUMENT;
 int rc=lock();if(rc)return rc;
 if(!atomic_cas(&rp_initialized,0,1))return unlock(RP_STATE);
 sink=*callbacks;memset(&status,0,sizeof(status));status.state=RP_IDLE;status.mode=(uint32_t)mode;
 async_poll=poll;wipe(&pending_binding,sizeof(pending_binding));pending_next=0;drain_action=drain_reason=0;
 status.codec_profile=(uint32_t)profile;
 clear_scratch();return unlock(RP_OK);
}
int rp_init_profile(const struct rp_sink *callbacks,enum rp_mode mode,uint32_t stack,enum rp_profile profile)
{ return init_core(callbacks,mode,stack,profile,NULL); }
int rp_init_async_profile(const struct rp_async_sink *callbacks,enum rp_mode mode,uint32_t stack,enum rp_profile profile)
{ if(!callbacks || !callbacks->poll)return RP_ARGUMENT;return init_core(&callbacks->submit,mode,stack,profile,callbacks->poll); }
int rp_poll(void)
{
 int rc=lock();if(rc)return rc;
 if(!async_poll || (status.state!=RP_RECORDING && status.state!=RP_DRAINING && status.state!=RP_PAUSED))return unlock(RP_STATE);
 rc=poll_pending();if(rc)return unlock(rc);
 if(status.state==RP_DRAINING){
  if(drain_action==1)rc=pause_finish();
  else if(drain_action==2){uint32_t reason=drain_reason;drain_action=0;rc=finish(reason);}
  else rc=fault(RP_STATE,RP_REASON_EXTERNAL);
 }
 return unlock(rc);
}
int rp_get_status(struct rp_status *out)
{ if(!out)return RP_ARGUMENT;int rc=lock();if(rc)return rc;*out=status;return unlock(RP_OK); }
int rp_select_mode(enum rp_mode mode)
{
 if(mode!=RP_MANUAL && mode!=RP_CONTINUOUS)return RP_ARGUMENT;
 int rc=lock();if(rc)return rc;if(!can_start())return unlock(RP_STATE);
 status.mode=(uint32_t)mode;return unlock(RP_OK);
}
int rp_start(const struct es_binding *identity,uint64_t first)
{
 if(!identity || identity->segment_sequence!=0 || identity->plaintext_bytes!=0 || first>UINT64_MAX-320U)return RP_ARGUMENT;
 struct es_binding checked=*identity;checked.plaintext_bytes=1;uint8_t header[ES_HEADER_BYTES];
 if(es_header_build(header,sizeof(header),&checked))return RP_ARGUMENT;
 int rc=lock();if(rc)return rc;if(!can_start())return unlock(RP_STATE);
 if(status.session_reserved && same_identity(identity,&session))return unlock(RP_ARGUMENT);
 uint32_t mode=status.mode,profile=status.codec_profile;memset(&status,0,sizeof(status));
 status.mode=mode;status.codec_profile=profile;session=*identity;status.next_sample=first;committed_next=first;
 drain_action=drain_reason=0;
 status.state=RP_FINALIZING;status.store_error=sink.reserve(sink.user,&session);
 if(status.store_error==RP_IO_FULL){status.state=RP_FULL;status.reason=RP_REASON_FULL;status.error=RP_STORAGE_FULL;
  clear_scratch();return unlock(RP_STORAGE_FULL);}
 if(status.store_error!=RP_IO_OK){status.commit_uncertain=status.store_error!=RP_IO_FAILED;
  return unlock(fault(RP_STORE_ERROR,RP_REASON_STORE));}
 status.session_reserved=1;rc=codec_init();if(rc)return unlock(rc);
 status.state=RP_RECORDING;return unlock(RP_OK);
}
int rp_push_frame(uint64_t first,const int16_t pcm[RP_FRAME_SAMPLES])
{
 if(!pcm || (uintptr_t)pcm>UINTPTR_MAX-RP_FRAME_SAMPLES*sizeof(*pcm))return RP_ARGUMENT;
 int rc=lock();if(rc)return rc;if(status.state!=RP_RECORDING)return unlock(RP_STATE);
 rc=poll_pending();if(rc<0)return unlock(rc);
 if(first!=status.next_sample)return unlock(fault(RP_CONTINUITY_ERROR,RP_REASON_CONTINUITY));
 if(first>UINT64_MAX-320U || status.captured_samples>UINT64_MAX-320U)return unlock(fault(RP_LIMIT,RP_REASON_LIMIT));
 rc=encode(pcm,false);if(rc)return unlock(rc);
 ++status.segment_frames;status.next_sample+=320U;status.captured_samples+=320U;
 if(status.segment_frames==RP_SEGMENT_FRAMES){
  rc=seal();if(rc==RP_STORAGE_FULL)return unlock(finish(RP_REASON_FULL));if(rc)return unlock(rc);
  rc=codec_init();if(rc)return unlock(rc);status.state=RP_RECORDING;
 }
 return unlock(RP_OK);
}
int rp_pause(void)
{
 int rc=lock();if(rc)return rc;if(status.state!=RP_RECORDING)return unlock(RP_STATE);
 return unlock(pause_finish());
}
int rp_resume(uint64_t next)
{
 int rc=lock();if(rc)return rc;if(status.state!=RP_PAUSED)return unlock(RP_STATE);
 if(next<status.next_sample || next>UINT64_MAX-320U)return unlock(RP_ARGUMENT);
 bool gap=next!=status.next_sample;if(gap && status.gaps==UINT32_MAX)return unlock(fault(RP_LIMIT,RP_REASON_LIMIT));
 status.next_sample=next;rc=codec_init();if(rc)return unlock(rc);
 if(gap){++status.gaps;segment_flags|=2U;}status.state=RP_RECORDING;return unlock(RP_OK);
}
int rp_end(enum rp_reason reason)
{
 if(reason!=RP_REASON_USER && reason!=RP_REASON_FULL && reason!=RP_REASON_OVERFLOW && reason!=RP_REASON_LOW_POWER)return RP_ARGUMENT;
 int rc=lock();if(rc)return rc;
 if(status.state==RP_DRAINING && drain_action==1){drain_action=2;drain_reason=(uint32_t)reason;return unlock(RP_PENDING);}
 if(status.state!=RP_RECORDING && status.state!=RP_PAUSED)return unlock(RP_STATE);
 return unlock(finish((uint32_t)reason));
}
int rp_external_fault(void)
{
 int rc=lock();if(rc)return rc;
 if(status.state!=RP_RECORDING && status.state!=RP_PAUSED && status.state!=RP_DRAINING)return unlock(RP_STATE);
 return unlock(fault(RP_EXTERNAL_ERROR,RP_REASON_EXTERNAL));
}
