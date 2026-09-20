/* Included only by the feature-gated runtime, after the worker entry points. */
static int long_publish(void)
{
 struct recording_capture_status c;struct rw_status w;
 int cr=recording_capture_get_status(&c),wr=rw_get_status(&w);
 if(cr==-EBUSY||wr==RW_BUSY)return LRC_BUSY;
 if(cr||wr||c.fault||c.capture_error)return LRC_FAULT;
 struct lcw_observation v={.usb=(uint32_t)atomic_get(&usb_configured),.mic_on=c.power_on,
  .producer_joined=c.joined&&c.clock_stopped&&c.buffers_scrubbed,.storage_joined=!atomic_get(&storage_running),
  .released=!atomic_get(&leased)&&atomic_get(&volume_suspended)&&!guards[0]&&!guards[1],
  .accepted_frames=w.epoch&&w.epoch==c.epoch?c.submitted_frames:0};
 return lcw_publish(&long_worker,&v);
}
static void long_terminal_publish(void)
{
 /* Worker has already joined capture/storage and released ownership. This is
  * bounded metadata publication ONLY: no renewed NAND/codec/lease deadline.
  * Benign concurrent STATUS must not reset a successfully stopped recorder. */
 uint64_t first=now(NULL),last=first;
 for(unsigned tries=0;tries<32;++tries){
  int rc=long_publish();if(!rc)return;if(rc!=LRC_BUSY)break;
  uint64_t t=now(NULL);if(t<last||t-first>=50U)break;last=t;k_msleep(1);
 }
 /* A permanently unavailable/corrupt cache remains UNKNOWN to the phone.
  * Preserve joined worker proof and fault in this boot; never reset/retry. */
 atomic_store(&long_control.fault,1);atomic_set(&faulted,1);
}
static int long_failure(uint32_t ticket)
{
 /* A queued START that fails before worker entry must not stay STARTING in a
  * truthful cached reply. Retain its identity/counters; no invented cleanup. */
 struct lc_state s;struct recording_capture_status c;
 if(lrc_snapshot(&long_control,ticket,&s))return -EPERM;
 s.phase=LC_FAULT;s.reason=RW_ADMISSION;s.flags=(uint16_t)((s.flags&~(LC_FINALIZED|LC_MIC_ON|LC_RELEASED|LC_PRODUCER_JOINED|LC_STORAGE_JOINED|LC_USB_PRESENT))|LC_FAULT_LATCH);
 if(atomic_get(&usb_configured))s.flags|=LC_USB_PRESENT;
 if(!recording_capture_get_status(&c)){
  if(c.power_on)s.flags|=LC_MIC_ON;
  if(c.joined&&c.clock_stopped&&c.buffers_scrubbed&&!c.power_on)s.flags|=LC_PRODUCER_JOINED;
 }
 if(!atomic_get(&storage_running))s.flags|=LC_STORAGE_JOINED;
 if(!atomic_get(&leased)&&!atomic_get(&recording)&&!guards[0]&&!guards[1]&&
    (s.flags&(LC_PRODUCER_JOINED|LC_STORAGE_JOINED))==(LC_PRODUCER_JOINED|LC_STORAGE_JOINED))s.flags|=LC_RELEASED;
 return lrc_publish(&long_control,ticket,&s);
}
static int long_auth(void *u,uint64_t conn)
{ARG_UNUSED(u);return long_authorized?long_authorized(long_auth_user,conn):0;}
static int long_usb(void *u){ARG_UNUSED(u);return atomic_get(&usb_configured)?1:0;}
static int long_power_ready(void)
{
#ifdef OPENPENDANT_PORTABLE_RECORDING
 return portable_admit();
#else
 return atomic_get(&usb_configured)!=0;
#endif
}
static int long_ready(void *u)
{
 ARG_UNUSED(u);return long_bound&&!atomic_get(&capacity_used)&&!atomic_get(&trial_used)&&!atomic_get(&recovery_used)&&atomic_get(&ready)&&atomic_get(&sync_configured)&&!atomic_get(&faulted)&&
  long_power_ready()&&!atomic_get(&command_busy)&&!atomic_get(&catalog_epoch)&&!atomic_get(&retire_pending)&&
  !atomic_get(&sync_actor)&&!atomic_get(&leased)&&!atomic_get(&recording)&&!atomic_get(&storage_running)&&
  !mic_commands_busy()&&!pendant_ble_pairing_busy()&&!pendant_recovery_is_pending();
}
static int long_queue(void *u,uint32_t ticket,uint64_t d)
{
 ARG_UNUSED(u);uint64_t t=now(NULL);
 if(!long_ready(NULL)||d<=t||d-t>LRC_PREPARE_MS||!atomic_cas(&command_busy,0,1))return -EBUSY;
 if(atomic_get(&catalog_epoch)||atomic_get(&leased)||atomic_get(&recording)||atomic_get(&storage_running)||
    !long_power_ready()){atomic_clear(&command_busy);return -EPERM;}
 struct command cmd={.task=TASK_START,.mode=RP_CONTINUOUS,.control_ticket=ticket,.deadline=d,
  .usb_epoch=(uint32_t)atomic_get(&usb_epoch)};
#ifdef OPENPENDANT_PORTABLE_RECORDING
 cmd.portable=1;
#endif
 if(k_msgq_put(&runtime_commands,&cmd,K_NO_WAIT)){atomic_clear(&command_busy);return -EBUSY;}
 k_sem_give(&command_dispatch);return 0;
}
int recording_runtime_long_bind(int (*auth)(void*,uint64_t),void *user,const uint8_t boot[16],const uint8_t binding[32])
{
 /* Trusted platform supplies PSA boot nonce and full ACTIVE configuration hash.
  * Binding performs no mount or microphone action; no callback under IRQ lock. */
 if(long_bound||!auth||!atomic_get(&ready)||!atomic_get(&sync_configured)||atomic_get(&command_busy))return -EPERM;
 const struct lrc_port port={NULL,now,long_auth,long_ready,long_queue,wake,long_usb};
 int rc=lrc_init(&long_control,&port,boot,binding);if(rc)return rc;
#ifdef OPENPENDANT_STANDALONE_CONTROL
 rc=lrc_set_standalone(&long_control,1);if(rc)return rc;
#endif
 long_auth_user=user;long_authorized=auth;long_bound=1;return 0;
}
int recording_runtime_long_pending(uint64_t conn,const uint8_t *p,size_t n,uint8_t out[LC_RESPONSE_BYTES])
{return long_bound?lrc_pending_reply(&long_control,conn,p,n,out):LRC_REFUSED;}
int recording_runtime_long_available(void){return long_bound&&!atomic_get(&faulted);}
void recording_runtime_long_button(int pressed)
{if(long_bound)(void)lrc_button_sample(&long_control,pressed);}
int recording_runtime_long_command(uint64_t conn,const uint8_t *p,size_t n,uint8_t out[LC_RESPONSE_BYTES])
{if(!long_bound)return LRC_REFUSED;return lrc_command(&long_control,conn,p,n,out);}
int recording_runtime_long_disconnected(uint64_t conn)
{return long_bound?lrc_disconnected(&long_control,conn):LRC_REFUSED;}
int recording_runtime_long_reply_allowed(uint64_t conn,uint16_t seq)
{return long_bound?lrc_reply_allowed(&long_control,conn,seq):0;}

static int long_initialize(void)
{
 struct recording_configuration cfg;
 if(rcfg_get(&cfg)||cfg.phase!=RCFG_ACTIVE||cfg.fault_banks||!rcfg_is_full(&cfg))return 0;
 static const char label[]="OpenPendant long recording control binding v1";
 uint8_t bytes[sizeof(label)+72U],digest[32],boot[16];size_t actual=0;
 memcpy(bytes,label,sizeof(label));size_t p=sizeof(label);
 memcpy(bytes+p,cfg.spec.device_id,16);p+=16;memcpy(bytes+p,cfg.spec.volume_id,16);p+=16;
 sys_put_le64(cfg.spec.generation,bytes+p);p+=8;memcpy(bytes+p,cfg.spec.recipient_fingerprint,32);
 if(psa_hash_compute(PSA_ALG_SHA_256,bytes,sizeof(bytes),digest,sizeof(digest),&actual)!=PSA_SUCCESS||
    actual!=32||psa_generate_random(boot,sizeof(boot))!=PSA_SUCCESS)return -EIO;
 boot[6]=(boot[6]&15U)|0x40U;boot[8]=(boot[8]&63U)|0x80U;
 return recording_runtime_long_bind(recording_control_ble_authorized,NULL,boot,digest);
}
