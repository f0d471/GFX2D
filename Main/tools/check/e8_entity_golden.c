/* 实体基准的前期研究：主机侧期望值的生成器。
 *
 * 板上 entityBench 与这里编的是**同一份 lib/core/entity.c**，
 * 跑同样的帧数、同样的初始种子。本程序把逐帧实体字段摘要与命令表摘要、
 * 终态摘要打成 C 头文件，板上的判据程序在独立的非计时复核遍里逐帧对账。
 *
 * 为什么在主机侧生成而不是板上自比：判据要求「实体所有相关字段与命令表字节
 * 逐帧在计时外摘要核对主机侧期望」。本版先生成各组 1000 帧后的终态摘要，
 * 主机侧是 x86 LP64，板是 RV32 ILP32，
 * 两边逻辑字段与命令记录一致才可通过。结构体尺寸另由静态断言约束，
 * ABI 正确性由独立 e8_entity_abi.c 对照。
 *
 *   gcc -O2 -I Main/sw/lib Main/tools/check/e8_entity_golden.c \
 *       Main/sw/lib/core/entity.c Main/sw/lib/core/crc32.c -o /tmp/e8_entity_golden
 *   /tmp/e8_entity_golden --out Main/sw/apps/probe/entityBench/src/golden.h
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/entity.h"

/* 每个用例跑多少帧、实体数。--quick 少跑几帧，只用来核对摘要路径。 */
#define FRAMES       1000u
#define FRAMES_QUICK 8u

/* 用例表。name 与板上的表同名，两边改一处就要同步。 */
typedef struct {
    const char *name;
    uint32_t    n;
    int         with_cmd;
    int         aos;        /* 1 = AoS，0 = SoA */
} case_t;

static const case_t g_cases[] = {
    { "A-upd-aos",        1000u, 0, 1 },
    { "B-upd-soa",        1000u, 0, 0 },
    { "C-submit-soa",     1000u, 1, 0 },
    { "C-n2000-soa",      2000u, 1, 0 },
    { "C-n5000-soa",      5000u, 1, 0 },
    { "C-n10000-soa",    10000u, 1, 0 },
    { "D-n2000-soa",      2000u, 0, 0 },
    { "D-n5000-soa",      5000u, 0, 0 },
    { "D-n10000-soa",    10000u, 0, 0 }
};

#define NCASES (sizeof g_cases / sizeof g_cases[0])

/* 种子：板上与这里都从同一个值开始，所以每帧的补入序列完全相同。 */
#define RNG_SEED 0x2468ace1u

typedef struct {
    uint32_t aos_digest;
    uint32_t soa_digest;
    uint32_t cmd_digest;
    uint32_t respawn;
    uint32_t cmd_used;
    uint32_t frames;
    uint32_t frame_state[FRAMES];
    uint32_t frame_cmd[FRAMES];
} result_t;

static void run_case(const case_t *c, uint32_t frames, result_t *out)
{
    static eb_ent   aos[10000];
    static uint32_t soa_words[(EB_SOA_BYTES(10000) + 3u) / 4u];
    static uint32_t cmd[10000 * EB_CMD_WORDS + EB_CMD_WORDS];
    int32_t  *x, *y, *vx, *vy;
    uint16_t *sprite, *flags;
    uint32_t  rng = RNG_SEED, f, respawn = 0u, used = 0u;
    uint32_t  i;

    memset(&aos[0], 0, sizeof aos);
    memset(soa_words, 0, sizeof soa_words);
    memset(cmd, 0, sizeof cmd);

    if (c->aos) {
        for (i = 0u; i < c->n; i++)
            eb_seed(&aos[i], &rng);
    } else {
        eb_soa_split(soa_words, c->n, &x, &y, &vx, &vy, &sprite, &flags);
        for (i = 0u; i < c->n; i++)
            eb_seed_soa(i, x, y, vx, vy, sprite, flags, &rng);
    }

    for (f = 0u; f < frames; f++) {
        if (c->aos) {
            eb_frame_aos(aos, c->n, &rng, &respawn);
        } else if (c->with_cmd) {
            used = eb_frame_soa_cmd(x, y, vx, vy, sprite, flags,
                                    c->n, &rng, cmd, &respawn);
        } else {
            eb_frame_soa(x, y, vx, vy, sprite, flags, c->n, &rng, &respawn);
        }
        out->frame_state[f] = c->aos ? eb_digest_aos(aos, c->n)
                                     : eb_digest_soa(x, y, vx, vy, sprite, flags, c->n);
        if (c->with_cmd)
            out->frame_cmd[f] = eb_digest_cmd(cmd, used * EB_CMD_WORDS);
    }

    out->frames      = frames;
    out->respawn     = respawn;
    out->cmd_used    = used;
    out->aos_digest  = c->aos ? eb_digest_aos(aos, c->n) : 0u;
    out->soa_digest  = c->aos ? 0u
                    : eb_digest_soa(x, y, vx, vy, sprite, flags, c->n);
    out->cmd_digest  = c->with_cmd ? eb_digest_cmd(cmd, used * EB_CMD_WORDS) : 0u;
}

