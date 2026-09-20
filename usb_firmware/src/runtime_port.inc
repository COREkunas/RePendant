/* Candidate-only inclusion AFTER recovery_commands.inc; not registered in
 * production. Register recoverpermitstatus once with argc1. hp_runtime_bind()
 * is exclusive boot setup after cached rcfg initialization, never automatic job.
 * This adapter TRUSTS the closed host protocol's fsync barrier; tokens do not
 * prove it. No new authority is derived from a settings-absence observation. */
#include "host_permit.h"
static struct host_permit hp_runtime_owner;
static uint32_t hp_runtime_bound;
static int hp_runtime_snapshot(void *u,struct hp_view *out)
{
 ARG_UNUSED(u);struct recording_configuration observed_cfg;
 int allowed=RR_EVIDENCE_ADMITTED&&diagnostics_idle()&&!atomic_get(&faulted)&&
  !atomic_get(&control_probe_used)&&!atomic_get(&phy_probe_used)&&!atomic_get(&preimage_used)&&
  recovery_one_bond()&&!recovery_config(&observed_cfg,0);
 unsigned key=irq_lock();
 *out=(struct hp_view){.now=now(NULL),.epoch=(uint32_t)atomic_get(&usb_epoch),
  .allowed=(uint32_t)(allowed&&atomic_get(&usb_configured)),
  .command_busy=(uint32_t)atomic_get(&command_busy),
  .task=atomic_get(&recovery_running)?active.task:0,
  .running=(uint32_t)atomic_get(&recovery_running),.used=(uint32_t)atomic_get(&recovery_used)};
 irq_unlock(key);return 0;
}
static int hp_runtime_bind(void)
{
 if(hp_runtime_bound)return -EPERM;
 hp_runtime_bound=1;
 struct hp_binding binding;memcpy(binding.device,rr_evidence.expected_device,16);
 memcpy(binding.transaction,rr_proof.transaction,16);memcpy(binding.record_digest,rr_expected_record_digest,32);
 const struct hp_port port={NULL,hp_runtime_snapshot};
 if(hp_init(&hp_runtime_owner,&binding,&port))return -EPERM;
 const struct rr_external_permit permit={&hp_runtime_owner,hp_consume};
 return recording_runtime_recovery_permit_bind(&permit);
}
static int hp_runtime_status(const struct shell *sh,size_t argc,char **argv)
{
 ARG_UNUSED(argv);
 if(argc!=1||!local(sh)||!hp_runtime_bound||!diagnostics_idle()||
  atomic_get(&volume_initialized)||!atomic_cas(&command_busy,0,1))return -EPERM;
 struct hp_offer offer;int rc=hp_get_offer(&hp_runtime_owner,&offer);
 if(!rc){char device[33],tx[33],digest[65];hex(offer.binding.device,16,device);
  hex(offer.binding.transaction,16,tx);hex(offer.binding.record_digest,32,digest);
  shell_print(sh,"RECORDER_PERMIT v=1 epoch=%u issued_ms=%llu expires_ms=%llu device=%s transaction=%s record=%s host_durability_proven=0",
   offer.epoch,(unsigned long long)offer.issued,(unsigned long long)offer.expires,device,tx,digest);}
 atomic_clear(&command_busy);return rc;
}
