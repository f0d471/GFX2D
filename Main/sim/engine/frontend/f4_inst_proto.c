/* F4 研究原型宿主对拍器：在 F1 冻结输入上验证 v1-I 编码契约的等价性，
 * 附定向边界样例。构造器与展开器的实现在 f4_core.c，与板测应用共用；
 * 生产 API、注错与完整判据由开发轮按 Todo/绘制前端F4 的 spec 另行交付。 */
#include "f4_core.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ARENA_MAX  (1u << 20)
#define STREAM_MAX 65536u
#define BOUND_N    70000u

static gfx_cmd *load_v1(const char *path, uint32_t *n)
{
    FILE *f = fopen(path, "rb");
    unsigned char hdr[196];
    uint32_t words[1];
    gfx_cmd *recs;
    uint32_t i;
    if (!f)
        return NULL;
    if (fread(hdr, 1, 196, f) != 196) {
        fclose(f);
        return NULL;
    }
    memcpy(words, hdr + 192, 4);
    *n = words[0];
    recs = malloc(sizeof(gfx_cmd) * (*n ? *n : 1));
    if (!recs) {
        fclose(f);
        return NULL;
    }
    for (i = 0; i < *n; i++) {
        if (fread(recs[i].w, 4, 8, f) != 8) {
            fclose(f);
            free(recs);
            return NULL;
        }
    }
    fclose(f);
    return recs;
}

/* 返回 0 等价；负数是失败步骤。原 v1 记录数与展开输出数必须守恒。 */
static int diff_scene(f4_builder *b, const gfx_cmd *in, uint32_t n,
                      gfx_cmd *expand, f4_id *ids, uint32_t cap)
{
    uint32_t i, k = 0;
    long got;
    memset(b->arena, 0xEE, b->arena_bytes);
    if (f4_build(b, in, n) != GFX_OK)
        return -1;
    got = f4_expand(b->stream, b->used, b->arena, b->arena_used, expand, cap, ids);
    if (got < 0)
        return -2;
    if ((uint32_t)got != n)
        return -3;    /* 非实例命令透传加实例展开，总数必须守恒 */
    for (i = 0; i < n; i++) {
        const gfx_cmd *o = &expand[i];
        if (gfx_op(&in[i]) == GFX_OP_BLIT) {
            /* 语义字段逐字相等；w4 在旧 BLIT 里未使用，展开置 0 */
            if (o->w[0] != in[i].w[0] || o->w[1] != in[i].w[1] ||
                o->w[2] != in[i].w[2] || o->w[3] != in[i].w[3] ||
                o->w[5] != in[i].w[5] || o->w[4] != 0)
                return -4;
            /* 原 v1 的标签契约：w7 等于记录的零基索引 */
            if (in[i].w[7] != i)
                return -5;
            /* 身份还原：第 k 颗 BLIT 的 (pkg,inst) 必须落在包流范围内 */
            if (ids[i].pkg >= b->used ||
                gfx_op(&b->stream[ids[i].pkg]) != F4_OP_BLIT_INST ||
                ids[i].inst >= b->stream[ids[i].pkg].w[2] ||
                b->stream[ids[i].pkg].w[7] != ids[i].pkg)
                return -6;
            k++;
        } else if (memcmp(o, &in[i], sizeof(gfx_cmd)) != 0) {
            return -7;    /* 透传命令必须逐字相同 */
        }
    }
    return 0;
}

