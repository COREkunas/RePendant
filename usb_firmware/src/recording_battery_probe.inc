/* Included after diagnostics_idle. Explicit shell actor only; never boot/BLE. */
static struct battery_probe_result battery_result;
static struct battery_restore_result battery_restore;
int pendant_button_read(void);
static int battery_control_lock(void)
{
 if(!long_bound)return 1;
 unsigned expected=0;
 if(!atomic_compare_exchange_strong(&long_control.gate,&expected,1))return 0;
 if(long_control.armed || long_control.submitting || long_control.fault ||
    long_control.current>long_control.used ||
    (long_control.current && long_control.states[long_control.current-1].phase>=LC_STARTING &&
     long_control.states[long_control.current-1].phase<=LC_DRAINING)){
  atomic_store(&long_control.gate,0);return 0;
 }
 return 1;
}
static void battery_control_unlock(void)
{if(long_bound)atomic_store(&long_control.gate,0);}
static int battery_allowed(void *user)
{
 const uint32_t *usb_generation=user;
 return *usb_generation==(uint32_t)atomic_get(&usb_epoch) && atomic_get(&usb_configured) &&
  atomic_get(&command_busy) && !atomic_get(&faulted) && !atomic_get(&recording) &&
  !atomic_get(&leased) && !atomic_get(&storage_running) && !atomic_get(&catalog_epoch) &&
  !atomic_get(&sync_actor) && !atomic_get(&retire_pending) &&
  !pendant_recovery_is_pending() && !pendant_ble_pairing_busy();
}
static int battery_status_command(const struct shell *sh,size_t argc,char **argv)
{
 ARG_UNUSED(argv);if(argc!=1 || sh!=shell_backend_uart_get_ptr())return -EPERM;
 if(atomic_get(&command_busy))return -EBUSY;
 shell_print(sh,"BATTERY_PROBE rc=%d attempted=%u reads=%u stopped=%u released=%u fault=%u coherent=%u trusted=%u elapsed_ms=%u config_writes=0",
  battery_result.rc,battery_result.attempted,battery_result.reads,battery_result.stopped,
  battery_result.released,battery_result.fault,battery_result.coherent,battery_result.trusted,battery_result.elapsed_ms);
 for(unsigned i=0;i<battery_result.reads && i<3;++i){
  const struct battery_probe_sample *s=&battery_result.sample[i];
  shell_print(sh,"BATTERY_SAMPLE index=%u raw=%u rc=%d tx=%u rx=%u errors=%u start=%u finish=%u",
   i,s->raw,s->rc,s->tx,s->rx,s->errors,s->started_ms,s->finished_ms);
 }
 return 0;
}
static int battery_probe_command(const struct shell *sh,size_t argc,char **argv)
{
 if(argc!=2 || strcmp(argv[1],"confirm-read-only") || !local(sh) ||
    !diagnostics_idle() || battery_service_claimed() || battery_result.attempted || battery_restore.attempted || !atomic_cas(&command_busy,0,1))return -EPERM;
 uint32_t usb_generation=(uint32_t)atomic_get(&usb_epoch);
 if(!battery_control_lock()){atomic_clear(&command_busy);return -EBUSY;}
 if(!diagnostics_idle() || pendant_button_read()!=0 || mic_commands_reserve_external()){
  battery_control_unlock();
  atomic_clear(&command_busy);return -EBUSY;
 }
 int rc=battery_probe_run(&battery_result,battery_allowed,&usb_generation);
 if(battery_result.stopped && battery_result.released && !battery_result.fault)mic_commands_release_external();
 else atomic_set(&faulted,1); /* Retain external reservation; no recovery/reset. */
 battery_control_unlock();
 atomic_clear(&command_busy);
 (void)battery_status_command(sh,1,NULL);return rc;
}
static int battery_restore_status(const struct shell *sh,size_t argc,char **argv)
{
 ARG_UNUSED(argv);if(argc!=1 || sh!=shell_backend_uart_get_ptr())return -EPERM;
 if(atomic_get(&command_busy))return -EBUSY;
 const struct battery_restore_result *r=&battery_restore;
 shell_print(sh,"BATTERY_RESTORE rc=%d attempted=%u phase=%u transfers=%u writes=%u stopped=%u released=%u fault=%u complete=%u elapsed_ms=%u state_changed=%u sign_changed=%u verified=%u mutated=%u",
  r->rc,r->attempted,r->phase,r->transfers,r->writes,r->stopped,r->released,r->fault,
  r->complete,r->elapsed_ms,r->state_changed,r->sign_changed,r->verified,r->mutated);
 shell_print(sh,"BATTERY_GAUGE type=%u fw=%u chem_before=%u chem_after=%u status_before=%u status_after=%u flags_before=%u flags_after=%u mv_before=%u mv_after=%u soc=%u current_raw=%u temp_raw=%u",
  r->device_type,r->fw_version,r->chem_before,r->chem_after,r->status_before,r->status_after,
  r->flags_before,r->flags_after,r->voltage_before,r->voltage_after,r->soc,r->current,r->temp);
 for(unsigned i=0;i<32;++i)shell_print(sh,"BATTERY_CONFIG i=%u state_before=%u state_after=%u gain_before=%u gain_after=%u",
  i,r->state_before[i],r->state_after[i],r->gain_before[i],r->gain_after[i]);
 return 0;
}
static int battery_restore_command(const struct shell *sh,size_t argc,char **argv)
{
 if(argc!=2 || strcmp(argv[1],"confirm-factory-gauge") || !local(sh) ||
    !diagnostics_idle() || battery_service_claimed() || battery_result.attempted || battery_restore.attempted ||
    !atomic_cas(&command_busy,0,1))return -EPERM;
 uint32_t usb_generation=(uint32_t)atomic_get(&usb_epoch);
 if(!battery_control_lock()){atomic_clear(&command_busy);return -EBUSY;}
 if(!diagnostics_idle() || pendant_button_read()!=0 || mic_commands_reserve_external()){
  battery_control_unlock();atomic_clear(&command_busy);return -EBUSY;
 }
 int rc=battery_restore_run(&battery_restore,battery_allowed,&usb_generation);
 if(battery_restore.stopped && battery_restore.released && !battery_restore.fault)mic_commands_release_external();
 else atomic_set(&faulted,1);
 battery_control_unlock();atomic_clear(&command_busy);
 (void)battery_restore_status(sh,1,NULL);return rc;
}
