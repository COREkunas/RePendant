/* Fixed public candidate only. No private archive or host-durability inference. */
static void recovery_count_bond(const struct bt_bond_info *info,void *out)
{ARG_UNUSED(info);unsigned *n=out;if(*n<2U)++*n;}
static int recovery_one_bond(void)
{unsigned n=0;bt_foreach_bond(BT_ID_DEFAULT,recovery_count_bond,&n);return n==1U;}
static int recovery_config(struct recording_configuration *out,int active_ok)
{
 uint8_t device[16];
 if(rcfg_get(out)||rcfg_device_id(device)||memcmp(device,rr_evidence.expected_device,16)||
  memcmp(out->spec.device_id,rr_evidence.expected_device,16)||out->fault_banks||out->spec.generation!=1||
  memcmp(out->spec.volume_id,rr_expected_descriptor+32,16)||
  memcmp(out->spec.recipient_fingerprint,rr_expected_descriptor+108,32)||
  memcmp(out->spec.preservation_manifest_sha256,rr_evidence.original_manifest,32)||
  memcmp(out->descriptor,rr_expected_descriptor,512)||memcmp(out->recipient,rr_expected_recipient,65)||
  !((out->phase==RCFG_PROVISIONING&&out->revision==2)||
    (active_ok&&out->phase==RCFG_ACTIVE&&out->revision==3)))return -EPERM;
 for(unsigned i=0;i<32;i++)if(out->spec.map_blocks[i]!=1025U+i)return -EPERM;
 return out->spec.control_blocks[0]!=1057U||out->spec.control_blocks[1]!=1058U?-EPERM:0;
}
static int recovery_check(void *u,int writing,uint64_t d)
{
 ARG_UNUSED(u);ARG_UNUSED(writing);uint64_t t=now(NULL);
 if(!RR_EVIDENCE_ADMITTED||!atomic_get(&recovery_running)||!atomic_get(&recovery_used)||
  !atomic_get(&command_busy)||active.task!=TASK_RECOVER||atomic_get(&recovery_expired)||
  t<recovery_last||t>=d||d>recovery_deadline||t>=recovery_deadline||active.deadline!=recovery_deadline||
  !atomic_get(&usb_configured)||active.usb_epoch!=recovery_session||
  recovery_session!=(uint32_t)atomic_get(&usb_epoch)||atomic_get(&faulted)||
  atomic_get(&recording)||atomic_get(&storage_running)||atomic_get(&catalog_epoch)||
  pendant_recovery_is_pending()||pendant_ble_pairing_busy()||!recovery_one_bond())return -EPERM;
 uint64_t after=now(NULL);
 if(after<t||after>=d||after>=recovery_deadline)return -EPERM;
 recovery_last=after;return 0;
}
static int recovery_acquire(void *u,uint64_t d)
{
 if(recovery_check(u,0,d)||!recovery_result.authorizer_saved||atomic_get(&leased)||mic_commands_reserve_external())return -EPERM;
 lease_usb_epoch=recovery_session;atomic_set(&leased,1);
 return recovery_check(u,0,d); /* Ambiguous late acquire retains ownership. */
}
static int recovery_release(void *u,uint64_t d)
{
 if(recovery_check(u,0,d)||!atomic_get(&leased)||lease_usb_epoch!=recovery_session||
  atomic_get(&recording)||atomic_get(&storage_running))return -EPERM;
 atomic_clear(&leased);mic_commands_release_external();return 0;
}
static int recovery_power(void *u,uint32_t access,uint32_t row,uint64_t d)
{ARG_UNUSED(access);ARG_UNUSED(row);return recovery_check(u,1,d);}
static int recovery_yield(void *u,uint64_t d)
{if(recovery_check(u,0,d))return -EPERM;k_yield();return recovery_check(u,0,d);}
static int recovery_forbid_provision(void *u,const struct recording_configuration *c,const uint8_t p[32],uint64_t d)
{ARG_UNUSED(u);ARG_UNUSED(c);ARG_UNUSED(p);ARG_UNUSED(d);return -EPERM;}
static int recovery_forbid_receipt(void *u,const struct rsm_volume *v,const struct rsm_receipt *r,uint64_t d)
{ARG_UNUSED(u);ARG_UNUSED(v);ARG_UNUSED(r);ARG_UNUSED(d);return -EPERM;}
static int recovery_forbid_delete(void *u,const struct rsm_volume *v,const uint8_t p[16],const struct rsm_terminal *r,uint64_t d)
{ARG_UNUSED(u);ARG_UNUSED(v);ARG_UNUSED(p);ARG_UNUSED(r);ARG_UNUSED(d);return -EPERM;}
static const struct rv_hooks recovery_hooks={NULL,recovery_acquire,recovery_release,recovery_check,
 recovery_power,recovery_yield,recovery_forbid_provision,recovery_forbid_receipt,recovery_forbid_delete};
