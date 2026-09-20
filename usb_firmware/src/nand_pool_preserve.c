/* SPDX-License-Identifier: Apache-2.0
 * Explicit read-only preservation of candidates1025..1058, not activation.
 * No caller supplies an opcode, physical address, feature value or write lease.
 * The same34block table will only become a recording volume after complete
 * two-copy preservation, bad-marker checks and separate scoped provisioning.
 */
#include "nand_owned_phy_nrf.h"
#include "mic_commands.h"
#include <errno.h>
#include <string.h>
#include <psa/crypto.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/shell/shell_uart.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/reboot.h>

static struct nand_owned_phy preservation_phy;
static uint8_t raw_first[4352],raw_second[4352];
static atomic_t initialized,active,failed;
static uint32_t selected_row;
static uint64_t physical_deadline;
static struct k_work_q timeout_queue;
static struct k_work_delayable timeout_work;
static struct k_work_sync timeout_sync;
K_THREAD_STACK_DEFINE(pool_timeout_stack,2048);
static const uint32_t pool_candidate_blocks[34] __attribute__((used,retain))={
 1025,1026,1027,1028,1029,1030,1031,1032,1033,1034,1035,1036,1037,1038,1039,1040,1041,
 1042,1043,1044,1045,1046,1047,1048,1049,1050,1051,1052,1053,1054,1055,1056,1057,1058};
int pendant_button_read(void);
static void wipe(void *p,size_t n){volatile uint8_t *b=p;while(n--)*b++=0;}
static void expiry(struct k_work *work)
{ARG_UNUSED(work);if(atomic_get(&active)){atomic_set(&failed,1);sys_reboot(SYS_REBOOT_COLD);}}
static int acquire(void *user,uint64_t deadline)
{ARG_UNUSED(user);
 if(!atomic_get(&active)||deadline!=physical_deadline||atomic_get(&failed)||
    pendant_button_read()!=0||pendant_recovery_is_pending())return -EPERM;
 return mic_commands_reserve_external();}
static int release(void *user,uint64_t deadline)
{ARG_UNUSED(user);if(!atomic_get(&active)||deadline!=physical_deadline||atomic_get(&failed))return -EPERM;
 mic_commands_release_external();return 0;}
static int authorize(void *user,uint32_t access,uint32_t row,uint64_t deadline)
{ARG_UNUSED(user);return atomic_get(&active)&&!atomic_get(&failed)&&access==NOP_READ&&row==selected_row&&
 deadline==physical_deadline&&(uint64_t)k_uptime_get()<deadline&&!pendant_recovery_is_pending()?0:-EPERM;}
static int initialize(void)
{
 if(atomic_get(&initialized))return atomic_get(&initialized)==1?0:-EIO;
 struct owned_volume_decoded candidate={0};
 memcpy(candidate.spec.map_blocks,pool_candidate_blocks,32*sizeof(uint32_t));
 memcpy(candidate.spec.control_blocks,pool_candidate_blocks+32,2*sizeof(uint32_t));
 /* Table-only READ scope. This is deliberately NOT a mounted volume, owner
  * descriptor, writable capability or fake recovery verification. */
 struct nop_port port;const struct nop_nrf_owner owner={NULL,acquire,release,authorize};
 if(nand_owned_phy_nrf_bind(&port,&preservation_phy,&owner)||nand_owned_phy_init(&preservation_phy,&candidate,&port)){
  atomic_set(&initialized,-1);return -EIO;
 }
 k_work_queue_start(&timeout_queue,pool_timeout_stack,K_THREAD_STACK_SIZEOF(pool_timeout_stack),4,NULL);
 k_work_init_delayable(&timeout_work,expiry);atomic_set(&initialized,1);return 0;
}
static int number(const char *s,unsigned limit,unsigned *out)
{unsigned v=0,n=0;if(!s||!*s)return -1;if(s[0]=='0'&&s[1])return -1;
 while(*s){if(*s<'0'||*s>'9'||n++>=3)return -1;v=v*10U+(unsigned)(*s++-'0');}
 if(v>=limit)return -1;
 *out=v;return 0;}
