/******************************************************************************
* @file vec_replay.h: 一致性向量的读取与重放
*
* @brief  两个来源、两个实现，交叉跑。
*
*         来源一是 `tools/check/check_cmd_semantics.py --emit` 导出的 159 条
*         （ABI 1，交换格式见 spec 第 02 章规范条款 §7.2）。它是规范的可执行镜像，
*         生成物不入库，所以这一组要先跑一次工具、再把路径交给 `make test VEC=…`。
*         来源二是 `vec_builtin.c` 里按第 02 章条文手写的那一组，不依赖 python，
*         默认就跑。
*
*         实现一是产品的 `exec_sw`，实现二是 `ref_exec`。
*
*         重放逐项比对错误码、当前表内零基索引、标签、取数次数、最终 clip
*         与期望内存，任何一项不同都算失败。
******************************************************************************/
#ifndef GFX2D_TEST_VEC_REPLAY_H
#define GFX2D_TEST_VEC_REPLAY_H

#include "gfx/gfx.h"

#define VEC_MAX_CMD   32u
#define VEC_MAX_LIST   4u
#define VEC_MAX_MEM  128u
#define VEC_NAME_LEN  64u

/* 一条向量。字段名与交换格式的 [vector] 行一一对应。 */
typedef struct {
    char     group[8];
    char     name[VEC_NAME_LEN];
    uint32_t nlist;
    int32_t  reads;                     /* -1 表示该向量没有给出这一项 */
    int      code;
    uint32_t errlist;
    uint32_t erridx;
    uint32_t errtag;
    int32_t  clip[4];

    uint32_t surf_seen;                 /* 出现过的 slot 的位图 */
    gfx_surf_raw surf[GFX_SURF_N];

    uint32_t listcount[VEC_MAX_LIST];   /* 每张表的 LIST_COUNT */
    uint32_t listncmd[VEC_MAX_LIST];    /* 每张表实际可读的记录数 */
    uint32_t ncmd;                      /* 各表记录数之和，cmds 里已填多少条 */
    gfx_cmd  cmds[VEC_MAX_CMD];

    uint32_t nmem, nexp;
    uint32_t mem_addr[VEC_MAX_MEM], mem_val[VEC_MAX_MEM];
    uint32_t exp_addr[VEC_MAX_MEM], exp_val[VEC_MAX_MEM];
} vec_case;

/* 跑一条向量。use_ref 非 0 时用 ref_exec，否则用 exec_sw。
   失败时把原因写进 why（至多 cap-1 个字符），返回 0；通过返回 1。 */
int vec_run_one(const vec_case *v, int use_ref, char *why, uint32_t cap);

/* 读并重放一个交换格式文件。返回失败条数，*ntotal 给出读到的向量数。
   文件打不开时返回 -1。 */
int vec_replay_file(const char *path, int use_ref, uint32_t *ntotal);

#endif /* GFX2D_TEST_VEC_REPLAY_H */
