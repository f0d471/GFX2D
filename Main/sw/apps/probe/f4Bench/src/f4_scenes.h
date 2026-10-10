/* 由 gen_inputs.py 生成；来源 sim/engine/frontend/inputs，勿手改。 */
#ifndef F4_SCENES_H
#define F4_SCENES_H

#include <stdint.h>

typedef struct {
    const char *name;
    const uint8_t *data;
    uint32_t bytes;
} f4_scene;

extern const uint8_t f4_scene00_mode0[6820];
extern const uint8_t f4_scene04_mode0[81124];
extern const uint8_t f4_scene14_mode0[92836];
extern const f4_scene f4_scene_table[3];
extern const uint32_t f4_scene_count;

#endif /* F4_SCENES_H */