static int token_valid(const char *s)
{if(!s||strlen(s)!=32)return 0;for(unsigned i=0;i<32;++i)if(!((s[i]>='0'&&s[i]<='9')||(s[i]>='a'&&s[i]<='f')))return 0;return 1;}
static void hex(char *out,const uint8_t *in,size_t bytes)
{static const char digits[]="0123456789abcdef";for(size_t i=0;i<bytes;++i){out[i*2]=digits[in[i]>>4U];out[i*2+1]=digits[in[i]&15U];}out[bytes*2]=0;}
static int read_command(const struct shell *sh,size_t argc,char **argv)
{
 unsigned index,page;
 if(sh!=shell_backend_uart_get_ptr()||argc!=4||number(argv[1],34,&index)||number(argv[2],64,&page)||!token_valid(argv[3]))return -EINVAL;
 if(initialize()||atomic_get(&failed)||!atomic_cas(&active,0,1))return -EBUSY;
 selected_row=pool_candidate_blocks[index]*64U+page;physical_deadline=(uint64_t)k_uptime_get()+2000U;
 if(k_work_schedule_for_queue(&timeout_queue,&timeout_work,K_MSEC(2000))<0){atomic_set(&failed,1);return -EIO;}
 uint32_t before=preservation_phy.starts;int rc=nand_owned_phy_open(&preservation_phy,physical_deadline);
 if(!rc)rc=nand_owned_phy_raw(&preservation_phy,selected_row,raw_first,physical_deadline);
 if(!rc)rc=nand_owned_phy_raw(&preservation_phy,selected_row,raw_second,physical_deadline);
 if(!rc&&memcmp(raw_first,raw_second,sizeof(raw_first)))rc=-EIO;
 uint8_t digest[32];size_t written=0;
 if(!rc&&(psa_hash_compute(PSA_ALG_SHA_256,raw_first,sizeof(raw_first),digest,sizeof(digest),&written)!=PSA_SUCCESS||written!=32))rc=-EIO;
 if(!rc)rc=nand_owned_phy_close(&preservation_phy,physical_deadline);
 if((uint64_t)k_uptime_get()>=physical_deadline)rc=-ETIMEDOUT;
 if(rc){atomic_set(&failed,1);/* Keep failed ownership, no automatic retry/reset. */}
 atomic_set(&active,0);(void)k_work_cancel_delayable_sync(&timeout_work,&timeout_sync);
 if(rc){wipe(raw_first,sizeof(raw_first));wipe(raw_second,sizeof(raw_second));
  shell_print(sh,"POOL_ERROR token=%s index=%u page=%u rc=%d fault=1",argv[3],index,page,rc);return rc;}
 char encoded[129],hash[65];hex(hash,digest,32);
 shell_print(sh,"POOL_BEGIN protocol=1 token=%s index=%u block=%u page=%u row=%u bytes=4352 copies=2 sha256=%s",
  argv[3],index,pool_candidate_blocks[index],page,selected_row,hash);
 for(unsigned chunk=0;chunk<68;++chunk){hex(encoded,raw_first+chunk*64U,64);
  shell_print(sh,"POOL_DATA token=%s chunk=%u hex=%s",argv[3],chunk,encoded);}
 shell_print(sh,"POOL_END token=%s starts=%u stopped=1 released=1 matched=1 a0=124 b0=16 array_writes=0 sha256=%s",
  argv[3],preservation_phy.starts-before,hash);
 wipe(raw_first,sizeof(raw_first));wipe(raw_second,sizeof(raw_second));wipe(encoded,sizeof(encoded));return 0;
}
static int state_command(const struct shell *sh,size_t argc,char **argv)
{ARG_UNUSED(argv);if(sh!=shell_backend_uart_get_ptr()||argc!=1)return -EINVAL;
 shell_print(sh,"POOL_STATUS protocol=1 first=1025 blocks=34 rows=64 raw=4352 initialized=%d active=%d fault=%d stopped=%u opened=%u array_writes=0",
  (int)atomic_get(&initialized),(int)atomic_get(&active),(int)atomic_get(&failed),preservation_phy.stopped,preservation_phy.opened);return 0;}
SHELL_STATIC_SUBCMD_SET_CREATE(pool_commands,
 SHELL_CMD_ARG(status,NULL,"Candidate preservation metadata, no hardware.",state_command,1,0),
 SHELL_CMD_ARG(read,NULL,"Read candidate index0..33 page0..63 twice, local hex export. Token32lowerhex.",read_command,4,0),
 SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(pool,&pool_commands,"Read-only candidate pool preservation.",NULL);
