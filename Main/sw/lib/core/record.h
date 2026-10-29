#ifndef CORE_RECORD_H
#define CORE_RECORD_H
#include <stddef.h>
#include <stdint.h>
#define RECORD_HEADER_BYTES 24u
int record_encode(void *dst,uint32_t capacity,const void *payload,uint32_t bytes,uint32_t sequence);
int record_validate(const void *src,uint32_t bytes,uint32_t *sequence,uint32_t *length);
/* Returns the newer valid slot, -1 if neither is valid; wrap uses serial arithmetic. */
int record_select(const void *a,uint32_t an,const void *b,uint32_t bn);
#endif
