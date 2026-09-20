/* Isolated one-shot diagnostic, before any normal volume/HAL initialization.
 * No enrolled descriptor, persistent setting, internal flash or key mutation. */
static int capacity_configuration(void)
{
 struct recording_configuration cfg;uint8_t device[16];
 if(rcfg_get(&cfg)||rcfg_device_id(device)||cfg.phase!=RCFG_ACTIVE||cfg.revision!=3||
    cfg.fault_banks||memcmp(device,control_probe_device,16)||
    memcmp(cfg.spec.device_id,device,16)||memcmp(cfg.descriptor+352,control_probe_digest,32)||
    cfg.spec.control_blocks[0]!=1057||cfg.spec.control_blocks[1]!=1058)return -EPERM;
 for(unsigned i=0;i<32;++i)if(cfg.spec.map_blocks[i]!=1025U+i)return -EPERM;
 return 0;
}
static int capacity_check(void *u,uint64_t d)
{
 ARG_UNUSED(u);
 return !atomic_get(&capacity_running)||!atomic_get(&capacity_used)||!atomic_get(&command_busy)||
  active.task!=TASK_CAPACITY_PROBE||active.deadline!=d||now(NULL)>=d||
  !atomic_get(&usb_configured)||active.usb_epoch!=(uint32_t)atomic_get(&usb_epoch)||
  atomic_get(&faulted)||atomic_get(&recording)||atomic_get(&storage_running)||atomic_get(&catalog_epoch)||
  atomic_get(&volume_initialized)||pendant_recovery_is_pending()||pendant_ble_pairing_busy()?-EPERM:0;
}
static int capacity_acquire(void *u,uint64_t d)
{
 if(capacity_check(u,d)||atomic_get(&leased)||atomic_get(&capacity_owner)||mic_commands_reserve_external())return -EBUSY;
 lease_usb_epoch=(uint32_t)atomic_get(&usb_epoch);atomic_set(&leased,1);atomic_set(&capacity_owner,1);
 return capacity_check(u,d);
}
static int capacity_release(void *u,uint64_t d)
{
 if(capacity_check(u,d)||!atomic_get(&leased)||!atomic_cas(&capacity_owner,1,0))return -EPERM;
 atomic_clear(&leased);mic_commands_release_external();return 0;
}
static int capacity_authorize(void *u,uint32_t access,uint32_t row,uint64_t d)
{
 return capacity_check(u,d)||!atomic_get(&leased)||!atomic_get(&capacity_owner)||row>=2048U*64U||
  access<NOP_READ||access>NOP_ERASE||
  (access!=NOP_READ&&(active.mode||!nand_capacity_probe_block(row/64U)))?-EPERM:0;
}
static const struct nop_nrf_owner capacity_hal_owner={NULL,capacity_acquire,capacity_release,capacity_authorize};
static void execute_capacity_probe(void)
{
 struct nop_port port;int rc=-EPERM;
 atomic_set(&capacity_running,1);
 if(!capacity_configuration()&&!capacity_check(NULL,active.deadline)&&
    !nand_owned_phy_nrf_bind(&port,&capacity_context.phy,&capacity_hal_owner))
  rc=nand_capacity_probe_run(&capacity_context,&port,active.deadline,(int)active.mode);
 capacity_context.result.rc=rc;
 if(rc){capacity_context.result.complete=0;atomic_set(&faulted,1);}
 atomic_clear(&capacity_running);
 if(active.shell)shell_print(active.shell,"RECORDER_DONE task=14 rc=%d microphone=0",rc);
 memset(&active,0,sizeof(active));atomic_clear(&command_busy);
}
