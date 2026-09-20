/* SPDX-License-Identifier: Apache-2.0 */
#include "durable_ble_codec.h"
#include <limits.h>
#include <string.h>

static int span(const void *p,size_t n) { return p!=NULL && n!=0U && (uintptr_t)p<=UINTPTR_MAX-(n-1U); }
static int separate(const void *a,size_t an,const void *b,size_t bn) {
 if(!span(a,an)||!span(b,bn))return 0;
 return (uintptr_t)a<(uintptr_t)b ? (uintptr_t)b-(uintptr_t)a>=an : (uintptr_t)a-(uintptr_t)b>=bn;
}
static int identity(const uint8_t *p,size_t n) {
 uint8_t nz=0U,nff=0U;for(size_t i=0;i<n;i++){nz|=p[i];nff|=(uint8_t)(p[i]^255U);}return nz!=0U&&nff!=0U;
}
static int zero(const uint8_t *p,size_t n) { uint8_t v=0U;for(size_t i=0;i<n;i++)v|=p[i];return v==0U; }
static uint16_t get16(const uint8_t *p) { return (uint16_t)((uint16_t)p[0]|((uint16_t)p[1]<<8)); }
static uint32_t get32(const uint8_t *p) { uint32_t v=0U;for(unsigned i=0;i<4U;i++)v|=(uint32_t)p[i]<<(i*8U);return v; }
static void put16(uint8_t *p,uint16_t v) { for(unsigned i=0;i<2U;i++)p[i]=(uint8_t)(v>>(i*8U)); }
static void put32(uint8_t *p,uint32_t v) { for(unsigned i=0;i<4U;i++)p[i]=(uint8_t)(v>>(i*8U)); }
static void put64(uint8_t *p,uint64_t v) { for(unsigned i=0;i<8U;i++)p[i]=(uint8_t)(v>>(i*8U)); }
int durable_ble_is_full(uint8_t command) {return command>=DB_FULL_CATALOG&&command<=DB_FULL_RECEIVE_RANGE;}
static uint8_t kind(uint8_t command){return command==DB_FULL_RECEIVE_RANGE?DB_RECEIVE_ACK:command==DB_FULL_STREAM?DB_SEGMENT:durable_ble_is_full(command)?(uint8_t)(command-8U):command;}
static int revision(uint64_t v,int full) { return v<=(full?DB_FULL_MAX_SEGMENTS*4U+2U:130U) && v%4U<=2U; }
static int container(uint32_t n) { return n>=425U && n<=DB_MAX_CONTAINER && (n-357U)%68U==0U; }
static int request_valid(const struct db_request *r) {
 if(!identity(r->nonce,16U)||r->sequence==0U)return 0;
 int full=durable_ble_is_full(r->command);
 uint32_t max_data=full?DB_FULL_MAX_DATA:DB_MAX_DATA,max_segments=full?DB_FULL_MAX_SEGMENTS:32U;
 if(r->command==DB_FULL_STREAM)max_data=DB_STREAM_MAX_DATA;
 switch(kind(r->command)) {
 case DB_CATALOG:
  return r->offset<(full?DB_FULL_MAX_CATALOG:DB_MAX_CATALOG)&&r->maximum>=1U&&r->maximum<=max_data&&
   zero(r->recording,16U)&&zero(r->operation,16U)&&zero(r->sha256,32U)&&r->revision==0U&&r->segment==0U&&r->container_bytes==0U;
 case DB_MANIFEST:
  return identity(r->recording,16U)&&identity(r->sha256,32U)&&revision(r->revision,full)&&
   r->offset<128U+64U*(r->revision/4U)&&r->maximum>=1U&&r->maximum<=max_data&&
   zero(r->operation,16U)&&r->segment==0U&&r->container_bytes==0U;
 case DB_SEGMENT:
  return identity(r->recording,16U)&&identity(r->sha256,32U)&&r->segment<max_segments&&r->offset<DB_MAX_CONTAINER&&
   r->maximum>=1U&&r->maximum<=max_data&&zero(r->operation,16U)&&r->revision==0U&&r->container_bytes==0U;
 case DB_RECEIVE_ACK:
  return identity(r->recording,16U)&&identity(r->sha256,32U)&&r->segment<max_segments&&
   (r->command==DB_FULL_RECEIVE_RANGE?(r->container_bytes>=1U&&r->container_bytes<=DB_RECEIPT_BATCH_MAX&&
      r->container_bytes<=max_segments-r->segment):container(r->container_bytes))&&
   zero(r->operation,16U)&&r->revision==0U&&r->offset==0U&&r->maximum==0U;
 case DB_DELETE:
  return identity(r->operation,16U)&&identity(r->sha256,32U)&&zero(r->recording,16U)&&r->revision==0U&&
   r->segment==0U&&r->container_bytes==0U&&r->offset==0U&&r->maximum==0U;
 default:return 0;
 }
}
int durable_ble_parse_request(const uint8_t *frame,size_t bytes,struct db_request *out) {
 if(bytes<8U||bytes>DB_MAX_REQUEST||!separate(frame,bytes,out,sizeof(*out)))return DB_ARGUMENT;
 uint8_t wire=frame[3],command=kind(wire);int full=durable_ble_is_full(wire);size_t payload=bytes-8U;
 if(!full&&bytes>80U)return DB_ARGUMENT;
 if(frame[0]!='O'||frame[1]!='P'||frame[2]!=1U||get16(&frame[6])!=payload||
    command<DB_CATALOG||command>DB_DELETE||get16(&frame[4])==0U)return DB_INVALID;
 size_t expected=command==DB_CATALOG?20U:72U;
 if(full&&command<=DB_SEGMENT)expected+=2U;
 if(payload!=expected)return DB_INVALID;
 const uint8_t *p=&frame[8];struct db_request r;memset(&r,0,sizeof(r));
 r.command=wire;r.sequence=get16(&frame[4]);memcpy(r.nonce,p,16U);
 unsigned extra=full?2U:0U;
 if(command==DB_CATALOG){r.offset=full?get32(p+16U):get16(p+16U);r.maximum=p[18+extra];if(p[19+extra]!=0U)return DB_INVALID;}
 else if(command==DB_MANIFEST){
  memcpy(r.recording,p+16U,16U);r.revision=get32(p+32U);memcpy(r.sha256,p+36U,32U);
  r.offset=full?get32(p+68U):get16(p+68U);r.maximum=p[70+extra];if(p[71+extra]!=0U)return DB_INVALID;
 } else if(command==DB_SEGMENT){
  memcpy(r.recording,p+16U,16U);r.segment=get32(p+32U);r.offset=full?get32(p+36U):get16(p+36U);r.maximum=p[38+extra];
  if(wire==DB_FULL_STREAM)r.maximum=get16(p+40U);
  else if(p[39+extra]!=0U)return DB_INVALID;
  memcpy(r.sha256,p+40U+extra,32U);
 } else if(command==DB_RECEIVE_ACK){
  memcpy(r.recording,p+16U,16U);r.segment=get32(p+32U);r.container_bytes=get32(p+36U);memcpy(r.sha256,p+40U,32U);
 } else {memcpy(r.operation,p+16U,16U);memcpy(r.sha256,p+32U,32U);if(!zero(p+64U,8U))return DB_INVALID;}
 if(!request_valid(&r))return DB_INVALID;
 memcpy(out,&r,sizeof(r));return DB_OK;
}
static void header(uint8_t *out,const struct db_request *r,uint16_t payload) {
 out[0]='O';out[1]='P';out[2]=1U;out[3]=(uint8_t)(r->command|0x80U);
 put16(out+4U,r->sequence);put16(out+6U,(uint16_t)(payload+1U));out[8]=0U;
}
int durable_ble_read_response(uint8_t *out,size_t capacity,const struct db_request *r,
 uint32_t total,const uint8_t *data,size_t bytes) {
 if(!r)return DB_ARGUMENT;
 int full=durable_ble_is_full(r->command);uint8_t command=kind(r->command);unsigned prefix=full?33U:29U;
 if(bytes<1U||bytes>(full?DB_FULL_MAX_DATA:DB_MAX_DATA)||capacity!=prefix+bytes||!separate(out,capacity,r,sizeof(*r))||
    !separate(out,capacity,data,bytes)||!separate(r,sizeof(*r),data,bytes))return DB_ARGUMENT;
 if(!request_valid(r)||command<DB_CATALOG||command>DB_SEGMENT||r->offset>=total||(!full&&total>UINT16_MAX))return DB_INVALID;
 if(command==DB_CATALOG&&(total<128U||total>(full?DB_FULL_MAX_CATALOG:DB_MAX_CATALOG)||(total-128U)%64U!=0U))return DB_INVALID;
 if(command==DB_MANIFEST&&total!=128U+64U*(r->revision/4U))return DB_INVALID;
 if(command==DB_SEGMENT&&!container(total))return DB_INVALID;
 size_t remaining=(size_t)total-r->offset;size_t wanted=remaining<r->maximum?remaining:r->maximum;
 if(r->command==DB_FULL_STREAM&&wanted>DB_FULL_MAX_DATA)wanted=DB_FULL_MAX_DATA;
 if(bytes!=wanted)return DB_INVALID;
 header(out,r,(uint16_t)(prefix-9U+bytes));memcpy(out+9U,r->nonce,16U);
 if(full){put32(out+25U,r->offset);put32(out+29U,total);}
 else{put16(out+25U,(uint16_t)r->offset);put16(out+27U,(uint16_t)total);}
 memcpy(out+prefix,data,bytes);return DB_OK;
}
int durable_ble_receipt_response(uint8_t *out,size_t capacity,const struct db_request *r) {
 if(capacity!=81U||!separate(out,capacity,r,sizeof(*r)))return DB_ARGUMENT;
 if(!request_valid(r)||kind(r->command)!=DB_RECEIVE_ACK)return DB_INVALID;
 header(out,r,72U);memcpy(out+9U,r->nonce,16U);memcpy(out+25U,r->recording,16U);
 put32(out+41U,r->segment);put32(out+45U,r->container_bytes);memcpy(out+49U,r->sha256,32U);return DB_OK;
}
int durable_ble_delete_response(uint8_t *out,size_t capacity,const struct db_request *r,
 uint64_t original_manifest_revision,uint64_t tombstone_revision) {
 if(capacity!=81U||!separate(out,capacity,r,sizeof(*r)))return DB_ARGUMENT;
 if(!request_valid(r)||kind(r->command)!=DB_DELETE||!revision(original_manifest_revision,durable_ble_is_full(r->command))||original_manifest_revision%4U==0U||
    tombstone_revision<=original_manifest_revision||tombstone_revision>INT64_MAX)return DB_INVALID;
 header(out,r,72U);memcpy(out+9U,r->nonce,16U);memcpy(out+25U,r->operation,16U);
 memcpy(out+41U,r->sha256,32U);put64(out+73U,tombstone_revision);return DB_OK;
}
static int catalog_build(uint8_t *out,size_t capacity,const struct db_volume *v,
 const uint8_t nonce[16],uint64_t snapshot_revision,const struct db_catalog_entry *entries,size_t count,int full) {
 if(count>(full?DB_FULL_MAX_RECORDINGS:DB_MAX_RECORDINGS)||capacity!=DB_CATALOG_HEADER+DB_CATALOG_ENTRY*count||
    !separate(out,capacity,v,sizeof(*v))||!separate(out,capacity,nonce,16U)||!separate(v,sizeof(*v),nonce,16U)||
    (count!=0U&&(!separate(out,capacity,entries,sizeof(*entries)*count)||!separate(v,sizeof(*v),entries,sizeof(*entries)*count)||
                !separate(nonce,16U,entries,sizeof(*entries)*count))))return DB_ARGUMENT;
 if(!identity(v->device,16U)||!identity(v->volume,16U)||!identity(v->fingerprint,32U)||!identity(nonce,16U)||
    v->generation==0U||v->generation>INT64_MAX||snapshot_revision==0U||snapshot_revision>INT64_MAX)return DB_INVALID;
 for(size_t i=0;i<count;i++) {
  const struct db_catalog_entry *e=&entries[i];
  if(!identity(e->recording,16U)||!identity(e->manifest_sha256,32U)||e->segments>(full?DB_FULL_MAX_SEGMENTS:32U)||e->state>2U||
     e->revision!=(uint64_t)e->segments*4U+e->state||(i!=0U&&memcmp(entries[i-1U].recording,e->recording,16U)>=0))return DB_INVALID;
 }
 memset(out,0,capacity);memcpy(out,full?"OPNDCT2":"OPNDCT1",7U);put16(out+8U,full?2U:1U);put16(out+10U,128U);put16(out+12U,64U);
 memcpy(out+16U,v->device,16U);memcpy(out+32U,v->volume,16U);put64(out+48U,v->generation);
 memcpy(out+56U,nonce,16U);put64(out+72U,snapshot_revision);memcpy(out+80U,v->fingerprint,32U);
 put32(out+112U,(uint32_t)count);put32(out+116U,2U);
 for(size_t i=0;i<count;i++) {
  const struct db_catalog_entry *e=&entries[i];uint8_t *p=out+128U+64U*i;
  memcpy(p,e->recording,16U);put64(p+16U,e->revision);put32(p+24U,e->segments);put32(p+28U,e->state);memcpy(p+32U,e->manifest_sha256,32U);
 }
 return DB_OK;
}
int durable_ble_catalog_build(uint8_t *out,size_t capacity,const struct db_volume *v,
 const uint8_t nonce[16],uint64_t revision,const struct db_catalog_entry *entries,size_t count)
{return catalog_build(out,capacity,v,nonce,revision,entries,count,0);}
int durable_ble_catalog_build_full(uint8_t *out,size_t capacity,const struct db_volume *v,
 const uint8_t nonce[16],uint64_t revision,const struct db_catalog_entry *entries,size_t count)
{return catalog_build(out,capacity,v,nonce,revision,entries,count,1);}