int recording_runtime_recovery_permit_bind(const struct rr_external_permit *port)
{
 if(!port||!port->consume||atomic_get(&recovery_used)||atomic_get(&command_busy)||
  !atomic_cas(&recovery_bound,0,1))return -EPERM;
 recovery_external=*port;return 0;
}
#ifndef OPENPENDANT_NATIVE_STORAGE
static int recovery_consume_permit(void *u,const uint8_t tx[16],const uint8_t digest[32],uint64_t d)
{
 ARG_UNUSED(u);
 if(recovery_check(NULL,0,d)||!atomic_get(&recovery_bound)||!recovery_external.consume||
  memcmp(tx,rr_proof.transaction,16)||memcmp(tx,active.confirmation,16)||
  memcmp(digest,rr_expected_record_digest,32)||!atomic_cas(&recovery_permit_used,0,1))return -EPERM;
 recovery_result.permit_attempted=1;
 int rc=recovery_external.consume(recovery_external.user,tx,digest,recovery_session,d);
 return rc?rc:recovery_check(NULL,0,d);
}
static int recovery_consume_authorizer(void *u,const struct recording_configuration *c,const struct rv_recovery_proof *p,uint64_t d)
{
 ARG_UNUSED(u);
 if(recovery_check(NULL,0,d))return -EPERM;
 int rc=ra_consume(&recovery_authorizer,c,p,d);
 if(rc||recovery_check(NULL,0,d))return rc?rc:-EPERM;
 recovery_result.authorizer_saved=1;return 0;
}
#endif
static void recovery_fault(uint32_t stage,int rc)
{
 if(recovery_result.first.valid)return;
 if(volume.recovery_first_fault.valid){recovery_result.first=volume.recovery_first_fault;return;}
 struct rv_recovery_first_fault *f=&recovery_result.first;
 f->valid=1;f->stage=stage;f->rc=rc;f->observed_ms=now(NULL);f->start_ms=recovery_started;
 f->parent_deadline=recovery_deadline;f->preimage_deadline=volume.recovery_preimage;
 f->mutation_deadline=volume.recovery_mutation;f->phase=recovery_result.phase;f->revision=recovery_result.revision;
 f->leased=(uint32_t)atomic_get(&leased);
#ifdef OPENPENDANT_NATIVE_STORAGE
 f->stopped=volume.native.phy.stopped;f->ready=volume.native.phy.ready_known;
 f->provider_fault=volume.native.fault;f->phy_fault=volume.native.phy.fault;
 f->starts=volume.native.phy.starts;f->a0=volume.native.phy.a0;f->b0=volume.native.phy.b0;
 f->c0=volume.native.phy.status;f->media_flags=volume.native.phy.media_flags;
#else
 f->stopped=volume.storage.phy.stopped;
 f->ready=volume.storage.phy.ready_known;f->provider_fault=volume.storage.fault;
 f->epoch=volume.storage.journal.current.epoch;f->high_water=volume.storage.journal.current.high_water;
 f->journal_fault=volume.storage.journal.fault;f->map_fault=volume.storage.map.fault;f->phy_fault=volume.storage.phy.fault;
 f->starts=volume.storage.phy.starts;f->a0=volume.storage.phy.a0;f->b0=volume.storage.phy.b0;
 f->c0=volume.storage.phy.status;f->media_flags=volume.storage.phy.media_flags;f->provider=volume.storage.first_fault;
#endif
}
static void execute_recovery(void)
{
#ifdef OPENPENDANT_NATIVE_STORAGE
 /* This is the retired legacy NAND control-journal transaction, not the
  * signed USB firmware bootloader. Native command admission already rejects
  * it; fail closed here too if a stale internal task somehow reaches us. */
 recovery_result.attempted=1;recovery_result.rc=-ENOTSUP;
 recovery_fault(0,-ENOTSUP);atomic_set(&faulted,1);
 if(active.shell)shell_print(active.shell,"RECORDER_DONE task=11 rc=%d microphone=0",-ENOTSUP);
 memset(&active,0,sizeof(active));atomic_clear(&command_busy);
#else
 struct recording_configuration cfg;struct ra_port port;const struct owned_page_hash hash={NULL,catalog_hash};
 const struct ra_settings_permit permit={NULL,recovery_consume_permit};
 const struct rv_recovery_authorizer authorizer={NULL,recovery_consume_authorizer};
 int rc=-EPERM;recovery_started=now(NULL);recovery_last=recovery_started;
 unsigned key=irq_lock();recovery_deadline=active.deadline;recovery_session=active.usb_epoch;
 atomic_set(&recovery_running,1);irq_unlock(key);
 recovery_result.attempted=1;recovery_result.stage=1;recovery_result.parent_deadline=active.deadline;
 if(recovery_check(NULL,0,active.deadline)||recovery_config(&cfg,0)||
  memcmp(active.confirmation,rr_proof.transaction,16)||atomic_get(&volume_initialized)||
  atomic_get(&control_probe_used)||atomic_get(&phy_probe_used)||atomic_get(&preimage_used))goto finished;
 recovery_result.stage=2;
 rc=ra_settings_bind(&port,&permit);if(rc)goto finished;
 rc=ra_init(&recovery_authorizer,&port,&hash,&cfg,&rr_proof,&rr_evidence);if(rc)goto finished;
 if(recovery_check(NULL,0,active.deadline)){rc=-EPERM;goto finished;}
 recovery_result.stage=3;
 atomic_set(&volume_initialized,1); /* Attempted permanent singleton; never reinitialize. */
 rc=rv_recovery_init(&volume,&recovery_hooks,&authorizer,&rr_proof);if(rc)goto finished;
 recovery_result.stage=4;rc=rv_recovery_run(&volume,active.deadline);if(rc)goto finished;
 recovery_result.stage=5;
 /* Same previously latched M; no new timeout even after successful phase3. */
 if(!volume.recovery_mutation||recovery_check(NULL,0,volume.recovery_mutation)||
  recovery_config(&cfg,1)||cfg.phase!=RCFG_ACTIVE||cfg.revision!=3){rc=-EPERM;goto finished;}
 recovery_result.suspend_rc=rv_suspend(&volume,volume.recovery_mutation);
 if(recovery_result.suspend_rc){rc=recovery_result.suspend_rc;goto finished;}
 recovery_result.stage=6;
 if(recovery_check(NULL,0,volume.recovery_mutation)||recovery_config(&cfg,1)||
  cfg.phase!=RCFG_ACTIVE||cfg.revision!=3||volume.state!=RV_SUSPENDED||volume.leased||
  atomic_get(&leased)||!volume.storage.phy.stopped||!volume.storage.phy.ready_known){rc=-EPERM;goto finished;}
 atomic_set(&volume_mounted,1);atomic_set(&volume_suspended,1);
 recovery_result.complete=1;rc=0;
finished:
 recovery_result.cfg_rc=rcfg_get(&cfg);
 if(!recovery_result.cfg_rc){recovery_result.phase=cfg.phase;recovery_result.revision=cfg.revision;recovery_result.fault_banks=cfg.fault_banks;}
 recovery_result.state=volume.state;recovery_result.leased=(uint32_t)atomic_get(&leased);
 recovery_result.stopped=volume.storage.phy.stopped;recovery_result.ready=volume.storage.phy.ready_known;
 recovery_result.preimage_deadline=volume.recovery_preimage;recovery_result.mutation_deadline=volume.recovery_mutation;
 if(!rc&&(recovery_result.cfg_rc||recovery_result.phase!=RCFG_ACTIVE||recovery_result.revision!=3||
  recovery_check(NULL,0,volume.recovery_mutation)))rc=-EPERM;
 /* Irrevocable joined-result point: no callbacks under this short lock. A
  * supervisor/USB IRQ between the last check and here cannot publish success.
  * Once running is cleared, a later timer cannot retroactively expire the job. */
 unsigned finish_key=irq_lock();uint64_t t=now(NULL);
 if(t>=recovery_deadline)atomic_set(&recovery_expired,1);
 if(!rc&&(t<recovery_last||t>=recovery_deadline||t>=volume.recovery_mutation||
  atomic_get(&recovery_expired)||atomic_get(&faulted)||!atomic_get(&usb_configured)||
  (uint32_t)atomic_get(&usb_epoch)!=recovery_session))rc=-EPERM;
 recovery_result.elapsed_ms=t>=recovery_started?t-recovery_started:0;
 recovery_result.rc=rc;recovery_result.complete=(uint32_t)(rc==0);
 recovery_result.expired=(uint32_t)atomic_get(&recovery_expired);
 atomic_clear(&recovery_running);
 irq_unlock(finish_key);
 if(rc){recovery_fault(recovery_result.stage,rc);atomic_set(&faulted,1);}
 if(active.shell)shell_print(active.shell,"RECORDER_DONE task=11 rc=%d microphone=0",rc);
 memset(&active,0,sizeof(active));atomic_clear(&command_busy);
#endif
}
