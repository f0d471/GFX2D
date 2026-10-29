#ifndef CORE_FIRMWARE_IMAGE_H
#define CORE_FIRMWARE_IMAGE_H
#include <stdint.h>
#define FIRMWARE_HEADER_BYTES 32u
/* Container for staging/inspection; existing Flash boot format is unchanged. */
int firmware_image_encode(void *out,uint32_t capacity,const void *image,uint32_t bytes,uint32_t target,uint32_t version);
int firmware_image_validate(const void *data,uint32_t bytes,uint32_t target,uint32_t minimum_version,uint32_t *image_bytes);
#endif
