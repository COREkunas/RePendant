/* SPDX-License-Identifier: Apache-2.0 */
#include "recording_configuration.h"
#include <errno.h>
#include <limits.h>
#include <string.h>
#include <psa/crypto.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/byteorder.h>

#define CFG_BYTES 648U
#define CFG_HASH_OFFSET 616U
static const char cfg_key[]="oprec/config1";
static const uint8_t magic[8]={'O','P','N','D','N','V','1',0};
static struct recording_configuration current;
static uint8_t saved[CFG_BYTES],scratch[CFG_BYTES];
static uint32_t initialized,fenced,present;
int rcfg_is_full(const struct recording_configuration *c)
{return c&&!memcmp(c->descriptor,"OPNDEX2\0",8);}
int rcfg_is_key_reset(const struct recording_configuration *c)
{return rcfg_is_full(c)&&c->spec.generation>=3&&!memcmp(c->descriptor+256,"OPNDKR1\0",8);}
const uint8_t *rcfg_descriptor_digest(const struct recording_configuration *c)
{return c?c->descriptor+(rcfg_is_full(c)?REX_DIGEST_OFFSET:352U):NULL;}
void rcfg_extent_identity(const struct recording_configuration *c,struct recording_extent_identity *id)
{memset(id,0,sizeof(*id));memcpy(id->device_id,c->spec.device_id,16);memcpy(id->volume_id,c->spec.volume_id,16);
 id->generation=c->spec.generation;memcpy(id->recipient_fingerprint,c->spec.recipient_fingerprint,32);}
