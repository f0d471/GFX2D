#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include "core/fixmath.h"
static uint32_t r=0x12345678u;
static uint32_t next(void){r^=r<<13;r^=r>>17;r^=r<<5;return r;}
static int64_t round_ref(int64_t n,int64_t d)
{int64_t q=n/d,m=n%d;if(m<0){--q;m+=d;}return q+(m>=d-d/2);}
static int32_t div_ref(int32_t a,int32_t b)
{
 if(!b)return a>0?INT32_MAX:a<0?INT32_MIN:0;
 int negative=(a<0)!=(b<0);uint64_t n=(uint64_t)(a<0?-(int64_t)a:a)*65536u;
 uint64_t d=b<0?-(int64_t)b:b,q=n/d,m=n%d;q+=negative?m>d/2:m>=d-d/2;
 if(negative)return q>=2147483648u?INT32_MIN:-(int32_t)q;
 return q>INT32_MAX?INT32_MAX:(int32_t)q;
}
int main(void)
{
 for(unsigned i=0;i<500000;++i) {
  uint64_t raw=((uint64_t)next()<<32)|next();int64_t n;memcpy(&n,&raw,8);
  uint32_t d=next()&INT32_MAX;if(!d)d=1;
  int64_t got=fx16_round_div(n,d),want=round_ref(n,d);
  if(got!=want){printf("ROUND FAIL i=%u n=%lld d=%u got=%lld want=%lld\n",i,(long long)n,d,(long long)got,(long long)want);return 1;}
  int32_t a=(int32_t)next(),b=(int32_t)next();
  if(fx16_div(a,b)!=div_ref(a,b)){printf("DIV FAIL i=%u a=%d b=%d\n",i,a,b);return 1;}
 }
 puts("DIVISION PASS 500000 full-width round and fixed-point divide pairs");return 0;
}
