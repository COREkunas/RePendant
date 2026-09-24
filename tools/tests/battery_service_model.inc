static void service_setup(void){setup_restore(0);model_flags=0x01c8;}
static int repair_refused(void *user){CHECK(user==&mock_twim);return 0;}
static void calibration_settle_tests(void)
{
 struct battery_restore_result r;struct bp_sample s;
 for(unsigned count=1;count<16;++count){
  service_setup();model_cca_reads=count;memset(&r,0,sizeof(r));
  CHECK(!battery_service_init_recover(allowed,allowed,&mock_twim,&r));
  CHECK(starts==9+2*count&&!r.attempted&&service_ready&&battery_service_released());
  CHECK(!battery_service_read(&s,allowed,&mock_twim)&&s.valid);
 }
 /* The user's unsealed/POR cold-boot shape must settle before it defers for
  * USB, and then revalidate and restore once. No default SOC admission. */
 setup_restore(0);model_sealed=0;model_cca_reads=3;memset(&r,0,sizeof(r));
 CHECK(battery_service_init_recover(allowed,repair_refused,&mock_twim,&r)==-EAGAIN);
 CHECK(service_deferred&&!r.attempted&&starts==15&&!service_ready);
 CHECK(!battery_service_resume_recover(allowed,allowed,&mock_twim,&r)&&r.complete);
 /* Calibration can start between service and repair, after unseal or after
  * soft reset. No config step proceeds until a clean status is observed. */
 for(unsigned skip=0;skip<=3;++skip){
  setup_restore(0);model_status_skip=skip;model_cca_reads=2;memset(&r,0,sizeof(r));
  CHECK(!battery_service_init_recover(allowed,allowed,&mock_twim,&r)&&r.complete&&service_ready);
 }
 service_setup();model_cca_reads=100;memset(&r,0,sizeof(r));
 CHECK(battery_service_init_recover(allowed,allowed,&mock_twim,&r)==-ETIMEDOUT);
 CHECK(starts==38&&!r.attempted&&!service_ready&&!service_deferred&&battery_service_released());
 unsigned total=starts;
 CHECK(battery_service_resume_recover(allowed,allowed,&mock_twim,&r)==-EPERM&&starts==total);
 for(unsigned bit=0;bit<16;++bit)if((0xd461U>>bit)&1U){
  service_setup();model_status_extra=(uint16_t)(1U<<bit);model_cca_reads=3;memset(&r,0,sizeof(r));
  CHECK(battery_service_init_recover(allowed,allowed,&mock_twim,&r)==-EPERM);
  CHECK(starts==8&&!service_ready&&!service_deferred&&!r.attempted);
 }
 for(unsigned fail=9;fail<=38;++fail){
  service_setup();model_cca_reads=100;failure=NACK;fail_at=fail;memset(&r,0,sizeof(r));
  CHECK(battery_service_init_recover(allowed,allowed,&mock_twim,&r)<0&&starts==fail);
  CHECK(!service_ready&&!service_deferred&&!r.attempted);
  service_setup();model_cca_reads=100;cancel_at=fail;memset(&r,0,sizeof(r));
  CHECK(battery_service_init_recover(allowed,allowed,&mock_twim,&r)<0&&starts==fail-1);
  CHECK(!service_ready&&!service_deferred&&!r.attempted);
 }
}
static void service_recovery_tests(void)
{
 struct battery_restore_result r;struct battery_service_init_result d;struct bp_sample s;
 service_setup();memset(&r,0,sizeof(r));
 CHECK(!battery_service_init_recover(allowed,allowed,&mock_twim,&r)&&starts==9&&!r.attempted&&service_ready);
 CHECK(!battery_service_read(&s,allowed,&mock_twim)&&s.valid);
 for(unsigned sealed=0;sealed<=1;++sealed){
  setup_restore(0);model_sealed=sealed;memset(&r,0,sizeof(r));
  CHECK(!battery_service_init_recover(allowed,allowed,&mock_twim,&r)&&service_ready);
  CHECK(r.attempted&&r.complete&&r.mutated&&r.verified==3&&r.phase==9&&r.stopped&&r.released&&!r.fault);
  CHECK(model_sealed&&!(model_flags&0x30)&&!model_cfg&&battery_service_released());
  battery_service_init_status(&d);CHECK(d.complete&&d.stage==8&&d.valid==31&&(d.flags&0x20));
  CHECK(!battery_service_read(&s,allowed,&mock_twim)&&s.valid&&s.sequence==1);
 }
 setup_restore(0);model_sealed=0;model_flags&=~0x20U;memset(&r,0,sizeof(r));
 CHECK(battery_service_init_recover(allowed,allowed,&mock_twim,&r)==-EPERM&&!service_ready&&!r.attempted&&starts==9);
 setup_restore(0);memset(&r,0,sizeof(r));
 CHECK(battery_service_init_recover(allowed,repair_refused,&mock_twim,&r)==-EAGAIN&&!r.attempted&&starts==9);
 CHECK(service_deferred&&!service_ready&&battery_service_released());
 unsigned deferred_starts=starts;
 CHECK(battery_service_init_recover(allowed,allowed,&mock_twim,&r)==-EPERM&&starts==deferred_starts);
 CHECK(!battery_service_resume_recover(allowed,allowed,&mock_twim,&r)&&r.complete&&service_ready&&!service_deferred);
 deferred_starts=starts;
 CHECK(battery_service_resume_recover(allowed,allowed,&mock_twim,&r)==-EPERM&&starts==deferred_starts);
 /* Revalidate after arbitrary waiting: a changed identity or transport fault
  * must refuse recovery and consume the deferred continuation. */
 for(unsigned c=1;c<=5;++c){
  setup_restore(0);memset(&r,0,sizeof(r));
  CHECK(battery_service_init_recover(allowed,repair_refused,&mock_twim,&r)==-EAGAIN);
  model_case=c;
  CHECK(battery_service_resume_recover(allowed,allowed,&mock_twim,&r)==-EPERM&&!r.attempted&&!service_deferred);
  deferred_starts=starts;
  CHECK(battery_service_resume_recover(allowed,allowed,&mock_twim,&r)==-EPERM&&starts==deferred_starts);
 }
 setup_restore(0);memset(&r,0,sizeof(r));
 CHECK(battery_service_init_recover(allowed,repair_refused,&mock_twim,&r)==-EAGAIN);
 failure=NACK;fail_at=10;
 CHECK(battery_service_resume_recover(allowed,allowed,&mock_twim,&r)<0&&!service_deferred&&!r.attempted);
 CHECK(battery_service_resume_recover(allowed,allowed,&mock_twim,&r)==-EPERM&&starts==10);
 for(unsigned n=1;n<=5;++n){setup_restore(n);memset(&r,0,sizeof(r));
  CHECK(battery_service_init_recover(allowed,allowed,&mock_twim,&r)<0&&!r.attempted&&!service_ready);}
 setup_restore(6);memset(&r,0,sizeof(r));
 CHECK(battery_service_init_recover(allowed,allowed,&mock_twim,&r)<0&&r.attempted&&!r.mutated&&!service_ready);
 setup_restore(0);model_sealed=0;memset(&r,0,sizeof(r));
 CHECK(!battery_service_init_recover(allowed,allowed,&mock_twim,&r));unsigned total=starts;
 for(unsigned n=1;n<=total;++n){
  setup_restore(0);model_sealed=0;memset(&r,0,sizeof(r));failure=NACK;fail_at=n;
  CHECK(battery_service_init_recover(allowed,allowed,&mock_twim,&r)<0&&!service_ready&&starts==n);
  CHECK(!r.complete);if(r.mutated)CHECK(fault&&!battery_service_released());
  unsigned old=starts;CHECK(battery_service_read(&s,allowed,&mock_twim)<0&&starts==old);
  CHECK(battery_service_init_recover(allowed,allowed,&mock_twim,&r)<0&&starts==old);
  CHECK(battery_service_resume_recover(allowed,allowed,&mock_twim,&r)<0&&starts==old);
  setup_restore(0);model_sealed=0;memset(&r,0,sizeof(r));cancel_at=n;
  CHECK(battery_service_init_recover(allowed,allowed,&mock_twim,&r)<0&&!service_ready&&starts==n-1);
  CHECK(!r.complete);if(r.mutated)CHECK(fault&&!battery_service_released());
 }
}
static void service_tests(void)
{
 struct bp_sample s;struct battery_restore_result r;struct battery_probe_result p;
 struct battery_service_init_result d,saved;
 uint8_t state[32],gain[32];
 service_setup();memcpy(state,model_state,32);memcpy(gain,model_gain,32);
 CHECK(!battery_service_claimed()&&battery_service_released());
 CHECK(!battery_service_init(allowed,&mock_twim)&&starts==9&&battery_service_claimed());
 battery_service_init_status(&d);saved=d;
 CHECK(d.attempted&&d.complete&&d.stage==8&&d.valid==31&&d.rc==0&&d.transfers==9);
 CHECK(d.stopped&&d.released&&!d.fault&&d.elapsed_ms>=18000&&d.elapsed_ms<18100);
 CHECK(d.type==0x427&&d.fw==0x202&&d.chem==0x3230&&d.status==0x2088&&d.flags==0x01c8);
 unsigned cached_starts=starts,cached_writes=writes;uint64_t cached_time=time_us;
 battery_service_init_status(NULL);battery_service_init_status(&d);
 CHECK(!memcmp(&saved,&d,sizeof(d))&&starts==cached_starts&&writes==cached_writes&&time_us==cached_time);
 CHECK(!battery_service_read(&s,allowed,&mock_twim)&&s.valid&&s.sequence==1&&s.mv==4212&&s.soc==98&&s.flags==0x01c8);
 CHECK(s.temp==2981&&bp_fresh(&s,(uint64_t)k_uptime_get())&&bp_start(&s,(uint64_t)k_uptime_get()));
 CHECK((uint64_t)k_uptime_get()-s.at>=5000&&(uint64_t)k_uptime_get()-s.at<5100);
 CHECK(s.current_ma==-3&&!bp_charging(&s,(uint64_t)k_uptime_get()));
 /* More than an hour of real production protocol operations; byte arrays
  * and sealed/config state may never change. No command after init is a write. */
 for(unsigned i=2;i<=601;++i){
  CHECK(!battery_service_read(&s,allowed,&mock_twim)&&s.sequence==i&&s.valid&&battery_service_released());
  CHECK(model_cmd==0&&model_sealed==1&&!model_cfg&&!model_key);
  CHECK(!memcmp(state,model_state,32)&&!memcmp(gain,model_gain,32));
 }
 unsigned total=starts;
 CHECK(battery_service_init(allowed,&mock_twim)<0&&starts==total);
 battery_service_init_status(&d);CHECK(!memcmp(&saved,&d,sizeof(d)));
 CHECK(battery_restore_run(&r,allowed,&mock_twim)==-EALREADY&&starts==total);
 CHECK(battery_probe_run(&p,allowed,&mock_twim)==-EALREADY&&starts==total);
 /* Every init and sample transfer failing: no automatic retry or reuse. */
 for(unsigned i=1;i<=15;++i){
  service_setup();failure=NACK;fail_at=i;int rc=battery_service_init(allowed,&mock_twim);
  if(i<=9)CHECK(rc<0&&!service_ready);else CHECK(!rc&&battery_service_read(&s,allowed,&mock_twim)<0&&!s.valid);
  total=starts;CHECK(total==i&&battery_service_released());
  battery_service_init_status(&d);
  if(i<=9){CHECK(d.stage==3+(i-1)/2&&!d.complete&&d.rc==rc&&d.transfers==i);
   CHECK(d.valid==((1U<<((i-1)/2))-1U));}
  else CHECK(d.complete&&d.stage==8&&d.rc==0&&d.valid==31&&d.transfers==9);
  CHECK(battery_service_read(&s,allowed,&mock_twim)<0&&starts==total);
  CHECK(battery_service_init(allowed,&mock_twim)<0&&starts==total);
 }
 for(unsigned i=1;i<=15;++i){
  service_setup();cancel_at=i;int rc=battery_service_init(allowed,&mock_twim);
  if(i<=9)CHECK(rc<0);else CHECK(!rc&&battery_service_read(&s,allowed,&mock_twim)<0);
  CHECK(starts==i-1&&battery_service_released());
 }
 for(unsigned i=1;i<=5;++i){service_setup();model_case=i;
  CHECK(battery_service_init(allowed,&mock_twim)<0&&!service_ready&&battery_service_released());
  battery_service_init_status(&d);CHECK(d.stage==i+2&&d.valid==(i==4?7U:((1U<<i)-1U))&&d.rc==-EPERM&&!d.complete);
  CHECK(d.transfers==(i==5?9:i*2));}
 service_setup();model_flags|=0x20;CHECK(battery_service_init(allowed,&mock_twim)<0&&!service_ready);
 battery_service_init_status(&d);CHECK(d.stage==7&&d.valid==31&&(d.flags&0x20)&&d.rc==-EPERM);
 service_setup();model_sealed=0;CHECK(battery_service_init(allowed,&mock_twim)<0&&!service_ready);
 battery_service_init_status(&d);CHECK(d.stage==6&&d.valid==15&&!(d.status&0x2000)&&d.rc==-EPERM);
 service_setup();mock_twim.SUBSCRIBE_STOP=1;
 CHECK(battery_service_init(allowed,&mock_twim)<0&&!battery_service_released()&&!writes&&!starts);
 service_setup();failure=NEVER_STOP;CHECK(battery_service_init(allowed,&mock_twim)<0&&!battery_service_released());
 CHECK(mock_twim.TXD.PTR==(uintptr_t)tx&&!stopped&&!released);
 for(unsigned i=10;i<=14;++i){service_setup();CHECK(!battery_service_init(allowed,&mock_twim));
  failure=NEVER_STOP;fail_at=i;CHECK(battery_service_read(&s,allowed,&mock_twim)<0&&!s.valid&&!battery_service_released());}
 service_setup();CHECK(!battery_service_init(allowed,&mock_twim));model_flags|=0x8000;
 CHECK(battery_service_read(&s,allowed,&mock_twim)<0&&!s.valid&&(s.flags&0x8000));
 service_setup();CHECK(!battery_service_init(allowed,&mock_twim));model_case=11;
 CHECK(battery_service_read(&s,allowed,&mock_twim)<0&&!s.valid);
 service_setup();time_us=UINT32_MAX-1500000ULL;CHECK(!battery_service_init(allowed,&mock_twim));
 CHECK(!battery_service_read(&s,allowed,&mock_twim)&&bp_start(&s,(uint64_t)k_uptime_get()));
 service_setup();CHECK(!battery_service_init(allowed,&mock_twim));preemption_us=30000;
 CHECK(battery_service_read(&s,allowed,&mock_twim)==-ETIMEDOUT&&!s.valid&&battery_service_released());
 CHECK(mock_twim.events[NRF_TWIM_EVENT_STOPPED]&&!fault&&!service_ready);
 /* Pure policy is advisory in this release, not wired to capture admission. */
 /* Sign, charge/discharge/full flags, stale and minimum-current noise cases. */
 for(int current=-32768;current<=32767;current+=1){
  s=(struct bp_sample){.at=100,.mv=4200,.temp=2981,.soc=80,.flags=8,.valid=1,.sequence=1,.current_ma=(int16_t)current};
  CHECK(bp_charging(&s,100)==(current>=5&&current<=1000));
 }
 service_setup();CHECK(!battery_service_init(allowed,&mock_twim));model_current=120;
 CHECK(!battery_service_read(&s,allowed,&mock_twim)&&s.current_ma==120&&bp_charging(&s,(uint64_t)k_uptime_get()));
 CHECK(!bp_charging(&s,s.at+BP_FRESH_MS+1));s.flags|=1;CHECK(!bp_charging(&s,s.at));
 s.flags=0x208;CHECK(!bp_charging(&s,s.at));s.flags=0x108;s.current_ma=0;CHECK(!bp_charging(&s,s.at));
 reset();s=(struct bp_sample){.at=100,.mv=4200,.temp=2981,.soc=80,.flags=8,.valid=1,.sequence=1};
 for(unsigned i=0;i<=65535;++i){s.flags=(uint16_t)i;CHECK(bp_start(&s,100)==((i&8)&&!(i&0xfc30)));}s.flags=8;
 CHECK(!bp_fresh(&s,99)&&bp_fresh(&s,20100)&&!bp_fresh(&s,20101));
 struct bp_lease l={0};CHECK(bp_begin(&l,&s,100)&&!bp_begin(&l,&s,101));
 s.soc=19;CHECK(bp_check(&l,&s,200)&&l.draining&&l.drain_until==32200);
 s.soc=80;s.at=30000;CHECK(bp_check(&l,&s,32199)&&l.drain_until==32200);
 CHECK(!bp_check(&l,&s,32200));
 l=(struct bp_lease){0};s.at=100;CHECK(bp_begin(&l,&s,100));
 s.valid=0;s.flags=0x8000;CHECK(!bp_check(&l,&s,200));
 service_recovery_tests();
 calibration_settle_tests();
}
