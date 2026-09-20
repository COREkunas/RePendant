/* SPDX-License-Identifier: Apache-2.0 */
#include "nand_capacity_probe.h"
#include <string.h>

void nand_capacity_probe_pattern(uint32_t row,uint8_t out[4096])
{
 uint32_t x=0x9e3779b9U^row;
 for(unsigned i=0;i<4096;++i){x^=x<<13;x^=x>>17;x^=x<<5;out[i]=(uint8_t)x;}
 memcpy(out,"OPNDCAP1",8);
 for(unsigned i=0;i<4;++i)out[8+i]=(uint8_t)(row>>(8U*i));
}
static int bad(const struct nand_capacity_probe *c,uint32_t b)
{return (c->bad[b/8U]>>(b%8U))&1U;}
int nand_capacity_probe_run(struct nand_capacity_probe *c,const struct nop_port *p,uint64_t d,int verify)
{
 if(!c||!p||!p->now_ms||c->result.attempted||(verify!=0&&verify!=1))return NOP_ARGUMENT;
 uint64_t start=p->now_ms(p->user);
 if(d<=start||d-start>NCP_BUDGET_MS)return NOP_ARGUMENT;
 struct ncp_result *r=&c->result;r->attempted=1;r->verify_only=(uint32_t)verify;r->stage=1;
 int rc=nand_owned_phy_init_capacity_probe(&c->phy,p);if(rc)goto done;
 rc=nand_owned_phy_open(&c->phy,d);if(rc)goto done;
 r->stage=2;
 /* Factory marks: both first pages, both marker bytes. Never erase marked
  * blocks, never create software bad markers or retry a failed operation. */
 for(uint32_t b=0;b<2048;++b){
  for(uint32_t page=0;page<2;++page){
   rc=nand_owned_phy_raw(&c->phy,b*64U+page,c->scratch,d);if(rc)goto done;
   ++r->markers;
   if(c->scratch[4096]!=255||c->scratch[4097]!=255)c->bad[b/8U]|=(uint8_t)(1U<<(b%8U));
  }
  if(bad(c,b))++r->bad_blocks;
 }
 if(r->bad_blocks>40){rc=NOP_MEDIA;goto done;}
 r->stage=3;
 for(uint32_t b=0,index=0;b<2048;++b){
  if(!nand_capacity_probe_block(b))continue;
  if(bad(c,b)){r->selected_bad|=1U<<index;++index;continue;}
  ++index;
  if(!verify){
   rc=nand_owned_phy_erase_checked(&c->phy,b,d);if(rc)goto done;
   ++r->blocks;
   for(uint32_t page=0;page<64;++page){
    uint32_t row=b*64U+page;nand_capacity_probe_pattern(row,c->page);
    rc=nand_owned_phy_program_checked(&c->phy,row,c->page,d);if(rc)goto done;
    ++r->programs;
   }
  }
 }
 r->stage=4;
 /* Separate complete pass AFTER all writes detects cross-block address alias,
  * not merely immediate readback of the most recently programmed page. */
 for(uint32_t b=0;b<2048;++b){
  if(!nand_capacity_probe_block(b)||bad(c,b))continue;
  for(uint32_t page=0;page<64;++page){
   uint32_t row=b*64U+page,ecc=0;
   rc=nand_owned_phy_read(&c->phy,row,c->scratch,&ecc,d);if(rc)goto done;
   ++r->reads;
   if(ecc!=0&&ecc!=1&&ecc!=3&&ecc!=5){rc=NOP_MEDIA;goto done;}
   nand_capacity_probe_pattern(row,c->page);
   for(unsigned i=0;i<4096;++i)if(c->page[i]!=c->scratch[i]){r->mismatch=i+1U;rc=NOP_FAULT;goto done;}
   r->bytes+=4096;r->highest_row=row;
  }
 }
 r->stage=5;rc=nand_owned_phy_close(&c->phy,d);if(rc)goto done;
 r->released=1;r->complete=1;r->stage=6;
done:
 r->rc=rc;r->stopped=c->phy.stopped;r->ready=c->phy.ready_known;
 r->a0=c->phy.a0;r->b0=c->phy.b0;r->c0=c->phy.status;
 r->phy_line=c->phy.fault_line;r->phy_fault=c->phy.fault;r->opcode=c->phy.last_opcode;
 r->verified=c->phy.verified_programs;r->wel=c->phy.wel_observed;
 if(!r->mismatch)r->mismatch=c->phy.verify_mismatch;
 uint64_t end=p->now_ms(p->user);r->elapsed_ms=end>=start?end-start:0;
 if(rc||end>=d||end<start){r->complete=0;if(!rc)rc=r->rc=NOP_FAULT;}
 /* PHY owns permanent independent DMA buffers. These CPU-only buffers never
  * escape or contain data needed to retry an uncertain operation. */
 memset(c->page,0,sizeof(c->page));memset(c->scratch,0,sizeof(c->scratch));
 return rc;
}
