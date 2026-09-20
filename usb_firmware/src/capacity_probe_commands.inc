static int capacity_probe_command(const struct shell *sh,size_t argc,char **argv)
{
 int verify=!strcmp(argv[0],"capacitycheck");
 if(argc!=2||strcmp(argv[1],verify?"confirm-read-only":"erase-fixed-16-blocks")||
  !local(sh)||!diagnostics_idle()||atomic_get(&command_busy)||atomic_get(&volume_initialized)||
  atomic_get(&control_probe_used)||atomic_get(&phy_probe_used)||atomic_get(&preimage_used)||
  atomic_get(&recovery_used)||atomic_get(&trial_used)||capacity_configuration()||
  !atomic_cas(&capacity_used,0,1))return -EPERM;
 struct command cmd={.task=TASK_CAPACITY_PROBE,.mode=(uint32_t)verify,.deadline=now(NULL)+NCP_BUDGET_MS};
 int rc=queue(sh,&cmd);if(rc)capacity_context.result.rc=rc;return rc;
}
static int capacity_probe_status(const struct shell *sh,size_t argc,char **argv)
{
 ARG_UNUSED(argv);int rc=control_snapshot_lock(sh,argc);if(rc)return rc;
 /* The union can contain an unrelated diagnostic until our admission. */
 struct ncp_result empty={0};const struct ncp_result *r=atomic_get(&capacity_used)?&capacity_context.result:&empty;
 shell_print(sh,"NAND_CAPACITY_TEST v=1 used=%u attempted=%u complete=%u verify_only=%u stage=%u rc=%d markers=%u bad_blocks=%u selected_bad=%u blocks=%u programs=%u reads=%u bytes=%u highest_row=%u stopped=%u ready=%u released=%u a0=%u b0=%u c0=%u phy_line=%u phy_fault=%u opcode=%u mismatch=%u verified=%u wel=%u elapsed_ms=%llu",
  (unsigned)atomic_get(&capacity_used),r->attempted,r->complete,r->verify_only,r->stage,r->rc,r->markers,
  r->bad_blocks,r->selected_bad,r->blocks,r->programs,r->reads,r->bytes,r->highest_row,
  r->stopped,r->ready,r->released,r->a0,r->b0,r->c0,r->phy_line,r->phy_fault,r->opcode,r->mismatch,
  r->verified,r->wel,(unsigned long long)r->elapsed_ms);
 atomic_clear(&command_busy);return 0;
}
