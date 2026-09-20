/* Single-core IRQ-protected power authority; no I/O or recording authority.
 * Only the runtime command actor changes the admitted work deadline. */
static int sync_power_check(void)
{
 unsigned key=irq_lock();struct bp_sample s=watch_sample;uint64_t t=now(NULL);
 if(atomic_get(&watch_done)||atomic_get(&watch_cancel))s.valid=0;
 int was_draining=sync_power_lease.draining;
 int ok=atomic_get(&sync_power_owned)&&bp_check(&sync_power_lease,&s,t);
 if(ok&&!was_draining&&sync_power_lease.draining){
  /* The initial catalog can take longer than a normal30s request. Finish
   * ONLY its already-fixed deadline plus cleanup; never extend on recovery. */
  uint64_t until=sync_power_work_until>t?sync_power_work_until:t;
  if(until>UINT64_MAX-BP_SAVE_MS)ok=0;
  else sync_power_lease.drain_until=until+BP_SAVE_MS;
 }
 irq_unlock(key);return ok;
}
static int sync_power_ready(void)
{
 unsigned key=irq_lock();int ok=sync_power_check()&&!sync_power_lease.draining&&
  !atomic_get(&watch_done)&&!atomic_get(&watch_cancel)&&bp_continue(&watch_sample,now(NULL));
 irq_unlock(key);return ok;
}
static int sync_power_begin(void)
{
 unsigned key=irq_lock();int ok=!atomic_get(&sync_power_owned)&&portable_admit()&&
  bp_begin(&sync_power_lease,&watch_sample,now(NULL));
 if(ok){atomic_set(&sync_power_owned,1);sync_power_work_until=now(NULL);}
 irq_unlock(key);return ok;
}
static void sync_power_end(void)
{
 if(atomic_get(&leased)||atomic_get(&recording)||atomic_get(&storage_running))fatal(NULL,-EPERM);
 unsigned key=irq_lock();atomic_clear(&sync_power_owned);
 memset(&sync_power_lease,0,sizeof(sync_power_lease));sync_power_work_until=0;irq_unlock(key);
}