static void emit_frames(FILE *fp, const char *kind, uint32_t case_id,
                        const uint32_t *values, uint32_t frames)
{
    uint32_t f;

    fprintf(fp, "static const uint32_t g_frame_%s_%u[EB_G_FRAMES] = {\n",
            kind, case_id);
    for (f = 0u; f < frames; f++) {
        if (f % 8u == 0u)
            fputs("    ", fp);
        fprintf(fp, "0x%08xu,%s", values[f], f % 8u == 7u ? "\n" : " ");
    }
    if (frames % 8u != 0u)
        fputc('\n', fp);
    fputs("};\n\n", fp);
}

int main(int argc, char **argv)
{
    const char *out_path = NULL;
    uint32_t    frames = FRAMES;
    FILE       *fp;
    uint32_t    i;
    result_t    r[NCASES];

    for (i = 1u; i < (uint32_t)argc; i++) {
        if (strcmp(argv[i], "--out") == 0 && i + 1u < (uint32_t)argc)
            out_path = argv[++i];
        else if (strcmp(argv[i], "--quick") == 0)
            frames = FRAMES_QUICK;
        else {
            fprintf(stderr, "用法: %s [--quick] --out <golden.h>\n", argv[0]);
            return 2;
        }
    }
    if (out_path == NULL) {
        fprintf(stderr, "缺少 --out\n");
        return 2;
    }

    for (i = 0u; i < NCASES; i++) {
        run_case(&g_cases[i], frames, &r[i]);
        printf("%-14s n %5u frames %u respawn %u cmd %u"
               "  aos %08x soa %08x cmd %08x\n",
               g_cases[i].name, g_cases[i].n, r[i].frames, r[i].respawn,
               r[i].cmd_used, r[i].aos_digest, r[i].soa_digest, r[i].cmd_digest);
    }
    if (r[0].aos_digest != r[1].soa_digest ||
        r[1].soa_digest != r[2].soa_digest) {
        fputs("同输入的 AoS/SoA 逻辑字段摘要不一致\n", stderr);
        return 1;
    }

    fp = fopen(out_path, "w");
    if (fp == NULL) {
        fprintf(stderr, "打不开 %s\n", out_path);
        return 1;
    }

    fprintf(fp, "/* 生成物：不要手改。由 Main/tools/check/e8_entity_golden.c 生成。\n");
    fprintf(fp, " *\n");
    fprintf(fp, " * 生成命令（仓库根目录下）：\n");
    fprintf(fp, " *   gcc -O2 -I Main/sw/lib Main/tools/check/e8_entity_golden.c \\\n");
    fprintf(fp, " *       Main/sw/lib/core/entity.c Main/sw/lib/core/crc32.c \\\n");
    fprintf(fp, " *       -o /tmp/e8_entity_golden && /tmp/e8_entity_golden \\\n");
    fprintf(fp, " *       --out Main/sw/apps/probe/entityBench/src/golden.h\n");
    fprintf(fp, " *\n");
    fprintf(fp, " * 判据与口径见实体池探针 entityBench（src/main.c）的文件头。\n");
    fprintf(fp, " */\n");
    fprintf(fp, "#ifndef EB_GOLDEN_H\n#define EB_GOLDEN_H\n\n#include <stdint.h>\n\n");
    fprintf(fp, "#define EB_G_FRAMES %uu\n", frames);
    fprintf(fp, "#define EB_G_N %uu\n\n", (unsigned)NCASES);
    for (i = 0u; i < NCASES; i++) {
        emit_frames(fp, "state", i, r[i].frame_state, frames);
        if (g_cases[i].with_cmd)
            emit_frames(fp, "cmd", i, r[i].frame_cmd, frames);
    }
    fprintf(fp, "typedef struct {\n");
    fprintf(fp, "    const char *name;\n");
    fprintf(fp, "    uint32_t    n;\n");
    fprintf(fp, "    uint32_t    with_cmd;\n");
    fprintf(fp, "    uint32_t    aos;\n");
    fprintf(fp, "    uint32_t    respawn;\n");
    fprintf(fp, "    uint32_t    cmd_used;\n");
    fprintf(fp, "    uint32_t    aos_digest;\n");
    fprintf(fp, "    uint32_t    soa_digest;\n");
    fprintf(fp, "    uint32_t    cmd_digest;\n");
    fprintf(fp, "    const uint32_t *frame_state;\n");
    fprintf(fp, "    const uint32_t *frame_cmd;\n");
    fprintf(fp, "} eb_golden;\n\n");
    fprintf(fp, "static const eb_golden g_golden[EB_G_N] = {\n");
    for (i = 0u; i < NCASES; i++) {
        fprintf(fp, "    { \"%s\", %uu, %uu, %uu, %uu, %uu,"
                    " 0x%08xu, 0x%08xu, 0x%08xu, g_frame_state_%u, ",
                 g_cases[i].name, g_cases[i].n, (unsigned)g_cases[i].with_cmd,
                 (unsigned)g_cases[i].aos, r[i].respawn, r[i].cmd_used,
                 r[i].aos_digest, r[i].soa_digest, r[i].cmd_digest, i);
        if (g_cases[i].with_cmd)
            fprintf(fp, "g_frame_cmd_%u },\n", i);
        else
            fputs("0 },\n", fp);
    }
    fprintf(fp, "};\n\n#endif /* EB_GOLDEN_H */\n");

    fclose(fp);
    printf("写出 %s\n", out_path);
    return 0;
}