K_MUTEX_DEFINE(configuration_lock);
static void wipe(void *p,size_t n){volatile uint8_t *b=p;while(n--)*b++=0;}
static int hash(void *u,const uint8_t *p,size_t n,uint8_t *out,size_t cap,size_t *actual)
{(void)u;return psa_hash_compute(PSA_ALG_SHA_256,p,n,out,cap,actual)==PSA_SUCCESS?0:-EIO;}
static const struct owned_page_hash hashing={NULL,hash};
static int digest(const uint8_t *p,size_t n,uint8_t out[32])
{size_t size=0;return hash(NULL,p,n,out,32,&size)||size!=32?-EIO:0;}
static int zero(const uint8_t *p,size_t n){while(n--)if(*p++)return 0;return 1;}
static int lock(void){return k_mutex_lock(&configuration_lock,K_NO_WAIT);}
static int done(int rc){wipe(scratch,sizeof(scratch));k_mutex_unlock(&configuration_lock);return rc;}
int rcfg_device_id(uint8_t out[16])
{
 if(!out)return -EINVAL;
 static const char domain[]="OpenPendant device v1";
 uint8_t bytes[sizeof(domain)+8],sum[32];memcpy(bytes,domain,sizeof(domain));
 if(hwinfo_get_device_id(bytes+sizeof(domain),8)!=8||digest(bytes,sizeof(bytes),sum))return -EIO;
 memcpy(out,sum,16);out[6]=(out[6]&15U)|0x50U;out[8]=(out[8]&63U)|0x80U;wipe(sum,sizeof(sum));return 0;
}
static int recipient_valid(const uint8_t point[65],const uint8_t expected[32])
{
 static const uint8_t domain[]="OpenPendant recipient public key v1";
 uint8_t bytes[sizeof(domain)+2+65],sum[32];
 if(point[0]!=4)return -EINVAL;
 memcpy(bytes,domain,sizeof(domain));bytes[sizeof(domain)]=0;bytes[sizeof(domain)+1]=0x10;
 memcpy(bytes+sizeof(domain)+2,point,65);
 if(digest(bytes,sizeof(bytes),sum)||memcmp(sum,expected,32))return -EINVAL;
 psa_key_attributes_t attr=PSA_KEY_ATTRIBUTES_INIT;mbedtls_svc_key_id_t key=MBEDTLS_SVC_KEY_ID_INIT;
 psa_set_key_type(&attr,PSA_KEY_TYPE_ECC_PUBLIC_KEY(PSA_ECC_FAMILY_SECP_R1));psa_set_key_bits(&attr,256);
 psa_status_t status=psa_import_key(&attr,point,65,&key);psa_reset_key_attributes(&attr);
 if(status!=PSA_SUCCESS)return -EINVAL;
 if(psa_destroy_key(key)!=PSA_SUCCESS){fenced=1;return -EIO;}
 return 0;
}
static int scope(const struct owned_volume_spec *spec,const uint8_t point[65])
{
 uint8_t device[16];if(rcfg_device_id(device)||memcmp(device,spec->device_id,16)||spec->generation!=1)return -EINVAL;
 for(unsigned i=0;i<32;++i)if(spec->map_blocks[i]!=1025U+i)return -EINVAL;
 if(spec->control_blocks[0]!=1057||spec->control_blocks[1]!=1058)return -EINVAL;
 return recipient_valid(point,spec->recipient_fingerprint);
}
static void spec_from_trusted_settings(struct owned_volume_spec *s,const uint8_t *d)
{
 memset(s,0,sizeof(*s));memcpy(s->device_id,d+16,16);memcpy(s->volume_id,d+32,16);s->generation=sys_get_le64(d+48);
 memcpy(s->recipient_fingerprint,d+108,32);memcpy(s->preservation_manifest_sha256,d+140,32);
 for(unsigned i=0;i<32;++i)s->map_blocks[i]=sys_get_le32(d+172+4*i);
 for(unsigned i=0;i<2;++i)s->control_blocks[i]=sys_get_le32(d+300+4*i);
}
static int decode(const uint8_t raw[CFG_BYTES],struct recording_configuration *out)
{
 int rotation=!memcmp(raw,"OPNDNV3\0",8)&&sys_get_le16(raw+8)==3;
 int full=rotation||(!memcmp(raw,"OPNDNV2\0",8)&&sys_get_le16(raw+8)==2);
 uint8_t sum[32];if((!full&&(memcmp(raw,magic,8)||sys_get_le16(raw+8)!=1))||sys_get_le16(raw+10)!=32||
  !zero(raw+28,4)||!zero(raw+609,7)||digest(raw,CFG_HASH_OFFSET,sum)||memcmp(sum,raw+CFG_HASH_OFFSET,32))return -EINVAL;
 memset(out,0,sizeof(*out));out->phase=sys_get_le32(raw+12);out->revision=sys_get_le64(raw+16);out->fault_banks=sys_get_le32(raw+24);
 if(out->phase<RCFG_PREPARED||out->phase>RCFG_ACTIVE||!out->revision||out->revision>INT64_MAX||out->fault_banks>3)return -EINVAL;
 /* Schema v1 has only two phase advances and two idempotent bank latches.
  * No other mutation exists, so revision must describe a reachable state. */
 if(out->revision!=(uint64_t)out->phase+(out->fault_banks&1U)+((out->fault_banks>>1)&1U))return -EINVAL;
 memcpy(out->descriptor,raw+32,512);memcpy(out->recipient,raw+544,65);
 if(full){
  const uint8_t *d=out->descriptor;uint8_t device[16];
  if((!rotation&&!zero(d+256,256))||rcfg_device_id(device))return -EINVAL;
  memcpy(out->spec.device_id,d+16,16);memcpy(out->spec.volume_id,d+32,16);
  out->spec.generation=sys_get_le64(d+48);memcpy(out->spec.recipient_fingerprint,d+56,32);
  struct recording_extent_identity id;rcfg_extent_identity(out,&id);
  if(memcmp(device,id.device_id,16)||(rotation?(id.generation<3||id.generation>INT64_MAX):id.generation!=2)||recipient_valid(out->recipient,id.recipient_fingerprint)||
   rex_validate_full(d,&id,&hashing))return -EINVAL;
  if(rotation){
   /* The upper half is a config-only transition journal, not NAND descriptor
    * bytes. The ordinary256B descriptor still binds every encrypted object.
    * Reconstruct the exact parent, including its public point/fingerprint. */
   struct recording_extent_identity parent=id;uint8_t descriptor[256];
   memcpy(parent.volume_id,d+296,16);parent.generation=sys_get_le64(d+312);
   memcpy(parent.recipient_fingerprint,d+320,32);
   if(memcmp(d+256,"OPNDKR1\0",8)||d[417]!=1||!zero(d+418,94)||
    parent.generation<2||parent.generation!=id.generation-1||
    !memcmp(parent.volume_id,id.volume_id,16)||!memcmp(parent.recipient_fingerprint,id.recipient_fingerprint,32)||
    recipient_valid(d+352,parent.recipient_fingerprint)||rex_build_full(descriptor,&parent,&hashing)||
    memcmp(descriptor+224,d+264,32))return -EINVAL;
  }
  return 0;
 }
 /* Only authenticated-by-location owner settings are a trust source here.
  * Never call this on host/NAND descriptors to invent provisioning authority. */
 spec_from_trusted_settings(&out->spec,out->descriptor);
 struct owned_volume_decoded checked;
 if(scope(&out->spec,out->recipient)||owned_volume_descriptor_validate(out->descriptor,512,&out->spec,&hashing,&checked))return -EINVAL;
 return 0;
}
static int encode(const struct recording_configuration *value,uint8_t out[CFG_BYTES])
{
 memset(out,0,CFG_BYTES);memcpy(out,magic,8);sys_put_le16(1,out+8);sys_put_le16(32,out+10);
 if(rcfg_is_full(value)){memcpy(out,"OPNDNV2\0",8);sys_put_le16(2,out+8);}
 if(rcfg_is_key_reset(value)){memcpy(out,"OPNDNV3\0",8);sys_put_le16(3,out+8);}
 sys_put_le32(value->phase,out+12);sys_put_le64(value->revision,out+16);sys_put_le32(value->fault_banks,out+24);
 memcpy(out+32,value->descriptor,512);memcpy(out+544,value->recipient,65);return digest(out,CFG_HASH_OFFSET,out+CFG_HASH_OFFSET);
}
struct load_context { uint8_t *bytes;uint32_t seen;int error; };
static int read_setting(const char *key,size_t len,settings_read_cb read_cb,void *cb_arg,void *param)
{
 struct load_context *load=param;
 /* settings_load_subtree_direct() in the pinned SDK ignores backend return
  * values, including a propagated callback error. Retain it independently;
  * a malformed/short/duplicate leaf must never look absent or successful. */
 if(load->error)return load->error;
 if((key&&*key)||load->seen||len!=CFG_BYTES||!read_cb){load->error=-EINVAL;return load->error;}
 load->seen=1;
 if(read_cb(cb_arg,load->bytes,len)!=(ssize_t)len)load->error=-EIO;
 return load->error;
}
static int read_saved(uint8_t out[CFG_BYTES])
{struct load_context load={out,0,0};int rc=settings_load_subtree_direct(cfg_key,read_setting,&load);return rc?rc:load.error?load.error:load.seen==1?0:1;}
static int persist(struct recording_configuration *next)
{
 if(fenced||next->revision>INT64_MAX||encode(next,scratch)){fenced=1;return -EIO;}
 uint8_t expected[32];memcpy(expected,scratch+CFG_HASH_OFFSET,32);
 if(settings_save_one(cfg_key,scratch,CFG_BYTES)||read_saved(scratch)||memcmp(expected,scratch+CFG_HASH_OFFSET,32)||
    decode(scratch,next)){fenced=1;return -EIO;}
 memcpy(saved,scratch,CFG_BYTES);current=*next;present=1;return 0;
}
int rcfg_init(void)
{
 int rc=lock();if(rc)return rc;if(initialized)return done(-EALREADY);initialized=1;
 rc=read_saved(saved);if(rc==1)return done(1);
 if(rc||decode(saved,&current)){fenced=1;return done(-EIO);}present=1;return done(0);
}
int rcfg_get(struct recording_configuration *out)
{
 if(!out)return -EINVAL;
 int rc=lock();if(rc)return rc;
 if(!initialized||fenced)return done(-EIO);
 if(!present)return done(1);
 *out=current;return done(0);
}
int rcfg_prepare(const struct owned_volume_spec *spec,const uint8_t recipient[65])
{
 if(!spec||!recipient)return -EINVAL;
 int rc=lock();if(rc)return rc;
 if(!initialized||fenced||present)return done(-EPERM);
 struct recording_configuration next={0};next.spec=*spec;memcpy(next.recipient,recipient,65);
 if(scope(spec,recipient)||owned_volume_descriptor_build(next.descriptor,512,spec,&hashing))return done(-EINVAL);
 next.phase=RCFG_PREPARED;next.revision=1;return done(persist(&next));
}
int rcfg_advance(uint32_t expected,uint32_t next_phase)
{
 int rc=lock();if(rc)return rc;
 if(!initialized||fenced||!present||current.fault_banks||current.phase!=expected||
  !((expected==RCFG_PREPARED&&next_phase==RCFG_PROVISIONING)||(expected==RCFG_PROVISIONING&&next_phase==RCFG_ACTIVE)))return done(-EPERM);
 struct recording_configuration next=current;next.phase=next_phase;++next.revision;return done(persist(&next));
}
int rcfg_prepare_full(const uint8_t expected[32],const uint8_t volume[16])
{
 if(!expected||!volume)return -EINVAL;
 int rc=lock();if(rc)return rc;
 if(!initialized||fenced||!present||current.phase!=RCFG_ACTIVE||current.fault_banks||
  rcfg_is_full(&current)||current.spec.generation!=1||memcmp(current.descriptor+352,expected,32)||
  !memcmp(current.spec.volume_id,volume,16))return done(-EPERM);
 struct recording_configuration next={0};
 memcpy(next.spec.device_id,current.spec.device_id,16);memcpy(next.spec.volume_id,volume,16);next.spec.generation=2;
 memcpy(next.spec.recipient_fingerprint,current.spec.recipient_fingerprint,32);memcpy(next.recipient,current.recipient,65);
 struct recording_extent_identity id;rcfg_extent_identity(&next,&id);
 if(rex_build_full(next.descriptor,&id,&hashing))return done(-EINVAL);
 next.phase=RCFG_PREPARED;next.revision=1;return done(persist(&next));
}
int rcfg_prepare_key_reset(const uint8_t expected[32],const uint8_t volume[16],
 const uint8_t fingerprint[32],const uint8_t recipient[65])
{
 if(!expected||!volume||!fingerprint||!recipient)return -EINVAL;
 int rc=lock();if(rc)return rc;
 if(!initialized||fenced||!present||current.phase!=RCFG_ACTIVE||current.fault_banks||
  !rcfg_is_full(&current)||current.spec.generation<2||current.spec.generation>=INT64_MAX||
  memcmp(rcfg_descriptor_digest(&current),expected,32)||!memcmp(current.spec.volume_id,volume,16)||
  !memcmp(current.spec.recipient_fingerprint,fingerprint,32))return done(-EPERM);
 if(recipient_valid(recipient,fingerprint))return done(-EINVAL);
 struct recording_configuration next={0};
 memcpy(next.spec.device_id,current.spec.device_id,16);memcpy(next.spec.volume_id,volume,16);
 next.spec.generation=current.spec.generation+1;
 memcpy(next.spec.recipient_fingerprint,fingerprint,32);memcpy(next.recipient,recipient,65);
 struct recording_extent_identity id;rcfg_extent_identity(&next,&id);
 if(rex_build_full(next.descriptor,&id,&hashing))return done(-EINVAL);
 uint8_t *d=next.descriptor;memcpy(d+256,"OPNDKR1\0",8);
 memcpy(d+264,rcfg_descriptor_digest(&current),32);memcpy(d+296,current.spec.volume_id,16);
 sys_put_le64(current.spec.generation,d+312);memcpy(d+320,current.spec.recipient_fingerprint,32);
 memcpy(d+352,current.recipient,65);d[417]=1; /* delete pendant only */
 next.phase=RCFG_PREPARED;next.revision=1;return done(persist(&next));
}
int rcfg_latch_fault(const uint8_t digest32[32],uint32_t bank)
{
 if(!digest32||bank>1)return -EINVAL;
 int rc=lock();if(rc)return rc;
 if(!initialized||fenced||!present||memcmp(rcfg_descriptor_digest(&current),digest32,32))return done(-EPERM);
 if(current.fault_banks&(1U<<bank))return done(0);
 struct recording_configuration next=current;next.fault_banks|=1U<<bank;++next.revision;return done(persist(&next));
}
int rcfg_faulted(const uint8_t digest32[32])
{
 if(!digest32)return 1;
 int rc=lock();if(rc)return 1;
 return done(!initialized||fenced||!present||current.fault_banks||memcmp(rcfg_descriptor_digest(&current),digest32,32)?1:0);
}
