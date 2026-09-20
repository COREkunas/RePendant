static int recovery_command(const struct shell *sh,size_t argc,char **argv)
{
#ifdef OPENPENDANT_NATIVE_STORAGE
 ARG_UNUSED(sh);ARG_UNUSED(argc);ARG_UNUSED(argv);return -ENOTSUP;
#else
 struct command cmd={.task=TASK_RECOVER};struct recording_configuration cfg;
 if(argc!=3||strcmp(argv[2],"confirm")||unhex(argv[1],cmd.confirmation,16)||
  memcmp(cmd.confirmation,rr_proof.transaction,16)||!RR_EVIDENCE_ADMITTED||
  !atomic_get(&recovery_bound)||!local(sh)||!diagnostics_idle()||!recovery_one_bond()||
  atomic_get(&command_busy)||atomic_get(&volume_initialized)||atomic_get(&control_probe_used)||
  atomic_get(&phy_probe_used)||atomic_get(&preimage_used)||recovery_config(&cfg,0))return -EPERM;
 uint64_t t=now(NULL);if(t>UINT64_MAX-RV_RECOVERY_PARENT_MS)return -EINVAL;
 if(!atomic_cas(&recovery_used,0,1))return -EALREADY;
 cmd.deadline=t+RV_RECOVERY_PARENT_MS;
 int rc=queue(sh,&cmd);
 if(rc){recovery_result.attempted=1;recovery_result.rc=rc;recovery_result.cfg_rc=0;
  recovery_result.phase=cfg.phase;recovery_result.revision=cfg.revision;recovery_result.fault_banks=cfg.fault_banks;
  recovery_result.parent_deadline=cmd.deadline;
  recovery_result.first=(struct rv_recovery_first_fault){.valid=1,.stage=0,.rc=rc,.observed_ms=now(NULL),
   .start_ms=t,.parent_deadline=cmd.deadline,.phase=cfg.phase,.revision=cfg.revision};
  atomic_set(&faulted,1);}
 return rc;
#endif
}
static int recovery_status_command(const struct shell *sh,size_t argc,char **argv)
{
 ARG_UNUSED(argv);int rc=control_snapshot_lock(sh,argc);if(rc)return rc;
 if(atomic_get(&recovery_running)){atomic_clear(&command_busy);return -EBUSY;}
 const struct rv_recovery_first_fault *f=&recovery_result.first;
 shell_print(sh,"RECORDER_RECOVERY v=1 used=%u busy=0 complete=%u rc=%d stage=%u expired=%u permit_attempted=%u authorizer_saved=%u",
  (unsigned)atomic_get(&recovery_used),recovery_result.complete,recovery_result.rc,recovery_result.stage,
  recovery_result.expired,recovery_result.permit_attempted,recovery_result.authorizer_saved);
 shell_print(sh,"RECORDER_RECOVERY_STATE cfg_rc=%d phase=%u revision=%llu fault_banks=%u state=%u lease=%u stopped=%u ready=%u suspend_rc=%d elapsed_ms=%llu parent_deadline=%llu preimage_deadline=%llu mutation_deadline=%llu",
  recovery_result.cfg_rc,recovery_result.phase,(unsigned long long)recovery_result.revision,recovery_result.fault_banks,
  recovery_result.state,recovery_result.leased,recovery_result.stopped,recovery_result.ready,recovery_result.suspend_rc,
  (unsigned long long)recovery_result.elapsed_ms,(unsigned long long)recovery_result.parent_deadline,
  (unsigned long long)recovery_result.preimage_deadline,(unsigned long long)recovery_result.mutation_deadline);
 shell_print(sh,"RECORDER_RECOVERY_FAULT valid=%u stage=%u rc=%d phase=%u revision=%llu lease=%u stopped=%u ready=%u starts=%u a0=%u b0=%u c0=%u observed_ms=%llu",
  f->valid,f->stage,f->rc,f->phase,(unsigned long long)f->revision,f->leased,f->stopped,f->ready,f->starts,f->a0,f->b0,f->c0,(unsigned long long)f->observed_ms);
 shell_print(sh,"RECORDER_RECOVERY_END automatic_retry=0 audio=0 sync_binding=0 host_durability_proven=0");
 atomic_clear(&command_busy);return 0;
}
static int storage_fault_command(const struct shell *sh,size_t argc,char **argv)
{
 ARG_UNUSED(argv);int rc=control_snapshot_lock(sh,argc);if(rc)return rc;
 struct nop_nrf_diagnostics h;nand_owned_phy_nrf_diagnostics(&h);
#ifndef OPENPENDANT_NATIVE_STORAGE
 const struct rsp_first_fault *p=&volume.storage.first_fault;
#endif
 shell_print(sh,"RECORDER_SPI v=1 transfers=%u late_end=%u late_stop=%u fault=%u rc=%d opcode=%u bytes=%u fast=%u started=%u stopped=%u tx=%u rx=%u before_ms=%llu after_ms=%llu deadline=%llu",
  h.transfers,h.late_end,h.late_stop,h.valid,h.rc,h.opcode,h.bytes,h.fast,h.started,h.stopped,h.tx_amount,h.rx_amount,
  (unsigned long long)h.before_ms,(unsigned long long)h.after_ms,(unsigned long long)h.deadline);
#ifdef OPENPENDANT_NATIVE_STORAGE
 const struct recording_native_store *s=&volume.native;
 shell_print(sh,"RECORDER_NATIVE state=%u fault=%u line=%u bad_mask=%u programs=%u erases=%u reads=%u stopped=%u ready=%u phy_line=%u row=%u verified=%u wel=%u capacity=%u",
 s->state,s->fault,s->fault_line,s->bad_mask,s->programs,s->erases,s->reads,
 s->phy.stopped,s->phy.ready_known,s->phy.fault_line,s->phy.last_row,s->phy.verified_programs,s->phy.wel_observed,s->capacity);
#else
 shell_print(sh,"RECORDER_PROVIDER valid=%u stage=%u route=%u row=%u opcode=%u bytes=%u fast=%u rc=%d phy_fault=%u stopped=%u ready=%u a0=%u b0=%u c0=%u journal_fault=%u map_fault=%u programs=%llu erases=%llu",
  p->valid,p->stage,p->route,p->row,p->opcode,p->bytes,p->fast,p->transfer_rc,p->phy_fault,p->stopped,p->ready,
  p->a0,p->b0,p->c0,p->journal_fault,p->map_fault,(unsigned long long)p->programs,(unsigned long long)p->erases);
#endif
 atomic_clear(&command_busy);return 0;
}
