#include "host_permit.h"
#include <stddef.h>
#include <string.h>
#define HP_MAGIC 0x48504d31U
static int separate(const void *a,size_t n,const void *b,size_t m)
{uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;return a&&b&&n<=UINTPTR_MAX-x&&m<=UINTPTR_MAX-y&&(x+n<=y||y+m<=x);}
static int non_sentinel(const uint8_t *p,size_t n)
{unsigned z=0,f=0;for(size_t i=0;i<n;i++){z|=p[i];f|=(unsigned)(p[i]^255U);}return z&&f;}
static int lock(struct host_permit *p)
{unsigned zero=0;return atomic_compare_exchange_strong(&p->gate,&zero,1U);}
static int finish(struct host_permit *p,int rc)
{if(rc)p->fault=1;atomic_store(&p->gate,0);return rc;}
static int view(struct host_permit *p,struct hp_view *v)
{
 memset(v,0,sizeof(*v));
 if(p->port.snapshot(p->port.user,v)||v->allowed!=1||v->command_busy!=1||
    !v->epoch||v->epoch>0x7fffffffU||v->now<p->last)return -1;
 p->last=v->now;return 0;
}
int hp_init(struct host_permit *p,const struct hp_binding *b,const struct hp_port *port)
{
 if(!separate(p,sizeof(*p),b,sizeof(*b))||!separate(p,sizeof(*p),port,sizeof(*port))||
    p->magic||p->fault||!port->snapshot||!non_sentinel(b->device,16)||!non_sentinel(b->transaction,16)||
    !non_sentinel(b->record_digest,32))return -1;
 memset(p,0,sizeof(*p));atomic_init(&p->gate,0);
 if(!atomic_is_lock_free(&p->gate)){p->fault=1;return -1;}
 p->binding=*b;p->port=*port;p->magic=HP_MAGIC;return 0;
}
int hp_get_offer(struct host_permit *p,struct hp_offer *out)
{
 if(!separate(p,sizeof(*p),out,sizeof(*out))||p->magic!=HP_MAGIC||!lock(p))return -1;
 struct hp_view v,w;struct hp_offer offer;
 if(p->fault||p->consumed||view(p,&v)||v.task||v.running||v.used||
    v.now>UINT64_MAX-HP_OFFER_MS)return finish(p,-1);
 offer=p->offered?p->offer:(struct hp_offer){.binding=p->binding,.issued=v.now,.expires=v.now+HP_OFFER_MS,.epoch=v.epoch};
 if(v.epoch!=offer.epoch||v.now>=offer.expires||view(p,&w)||w.task||w.running||w.used||
    w.epoch!=offer.epoch||w.now>=offer.expires)return finish(p,-1);
 p->offer=offer;p->offered=1;*out=offer;return finish(p,0);
}
int hp_consume(void *user,const uint8_t transaction[16],const uint8_t digest[32],uint32_t epoch,uint64_t d)
{
 struct host_permit *p=user;struct hp_view v,w;
 if(!separate(p,sizeof(*p),transaction,16)||!separate(p,sizeof(*p),digest,32)||p->magic!=HP_MAGIC||!lock(p))return -1;
 if(p->consumed)return finish(p,-1);
 p->consumed=1; /* No same-boot retry, including failed or recursive callback. */
 if(p->fault||!p->offered||memcmp(transaction,p->binding.transaction,16)||
    memcmp(digest,p->binding.record_digest,32)||epoch!=p->offer.epoch||!d||
    view(p,&v)||v.epoch!=epoch||v.task!=11||v.running!=1||v.used!=1||
    v.now>=d||v.now>=p->offer.expires||view(p,&w)||w.epoch!=epoch||
    w.task!=11||w.running!=1||w.used!=1||w.now>=d||w.now>=p->offer.expires)return finish(p,-1);
 return finish(p,0);
}
