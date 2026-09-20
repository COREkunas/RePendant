/* SPDX-License-Identifier: Apache-2.0
 * Direct factory-geometry Dhara trial. Synthetic data only, no heap or audio. */
#include "dhara_trial.h"
#include <string.h>
static struct dhara_trial *active_trial;
int dt_owns(const struct dhara_nand *n){return active_trial&&n==&active_trial->nand;}
static int failure(struct dhara_trial *t,int rc,dhara_error_t *err)
{t->fault=1;if(!t->result.rc)t->result.rc=rc;dhara_set_error(err,DHARA_E_ECC);return -1;}
static int gate(struct dhara_trial *t,uint32_t page,dhara_error_t *err)
{
 if(t->fault||page>=DT_BLOCKS*64U||t->phy.port.now_ms(t->phy.port.user)>=t->deadline)
  return failure(t,-101,err);
 t->result.last_row=DT_FIRST_BLOCK*64U+page;return 0;
}
static int physical(struct dhara_trial *t,int rc,dhara_error_t *err)
{if(rc){t->result.phy_rc=rc;return failure(t,-102,err);}return 0;}
int dt_is_bad(const struct dhara_nand *n,dhara_block_t b)
{struct dhara_trial *t=active_trial;return !dt_owns(n)||b>=DT_BLOCKS||gate(t,b*64U,NULL)||((t->result.bad_mask>>b)&1U);}
void dt_mark_bad(const struct dhara_nand *n,dhara_block_t b)
{(void)b;if(dt_owns(n))failure(active_trial,-103,NULL);}
int dt_erase(const struct dhara_nand *n,dhara_block_t b,dhara_error_t *err)
{
 if(!dt_owns(n))return -1;
 struct dhara_trial *t=active_trial;
 if(b>=DT_BLOCKS||!t->writable||dt_is_bad(n,b))return failure(t,-104,err);
 if(physical(t,nand_owned_phy_erase(&t->phy,DT_FIRST_BLOCK+b,t->deadline),err))return -1;
 ++t->result.erases;return 0;
}
int dt_prog(const struct dhara_nand *n,dhara_page_t p,const uint8_t *data,dhara_error_t *err)
{
 if(!dt_owns(n))return -1;
 struct dhara_trial *t=active_trial;
 if(gate(t,p,err)||!t->writable||!data||dt_is_bad(n,p/64U))return failure(t,-105,err);
 if(physical(t,nand_owned_phy_program_checked(&t->phy,DT_FIRST_BLOCK*64U+p,data,t->deadline),err))return -1;
 ++t->result.programs;return 0;
}
int dt_is_free(const struct dhara_nand *n,dhara_page_t p)
{
 if(!dt_owns(n))return 0;
 struct dhara_trial *t=active_trial;
 if(gate(t,p,NULL)||dt_is_bad(n,p/64U)||physical(t,nand_owned_phy_raw(&t->phy,DT_FIRST_BLOCK*64U+p,t->scratch,t->deadline),NULL))return 0;
 for(unsigned i=0;i<sizeof(t->scratch);++i)if(t->scratch[i]!=0xff)return 0;
 return 1;
}
int dt_read(const struct dhara_nand *n,dhara_page_t p,size_t off,size_t len,uint8_t *out,dhara_error_t *err)
{
 if(!dt_owns(n))return -1;
 struct dhara_trial *t=active_trial;uint32_t ecc=0;
 if(gate(t,p,err)||!out||off>4096||len>4096-off)return failure(t,-106,err);
 if(physical(t,nand_owned_phy_read(&t->phy,DT_FIRST_BLOCK*64U+p,t->scratch,&ecc,t->deadline),err))return -1;
 t->result.ecc=ecc;
 if(ecc!=0&&ecc!=1&&ecc!=3&&ecc!=5)return failure(t,-107,err);
 memmove(out,t->scratch+off,len);return 0;
}
int dt_copy(const struct dhara_nand *n,dhara_page_t src,dhara_page_t dst,dhara_error_t *err)
{
 if(!dt_owns(n))return -1;
 struct dhara_trial *t=active_trial;
 if(dt_read(n,src,0,4096,t->scratch,err))return -1;
 return dt_prog(n,dst,t->scratch,err);
}
static uint8_t pattern(uint32_t sector,uint32_t offset,uint32_t generation)
{
 uint32_t x=0x534e4144U^(sector*0x9e3779b9U)^(offset*0x85ebca6bU)^(generation*0xc2b2ae35U);
 x^=x>>16;x*=0x7feb352dU;x^=x>>15;return (uint8_t)(x^(x>>8));
}
static void init_map(struct dhara_trial *t)
{memset(&t->map,0,sizeof(t->map));memset(t->metadata,0,sizeof(t->metadata));dhara_map_init(&t->map,&t->nand,t->metadata,4);}
static int compare(struct dhara_trial *t,dhara_error_t *err)
{
 if(dhara_map_size(&t->map)!=DT_SECTORS)return failure(t,-108,err);
 for(uint32_t s=0;s<DT_SECTORS;++s){
  if(dhara_map_read(&t->map,s,t->page,err)||t->fault)return failure(t,-109,err);
  for(uint32_t i=0;i<4096;++i)if(t->page[i]!=pattern(s,i,s<16?2U:1U))return failure(t,-110,err);
  ++t->result.reads;t->result.checked_bytes+=4096;
 }
 return 0;
}
int dhara_trial_run(struct dhara_trial *t,const struct owned_volume_decoded *v,
 const struct nop_port *p,uint64_t deadline,int verify_only)
{
 if(!t||!v||!p||!p->now_ms||active_trial||t->result.attempted||(verify_only!=0&&verify_only!=1))return -100;
 for(unsigned i=0;i<DT_BLOCKS;++i)if(v->spec.map_blocks[i]!=DT_FIRST_BLOCK+i)return -100;
 t->start=p->now_ms(p->user);if(deadline<=t->start||deadline-t->start>DT_BUDGET_MS)return -100;
 active_trial=t;t->deadline=deadline;t->result.attempted=1;t->result.verify_only=(uint32_t)verify_only;
 t->nand=(struct dhara_nand){12,6,DT_BLOCKS};dhara_error_t err=DHARA_E_NONE;
 int rc=nand_owned_phy_init(&t->phy,v,p);if(rc){failure(t,-111,&err);goto done;}
 t->result.stage=1;if(physical(t,nand_owned_phy_open(&t->phy,deadline),&err))goto done;
 t->result.stage=2;
 for(unsigned b=0;b<DT_BLOCKS;++b)for(unsigned page=0;page<2;++page){
  uint32_t row=(DT_FIRST_BLOCK+b)*64U+page;t->result.last_row=row;
  if(physical(t,nand_owned_phy_raw(&t->phy,row,t->scratch,deadline),&err))goto done;
  if(t->scratch[4096]!=0xff||t->scratch[4097]!=0xff)t->result.bad_mask|=1U<<b;
 }
 unsigned good=0;for(unsigned b=0;b<DT_BLOCKS;++b)good+=!(t->result.bad_mask&(1U<<b));
 if(good<24){failure(t,-112,&err);goto done;}
 init_map(t);t->result.capacity=dhara_map_capacity(&t->map);
 if(t->result.capacity<DT_SECTORS){failure(t,-113,&err);goto done;}
 if(!verify_only){
  t->writable=1;t->result.stage=3;
  for(unsigned b=0;b<DT_BLOCKS;++b)if(!(t->result.bad_mask&(1U<<b))&&dt_erase(&t->nand,b,&err))goto done;
  t->result.stage=4;
  for(unsigned w=0;w<DT_SECTORS+16;++w){uint32_t s=w<DT_SECTORS?w:w-DT_SECTORS;
   for(unsigned i=0;i<4096;++i)t->page[i]=pattern(s,i,w<DT_SECTORS?1U:2U);
   if(dhara_map_write(&t->map,s,t->page,&err)||t->fault){failure(t,-114,NULL);goto done;}++t->result.writes;
  }
  t->result.stage=5;if(dhara_map_sync(&t->map,&err)||t->fault){failure(t,-115,NULL);goto done;}
  t->writable=0;t->result.stage=6;if(compare(t,&err))goto done;
  t->result.stage=7;if(physical(t,nand_owned_phy_close(&t->phy,deadline),&err))goto done;
  memset(t->page,0,sizeof(t->page));memset(t->scratch,0,sizeof(t->scratch));init_map(t);
  t->result.stage=8;if(physical(t,nand_owned_phy_open(&t->phy,deadline),&err))goto done;
 }
 t->result.stage=9;if(dhara_map_resume(&t->map,&err)||t->fault){failure(t,-116,NULL);goto done;}
 t->result.stage=10;if(compare(t,&err))goto done;
 t->result.stage=11;if(physical(t,nand_owned_phy_close(&t->phy,deadline),&err))goto done;
 t->result.released=1;t->result.complete=1;t->result.stage=12;
done:
 t->writable=0;t->result.stopped=t->phy.stopped;t->result.ready=t->phy.ready_known;
 t->result.a0=t->phy.a0;t->result.b0=t->phy.b0;t->result.c0=t->phy.status;t->result.starts=t->phy.starts;
 t->result.phy_line=t->phy.fault_line;t->result.last_opcode=t->phy.last_opcode;t->result.last_row=t->phy.last_row;
 t->result.wel_observed=t->phy.wel_observed;t->result.verified_programs=t->phy.verified_programs;t->result.verify_mismatch=t->phy.verify_mismatch;
 t->result.library_error=(uint32_t)err;t->result.elapsed_ms=p->now_ms(p->user)-t->start;
 memset(t->page,0,sizeof(t->page));memset(t->scratch,0,sizeof(t->scratch));
 return t->result.rc;
}
