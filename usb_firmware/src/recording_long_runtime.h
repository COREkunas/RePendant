#ifndef OPENPENDANT_RECORDING_LONG_RUNTIME_H
#define OPENPENDANT_RECORDING_LONG_RUNTIME_H
#include "recording_control_worker.h"
int recording_runtime_long_bind(int (*auth)(void*,uint64_t),void*,const uint8_t boot[16],const uint8_t binding[32]);
int recording_runtime_long_available(void);
int recording_runtime_long_command(uint64_t,const uint8_t*,size_t,uint8_t[LC_RESPONSE_BYTES]);
int recording_runtime_long_pending(uint64_t,const uint8_t*,size_t,uint8_t[LC_RESPONSE_BYTES]);
int recording_runtime_long_disconnected(uint64_t);
int recording_runtime_long_reply_allowed(uint64_t,uint16_t);
void recording_runtime_long_button(int pressed);
#endif
