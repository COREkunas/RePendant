#include <stdint.h>
int64_t k_uptime_get(void);
struct k_work { void (*handler)(struct k_work *); };
#define K_WORK_DEFINE(name,func) struct k_work name={func}
int k_work_submit(struct k_work *);
struct k_sem { unsigned count,limit; };
#define K_SEM_DEFINE(name,initial,maximum) struct k_sem name={initial,maximum}
#define K_MSEC(ms) (ms)
void k_sem_give(struct k_sem *);
int k_sem_take(struct k_sem *,int);
