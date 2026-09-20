#include "recording_fault_trace.h"
#ifdef __ZEPHYR__
#include <zephyr/linker/section_tags.h>
#define RETAINED __noinit
#else
#define RETAINED
#endif
struct retained_trace { uint32_t magic;struct recording_fault_trace value;uint32_t check; };
static volatile RETAINED struct retained_trace retained;
static uint32_t checksum(const struct recording_fault_trace *v)
{
 const unsigned char *p=(const unsigned char*)v;uint32_t h=2166136261U;
 for(unsigned i=0;i<sizeof(*v);++i)h=(h^p[i])*16777619U;
 return h;
}
void recording_fault_save(const struct recording_fault_trace *v)
{retained.magic=0;retained.value=*v;retained.check=checksum(v);retained.magic=0x52465433U;}
int recording_fault_read(struct recording_fault_trace *v)
{
 if(!v||retained.magic!=0x52465433U)return 0;
 *v=retained.value;
 return checksum(v)==retained.check&&retained.magic==0x52465433U;
}
