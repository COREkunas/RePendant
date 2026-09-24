/* Serialized by a single service actor; used=2 excludes the old diagnostics.
 * Existing fixed HAL/2-second pacing/STOP rules are reused unchanged. */
static uint32_t service_ready,service_sequence,service_deferred;
static struct battery_service_init_result service_initial;
void battery_service_init_status(struct battery_service_init_result *out)
{if(out)*out=service_initial;}
int battery_service_released(void){return stopped&&released&&!fault&&idle();}
int battery_service_claimed(void){return atomic_get(&used)==2;}
static int service_initialize(int (*allow)(void*),int (*restore_allow)(void*),
                              void *user,struct battery_restore_result *recovered,int resume)
{
 if(!allow)return -EPERM;
 if(resume){
  /* Only a verified POR deferred BEFORE recovery I/O may resume. Transport,
   * ownership, identity and partial-write failures never create this token. */
  if(!service_deferred||atomic_get(&used)!=2||!battery_service_released()||
     !recovered||recovered->attempted)return -EPERM;
  service_deferred=0;
 }else if(!atomic_cas(&used,0,2))return -EPERM;
 struct battery_restore_result r={0};struct restore_actor a={&r,allow,user,k_uptime_get(),0};
 int rc=-EPERM;service_initial=(struct battery_service_init_result){.attempted=1,.stage=1};
 service_initial.stage=2;
 if(!allow(user))goto done;
 if(!idle()){stopped=released=0;fault=1;rc=-EBUSY;goto done;}
#define BS_READ(stage_number,bit,operation,condition) do { \
 service_initial.stage=(stage_number);rc=(operation);if(rc)goto done; \
 service_initial.valid|=(bit);if(!(condition)){rc=-EPERM;goto done;} \
 }while(0)
 BS_READ(3,1,restore_query(&a,1,&service_initial.type),service_initial.type==0x427);
 BS_READ(4,2,restore_query(&a,2,&service_initial.fw),service_initial.fw==0x202);
 BS_READ(5,4,restore_query(&a,8,&service_initial.chem),service_initial.chem==0x3230);
 BS_READ(6,8,restore_settled_status(&a,&service_initial.status),
         restore_status_valid(service_initial.status)&&(restore_allow||(service_initial.status&0x2000)));
 BS_READ(7,16,restore_read(&a,6,&service_initial.flags),restore_flags_valid(service_initial.flags,restore_allow!=NULL));
#undef BS_READ
 if(!(service_initial.status&0x2000)||(service_initial.flags&0x20)){
  /* An unsealed gauge alone is NOT evidence of reset. No blind resealing,
   * resetting, configuration of an unknown part, or admission on defaults. */
  rc=-EPERM;
  if(!restore_allow||!recovered||!(service_initial.flags&0x20))goto done;
  if(!restore_allow(user)){
   service_deferred=1;rc=-EAGAIN;goto done;
  }
  service_initial.stage=9;memset(recovered,0,sizeof(*recovered));recovered->attempted=1;
  struct restore_actor repair={recovered,restore_allow,user,k_uptime_get(),0};
  rc=restore_sequence(&repair);
  if(rc&&recovered->mutated)fault=1;
  recovered->rc=rc;recovered->fault=fault;recovered->stopped=stopped;recovered->released=released;
  recovered->elapsed_ms=(uint32_t)(k_uptime_get()-repair.begin);
  if(rc)goto done;
 }
 service_ready=1;service_initial.stage=8;service_initial.complete=1;
done:
 service_initial.rc=rc;service_initial.transfers=r.transfers;
 service_initial.elapsed_ms=(uint32_t)(k_uptime_get()-a.begin);
 service_initial.stopped=stopped;service_initial.released=released;service_initial.fault=fault;
 return rc;
}
int battery_service_init(int (*allow)(void*),void *user)
{return service_initialize(allow,NULL,user,NULL,0);}
int battery_service_init_recover(int (*allow)(void*),int (*restore_allow)(void*),
                                void *user,struct battery_restore_result *recovered)
{
 if(!restore_allow||!recovered)return -EINVAL;
 return service_initialize(allow,restore_allow,user,recovered,0);
}
int battery_service_resume_recover(int (*allow)(void*),int (*restore_allow)(void*),
                                  void *user,struct battery_restore_result *recovered)
{
 if(!restore_allow||!recovered)return -EINVAL;
 return service_initialize(allow,restore_allow,user,recovered,1);
}
int battery_service_read(struct bp_sample *out,int (*allow)(void*),void *user)
{
 if(!out||!allow||!service_ready)return -EPERM;
 memset(out,0,sizeof(*out));
 struct bp_sample s={0};struct battery_restore_result r={0};
 struct restore_actor a={&r,allow,user,k_uptime_get(),1};uint16_t closing,current;int rc;
 /* Both flags bracket every sample. Changing ordinary CHG/OCV bits does not
  * invalidate it, but POR, CFGUPDATE, missing battery or thermal flags do. */
 if((rc=restore_read(&a,6,&s.flags))||!restore_flags_valid(s.flags,0))goto failed;
 if((rc=restore_read(&a,4,&s.mv)))goto failed;
 if((rc=restore_read(&a,0x1e,&s.temp)))goto failed;
 if((rc=restore_read(&a,0x1c,&s.soc)))goto failed;
 if((rc=restore_read(&a,0x10,&current)))goto failed;
 s.current_ma=(int16_t)(current<=32767U?(int32_t)current:(int32_t)current-65536);
 if((rc=restore_read(&a,6,&closing)))goto failed;
 s.flags=closing;
 if(!restore_flags_valid(closing,0))goto failed;
 if(s.mv>6000||!s.mv||s.soc>100||s.temp<2300||s.temp>3700||service_sequence==UINT32_MAX){rc=-EPERM;goto failed;}
 /* Age the complete sample from its OLDEST observation, not its last byte. */
 s.at=(uint64_t)a.begin+1000;s.flags=closing;s.valid=1;s.sequence=++service_sequence;*out=s;return 0;
failed:
 /* Retain explicit critical flags even on an incomplete sample. It is never
  * presented as valid voltage/SOC, but must not turn a thermal fault into a
  * harmless stale-data grace window in a future recording power policy. */
 service_ready=0;out->flags=s.flags;return rc?rc:-EPERM;
}