static int boundary_cases(void)
{
    /* 定向边界：拆包上界、append 层坐标拒绝、坏 layout/stride 拒绝 */
    static gfx_cmd in[BOUND_N];
    static gfx_cmd expand[BOUND_N];
    static f4_id ids[BOUND_N];
    static uint8_t arena[ARENA_MAX];
    f4_builder b;
    uint32_t i;
    long got;
    int rc;

    memset(&b, 0, sizeof b);
    static gfx_cmd boundary_stream[F4_INST_MAX+8];
    b.stream = boundary_stream;
    b.cap = F4_INST_MAX + 8;
    b.arena = arena;
    b.arena_bytes = ARENA_MAX;

    /* 同状态 70000 颗固定源 BLIT：拆成 65535+4465 两包 */
    for (i = 0; i < BOUND_N; i++) {
        in[i].w[0] = GFX_OP_BLIT | (GFX_EQ_ALPHA << 8) | (2u << 16);
        in[i].w[1] = f4_pack_pair(8, 8);
        in[i].w[2] = f4_pack_pair((int32_t)(i % 600), (int32_t)(i / 600));
        in[i].w[3] = f4_pack_pair(16, 16);
        in[i].w[5] = 0;
        in[i].w[7] = i;
    }
    rc = f4_build(&b, in, BOUND_N);
    if (rc != GFX_OK) {
        printf("build rc=%d used=%u\n", rc, b.used);
        return 1;
    }
    /* build 末尾已封包，续包的实例数看 stream[1].w[2] */
    if (b.used != 2 || b.split_limit != 1 ||
        b.stream[1].w[2] != BOUND_N - F4_INST_MAX) {
        printf("split diag: used=%u split=%u count2=%u pk4=%u\n",
               b.used, b.split_limit, b.stream[1].w[2], b.packets4);
        return 2;
    }
    got = f4_expand(b.stream, b.used, b.arena, b.arena_used, expand, BOUND_N, ids);
    if (got != (long)BOUND_N)
        return 3;
    for (i = 0; i < BOUND_N; i++)
        if (expand[i].w[2] != in[i].w[2])
            return 4;

    /* 越界坐标必须在新 API 的 append 层被拒绝；v1 记录本身是 i16 编码，
       从 v1 流读出的坐标不存在越界，无损检查只作用于 int32 入参 */
    if (f4_append_inst(&b, GFX_OP_BLIT, 0, f4_pack_pair(4, 4), 32768, 0, 8, 8)
        != GFX_BUILD_BAD_ARG)
        return 5;
    if (f4_append_inst(&b, GFX_OP_BLIT, 0, f4_pack_pair(4, 4), 0, -32769, 8, 8)
        != GFX_BUILD_BAD_ARG)
        return 6;
    if (f4_append_inst(&b, GFX_OP_BLIT, 0, f4_pack_pair(4, 4), 100, 100, 8, 8)
        != GFX_OK)
        return 7;

    /* 坏 layout/stride 包必须被展开拒绝（独立编码，不经构造器） */
    {
        gfx_cmd bad;
        memset(&bad, 0, sizeof bad);
        bad.w[0] = F4_OP_BLIT_INST;
        bad.w[2] = 1;
        bad.w[5] = 8u | (2u << 16);      /* layout=2 按 BAD_FIELD 拒绝 */
        if (f4_expand(&bad, 1, arena, ARENA_MAX, expand, 8, NULL) != -2)
            return 8;
        bad.w[5] = 3u;                   /* stride 非 4 的倍数 */
        if (f4_expand(&bad, 1, arena, ARENA_MAX, expand, 8, NULL) != -2)
            return 9;
        bad.w[5] = 4u | (1u << 16);      /* XYUV8 但 w6 未清零 */
        bad.w[6] = 0x12345678u;
        if (f4_expand(&bad, 1, arena, ARENA_MAX, expand, 8, NULL) != -2)
            return 10;
    }
    if(f4_append_inst(&b,GFX_OP_BLIT,0,f4_pack_pair(1,1),INT32_MAX,INT32_MIN,0,0)!=GFX_BUILD_BAD_ARG)
        return 14;
    /* DMA 对齐与完整末拍边界必须独立拒绝。 */
    {
        gfx_cmd p={{F4_OP_BLIT_INST,4u,1u,0u,0u,4u,0u,0u}};
        if(f4_expand(&p,1,arena,16,expand,8,NULL)!=-2)return 11;
        p.w[1]=0u;
        if(f4_expand(&p,1,arena,4,expand,8,NULL)!=-3)return 12;
        if(f4_expand(&p,1,arena,16,expand,8,NULL)!=1)return 13;
    }
    return 0;
}

int main(int argc, char **argv)
{
    static gfx_cmd *expand;
    static f4_id *ids;
    uint8_t *arena;
    int result=0;
    f4_builder b;
    if (argc < 2) {
        fputs("usage: f4_inst_proto <sceneNN_mode0.bin>... | --boundary\n", stderr);
        return 2;
    }
    if (strcmp(argv[1], "--boundary") == 0) {
        int rc = boundary_cases();
        printf("boundary %s (rc=%d)\n", rc == 0 ? "PASS" : "FAIL", rc);
        return rc;
    }
    arena=malloc(ARENA_MAX);
    expand = malloc(sizeof(gfx_cmd) * STREAM_MAX);
    ids = malloc(sizeof(f4_id) * STREAM_MAX);
    memset(&b, 0, sizeof b);
    b.stream = malloc(sizeof(gfx_cmd) * STREAM_MAX);
    b.cap = STREAM_MAX;
    b.arena = arena;
    b.arena_bytes = ARENA_MAX;
    if(!arena || !expand || !ids || !b.stream){result=1;goto cleanup;}
    for (int a = 1; a < argc; a++) {
        uint32_t n = 0;
        gfx_cmd *in = load_v1(argv[a], &n);
        int rc;
        if (!in) {
            printf("%s LOAD-FAIL\n", argv[a]);
            result=1;goto cleanup;
        }
        rc = diff_scene(&b, in, n, expand, ids, STREAM_MAX);
        if (rc != 0) {
            printf("%s DIFF rc=%d\n", argv[a], rc);
            free(in);result=1;goto cleanup;
        }
        {
            uint32_t bytes_orig = 32u * n;
            uint32_t bytes_new = 32u * b.used + b.arena_used;
            printf("%s PASS in=%u stream=%u arena=%u pk4=%u in4=%u pk8=%u in8=%u "
                   "bytes %u->%u (%u.%02u%%)\n",
                   argv[a], n, b.used, b.arena_used, b.packets4, b.inst4,
                   b.packets8, b.inst8, bytes_orig, bytes_new,
                   bytes_new * 100u / bytes_orig,
                   (bytes_new * 10000u / bytes_orig) % 100u);
        }
        free(in);
    }
cleanup:
    free(arena);free(expand);free(ids);free(b.stream);
    return result;
}
