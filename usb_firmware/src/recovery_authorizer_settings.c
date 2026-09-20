#include "recovery_authorizer_settings.h"
#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
K_MUTEX_DEFINE(ra_settings_mutex);
static struct ra_settings_permit permit;
static uint32_t bound,held,permit_taken,write_attempted;
static uint8_t permitted_transaction[16],permitted_digest[32];
static uint64_t now_ms(void *u){(void)u;return (uint64_t)k_uptime_get();}
static int acquire(void *u){(void)u;if(k_mutex_lock(&ra_settings_mutex,K_NO_WAIT))return -EBUSY;
 if(held){k_mutex_unlock(&ra_settings_mutex);return -EBUSY;}held=1;return 0;}
static int release(void *u){(void)u;if(!held)return -EPERM;
 int rc=k_mutex_unlock(&ra_settings_mutex);if(!rc)held=0;return rc;}
static int consume(void *u,const uint8_t transaction[16],const uint8_t digest[32],uint64_t d)
{
 (void)u;if(!held||permit_taken||now_ms(NULL)>=d)return -EPERM;
 permit_taken=1; /* Ambiguous external durable effect cannot retry this boot. */
 int rc=permit.consume(permit.user,transaction,digest,d);if(rc||now_ms(NULL)>=d)return -EPERM;
 memcpy(permitted_transaction,transaction,16);memcpy(permitted_digest,digest,32);return 0;
}
struct load {uint8_t *out;size_t actual;int seen,error;};
static int loaded(const char *key,size_t len,settings_read_cb read_cb,void *arg,void *user)
{
 struct load *l=user;if(l->error)return l->error;
 if((key&&*key)||l->seen||len!=RA_RECORD_BYTES||!read_cb){l->error=-EINVAL;return l->error;}
 l->seen=1;ssize_t n=read_cb(arg,l->out,len);
 if(n!=(ssize_t)len){l->error=-EIO;return l->error;}l->actual=len;return 0;
}
static int read_record(void *u,uint8_t out[RA_RECORD_BYTES],size_t *actual,uint64_t d)
{
 (void)u;*actual=0;if(!held||!permit_taken||now_ms(NULL)>=d)return -EPERM;
 struct load l={out,0,0,0};int rc=settings_load_subtree_direct(RA_SETTINGS_KEY,loaded,&l);
 /* Pinned SDK may ignore callback errors. Retain them independently. */
 if(rc||l.error||now_ms(NULL)>=d)return rc?rc:l.error?l.error:-ETIMEDOUT;
 *actual=l.actual;return l.seen?0:RA_ABSENT;
}
static int write_record(void *u,const uint8_t record[RA_RECORD_BYTES],uint64_t d)
{
 (void)u;if(!held||!permit_taken||write_attempted||now_ms(NULL)>=d||
    memcmp(record+32,permitted_transaction,16)||memcmp(record+RA_DIGEST_OFFSET,permitted_digest,32))return -EPERM;
 write_attempted=1;
 int rc=settings_save_one(RA_SETTINGS_KEY,record,RA_RECORD_BYTES);
 return rc?rc:now_ms(NULL)>=d?-ETIMEDOUT:0;
}
int ra_settings_bind(struct ra_port *out,const struct ra_settings_permit *p)
{
 if(!out||!p||!p->consume||k_mutex_lock(&ra_settings_mutex,K_NO_WAIT))return -EINVAL;
 if(bound){k_mutex_unlock(&ra_settings_mutex);return -EALREADY;}bound=1;permit=*p;
 *out=(struct ra_port){NULL,now_ms,acquire,release,consume,read_record,write_record};
 int rc=k_mutex_unlock(&ra_settings_mutex);return rc?rc:0;
}
