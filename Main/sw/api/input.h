/* Portable action bits. Pin assignment and transport are private to the port. */
#ifndef PLATFORM_INPUT_H
#define PLATFORM_INPUT_H
#include <stdint.h>
#define PLATFORM_KEY_UP       (1u<<0)
#define PLATFORM_KEY_DOWN     (1u<<1)
#define PLATFORM_KEY_LEFT     (1u<<2)
#define PLATFORM_KEY_RIGHT    (1u<<3)
#define PLATFORM_KEY_ACTION   (1u<<4)
#define PLATFORM_KEY_SLOW     (1u<<5)
#define PLATFORM_KEY_BACKEND  (1u<<6)
#define PLATFORM_KEY_MODE1    (1u<<7)
#define PLATFORM_KEY_MODE2    (1u<<8)
#define PLATFORM_KEY_MODE3    (1u<<9)
#define PLATFORM_KEY_CONFIRM  (1u<<10)
#define PLATFORM_KEY_BACK     (1u<<11)
#define PLATFORM_KEY_SCENE    (1u<<12)
#define PLATFORM_KEY_APP5     (1u<<13)   /* 快捷键 5:进 RIDGE FLIGHT */
#define PLATFORM_KEY_APP6     (1u<<14)   /* 快捷键 6:进 HALTTIME(SW21;15 位串口快照最高有效位) */
#define PLATFORM_KEY_STRAFE_L (1u<<15)
#define PLATFORM_KEY_STRAFE_R (1u<<16)
#define PLATFORM_KEY_PICKUP   (1u<<17)
#define PLATFORM_KEY_RESTART  (1u<<18)
#define PLATFORM_KEY_LAUNCH_MASK (PLATFORM_KEY_MODE1|PLATFORM_KEY_MODE2|PLATFORM_KEY_MODE3)
typedef struct { uint32_t held,pressed,released; } platform_input;
#endif
