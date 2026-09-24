/* SPDX-License-Identifier: Apache-2.0 */
#include "device_preferences.h"
#include <string.h>
#include <limits.h>
uint32_t dp_revision(const uint8_t *p)
{return (uint32_t)p[12]|(uint32_t)p[13]<<8|(uint32_t)p[14]<<16|(uint32_t)p[15]<<24;}
void dp_defaults(uint8_t p[DP_BYTES])
{const uint8_t d[DP_BYTES]={2,0,32,1,4,0,0,1,25,1,0,0,0,0,0,0};memcpy(p,d,DP_BYTES);}
void dp_upgrade(uint8_t *p){if(dp_valid(p,DP_BYTES)&&p[0]==1)p[0]=2;}
int dp_valid(const uint8_t *p,size_t n)
{return p&&n==DP_BYTES&&(p[0]==1||p[0]==2)&&p[1]<=2&&p[2]>=8&&p[2]<=64&&p[3]>=1&&p[3]<=7&&
 p[4]>=1&&p[4]<=7&&p[5]<=7&&p[6]<=7&&p[7]>=1&&p[7]<=7&&p[8]>=20&&p[8]<=50&&p[9]<=3&&
 p[10]<=1&&p[11]<=7&&(p[0]==2||(!p[10]&&!p[11]));}
int dp_next(const uint8_t *c,const uint8_t *r,uint8_t *o)
{if(!o||!dp_valid(c,DP_BYTES)||!dp_valid(r,DP_BYTES)||c[0]!=r[0]||dp_revision(c)!=dp_revision(r)||dp_revision(c)==UINT32_MAX)return -1;
 memcpy(o,r,DP_BYTES);uint32_t rev=dp_revision(c)+1;for(unsigned i=0;i<4;i++)o[12+i]=(uint8_t)(rev>>(8*i));return 0;}
uint32_t dp_idle_ms(const uint8_t *p)
{const uint32_t values[]={0,60000,300000,900000};return dp_valid(p,DP_BYTES)?values[p[9]]:0;}
uint16_t dp_advertising_units(const uint8_t *p,int idle)
{if(!dp_valid(p,DP_BYTES)||!idle||!dp_idle_ms(p))return 48;return p[1]==2?1600:p[1]==1?400:48;}
void dp_color(const uint8_t *p,enum dp_event event,uint64_t now,uint8_t rgb[3])
{static const uint8_t colors[8][3]={{0,0,0},{1,0,0},{0,1,0},{0,0,1},{1,1,0},{0,1,1},{1,0,1},{1,1,1}};
 memset(rgb,0,3);if(!dp_valid(p,DP_BYTES)||event<DP_RECORDING||event>DP_CHARGING)return;
 unsigned color=event==DP_CHARGING?p[11]:p[2+event];if(event==DP_LOW&&now%2000>=250)return;
 for(unsigned i=0;i<3;i++)rgb[i]=(uint8_t)(colors[color][i]*p[2]);}
int dp_blink(unsigned count,uint64_t began,uint64_t now)
{if((count!=2&&count!=3)||now<began||now-began>=count*300U)return -1;return (now-began)%300U<150U;}
