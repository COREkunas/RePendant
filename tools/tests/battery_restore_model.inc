/* Model gauge committed/staged memory separately. No hardware. */
static uint8_t model_checksum(void)
{unsigned cs=255;for(unsigned i=0;i<32;++i)cs-=model_stage[i];return (uint8_t)(cs&255U);}
static void model_transfer(void)
{
 unsigned nt=mock_twim.TXD.MAXCNT,nr=mock_twim.RXD.MAXCNT;
 if(nr){
  CHECK(nt==1);uint16_t v=0;
  if(tx[0]==0){
   CHECK(nr==2);
   if(model_cmd==1)v=model_case==1?0x0426:0x0427;
   else if(model_cmd==2)v=model_case==2?0x0100:0x0202;
   else if(model_cmd==8)v=model_case==3?0x1202:0x3230;
   else {CHECK(model_cmd==0);v=(uint16_t)(0x88U|(model_sealed?0x2000U:0));if(model_case==4)v|=0x8000;
    if(model_status_skip)--model_status_skip;
    else {v|=model_status_extra;if(model_cca_reads){v|=0x0800;--model_cca_reads;}}}
  }else if(tx[0]==6){CHECK(nr==2);v=model_flags;if(model_case==5)v|=0x4000;}
  else if(tx[0]==4){CHECK(nr==2);v=model_case==6?3000:4212;}
  else if(tx[0]==0x1c){CHECK(nr==2);v=model_case==11?101:98;}
  else if(tx[0]==0x10){CHECK(nr==2);v=model_current;}
  else if(tx[0]==0x1e){CHECK(nr==2);v=2981;}
  else if(tx[0]==0x40){CHECK(nr==32&&model_cfg&&!model_sealed);memcpy(rx,model_stage,32);return;}
  else if(tx[0]==0x60){CHECK(nr==1&&model_cfg&&!model_sealed);rx[0]=model_checksum();if(model_case==7)rx[0]^=1;return;}
  else CHECK(0);
  rx[0]=(uint8_t)v;rx[1]=(uint8_t)(v>>8);return;
 }
 CHECK(nt>=2);
 if(tx[0]==0){
  CHECK(nt==3);model_cmd=(uint16_t)(tx[1]|((uint16_t)tx[2]<<8));
  if(model_cmd==0x8000){CHECK(model_sealed);if(++model_key==2)model_sealed=0;}
  else if(model_cmd==0x13){CHECK(!model_sealed);model_cfg=1;model_flags|=0x10;}
  else if(model_cmd==0x42){CHECK(model_cfg&&!model_sealed);model_cfg=0;model_flags&=0xffcfU;if(model_case==9)model_flags|=0x20;}
  else if(model_cmd==0x20){CHECK(!model_cfg);if(model_case!=10)model_sealed=1;}
  else CHECK(model_cmd==0||model_cmd==1||model_cmd==2||model_cmd==8);
  return;
 }
 CHECK(model_cfg&&!model_sealed);
 if(tx[0]==0x61){CHECK(nt==2&&tx[1]==0);}
 else if(tx[0]==0x3e){CHECK(nt==2&&(tx[1]==82||tx[1]==105));model_subclass=tx[1];}
 else if(tx[0]==0x3f){CHECK(nt==2&&!tx[1]);memcpy(model_stage,model_subclass==82?model_state:model_gain,32);}
 else if(tx[0]==0x40){CHECK(nt==33&&model_subclass==82);memcpy(model_stage,tx+1,32);}
 else if(tx[0]==0x45){CHECK(nt==2&&model_subclass==105);CHECK(tx[1]==(model_stage[5]&0x7f));model_stage[5]=tx[1];}
 else if(tx[0]==0x60){CHECK(nt==2&&tx[1]==model_checksum());
  if(model_case!=8)memcpy(model_subclass==82?model_state:model_gain,model_stage,32);
 }else CHECK(0);
}
static void setup_restore(unsigned c)
{
 reset();restore_mode=1;model_case=c;transfer_delay=10;model_key=model_cfg=model_subclass=0;
 model_sealed=1;model_flags=0x0168;model_current=(uint16_t)-3;
 for(unsigned i=0;i<32;++i){model_state[i]=(uint8_t)(i*5U);model_gain[i]=(uint8_t)(i*3U);}
 model_gain[5]|=0x80;
 if(c==12){model_sealed=0;model_gain[5]&=0x7f;restore_word(model_state,6,245);
  restore_word(model_state,8,906);restore_word(model_state,10,3000);restore_word(model_state,21,54);}
}
static void restore_tests(void)
{
 struct battery_restore_result r,old;uint8_t bs[32],bg[32];unsigned total;
 setup_restore(0);memcpy(bs,model_state,32);memcpy(bg,model_gain,32);
 CHECK(!battery_restore_run(&r,allowed,&mock_twim));total=starts;
 CHECK(r.complete&&r.phase==9&&r.verified==3&&r.state_changed&&r.sign_changed&&r.mutated);
 CHECK(r.transfers==starts&&r.writes>0&&r.elapsed_ms<170000&&r.stopped&&r.released&&!r.fault&&idle());
 CHECK(model_sealed&&!model_cfg&&model_flags==0x148&&r.soc==98&&r.current==(uint16_t)-3);
 CHECK(!memcmp(bs,r.state_before,32)&&!memcmp(bg,r.gain_before,32));
 restore_word(bs,6,245);restore_word(bs,8,906);restore_word(bs,10,3000);restore_word(bs,21,54);bg[5]&=0x7f;
 CHECK(!memcmp(bs,model_state,32)&&!memcmp(bg,model_gain,32));
 CHECK(model_state[6]==0&&model_state[7]==245&&model_state[8]==3&&model_state[9]==0x8a&&
  model_state[10]==0x0b&&model_state[11]==0xb8&&model_state[21]==0&&model_state[22]==54);
 old=r;CHECK(battery_restore_run(&r,allowed,&mock_twim)==-EALREADY&&!memcmp(&old,&r,sizeof(r)));
 struct battery_probe_result p;CHECK(battery_probe_run(&p,allowed,&mock_twim)==-EALREADY);
 for(unsigned c=1;c<=12;++c){
  setup_restore(c);int rc=battery_restore_run(&r,allowed,&mock_twim);
  if(c==12){CHECK(!rc&&r.complete&&!r.sign_changed&&!r.state_changed&&r.verified==3&&model_sealed);}
  else {CHECK(rc&&!r.complete&&r.stopped&&r.released);CHECK(c<=6?(!r.mutated&&!r.fault):(r.mutated&&r.fault));}
 }
 /* Every wire transfer can fail or lose USB authorization; no retry/next I/O,
  * no COMPLETE result, and partial mutation remains explicitly fenced. */
 for(unsigned n=1;n<=total;++n){
  setup_restore(0);failure=NACK;fail_at=n;
  CHECK(battery_restore_run(&r,allowed,&mock_twim)<0&&!r.complete&&starts==n&&r.stopped&&r.released);
  CHECK(r.mutated?r.fault:!r.fault);
  setup_restore(0);cancel_at=n;
  CHECK(battery_restore_run(&r,allowed,&mock_twim)<0&&!r.complete&&starts==n-1&&r.released);
  CHECK(r.mutated?r.fault:!r.fault);
 }
 for(unsigned e=NEVER_STOP;e<=STUCK_CLOCK;++e){
  setup_restore(0);failure=e;if(e==SHORT_READ)fail_at=2;
  CHECK(battery_restore_run(&r,allowed,&mock_twim)<0&&!r.complete&&starts<=fail_at);
  if(e==NEVER_STOP)CHECK(r.fault&&!r.stopped&&!r.released&&mock_twim.TXD.PTR==(uintptr_t)tx);
 }
 setup_restore(0);mock_twim.SUBSCRIBE_STOP=1;
 CHECK(battery_restore_run(&r,allowed,&mock_twim)<0&&r.fault&&!r.released&&!starts&&!writes);
 setup_restore(0);time_us=UINT32_MAX-1500000ULL;
 CHECK(!battery_restore_run(&r,allowed,&mock_twim)&&r.complete);
}
