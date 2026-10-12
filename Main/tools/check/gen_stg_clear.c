/******************************************************************************
* @file gen_stg_clear.c: 搜一段通关的按键序列（游戏的工具辅助录像）
*
* @brief  「可玩」判据的通关录像用工具搜出来，不要求人工录（2026-09-28 定案）。
*         本程序只链 lib/ 与 src/game/stg_*.c，不链 bsp、不组帧，在主机上逐帧跑游戏逻辑
*         （stg_game_step，与判据入口同一个函数、同一个种子、受击打开），
*         输出 lib/core/replay 格式的录像，入库为 apps/game/gfx2dGame/replay/stg_clear.g2dr。
*
*         目标：从标题打到 CLEAR、残机不用完。优先不中弹；实在躲不开时允许中弹，
*         由自动炸弹（有炸弹时）或扣一个残机抵掉，中弹次数如实打在摘要里。
*         搜索全部是整数运算、固定顺序，同一份游戏代码两次搜出同一串按键：
*           - 按块走，一块 BLOCK（4）帧，块内按住同一个按键位图；
*             候选 18 个：九个方向（自动驾驶的固定顺序）各分高速、低速。
*             排列顺序：自动驾驶在块首选的方向（高速）在最前，其余高速按固定顺序，再是九个低速；
*           - 每个候选先走完这一块，块内中弹的淘汰；再从块末推演 HORIZON（64）帧，
*             两种走法（每块块首问一次自动驾驶、继续按住这个候选）取活得久的那种，活满 HORIZON 帧记满分。
*             两种走法都是逐块定键，搜索本身走得出来；
*           - 按得分从高到低、同分按上面的排列取第一个，提交这一块；
*           - 一块的候选全部在块内中弹（或已被禁用）时回退：退 d 块，把那一块原来提交的按键记进它的禁用表，
*             在剩下的候选里重新择优，再往前走。同一个失败前沿上连续失败时 d 按 1、1、2、2、4、4……加倍，
*             至多 64 块；前沿往前推进了就重新从 1 算。某一块从前面走过来时禁用表清空；
*           - 同一个前沿失败满 TRY_SOFT（14）次（最后两次已经退到 64 块）就不再回退，
*             在当前走不下去的那一块允许中弹：块内中弹但残机没用完的候选也参加择优，
*             得分减 HORIZON + 1，排在所有块内不中弹的候选之后。这一块提交之后前沿与计数重新开始。
*           - 要退到存档环（RING 块）以外，或允许中弹之后仍然走不下去，就算搜索失败。
*         标题与 CLEAR 两段不认方向键：按键给 0、逐帧走、不搜。
*         CLEAR 满 180 帧回到标题的那一帧是录像的最后一帧（判据入口在同一帧结束）。
*
*         注意游戏状态的拷贝：stg_game 里四个池的 ent 指向同一个结构体里的存储，
*         存档与推演都经 snap_copy，拷完把指针改回副本自己的存储。
*
*         用法：
*           gen_stg_clear --out <file.g2dr>     搜索，写录像，再从头按录像跑一遍打摘要
*           gen_stg_clear --verify <file.g2dr>  从头按录像跑一遍（不搜），打摘要
*         摘要与 gfx2dGame 判据入口（-DSTG_FIELD=4 -DSTG_JUDGE_REPLAY=1）同格式，检查脚本逐行比：
*           seg <帧号> <段名>                                         开头一行，之后每段开始一行
*           clear <帧号> score <得分> graze <擦弹> lives <残机> bombs <炸弹>   进入 CLEAR 那一帧之后的状态
*           end frames <总帧数> hits <中弹次数> over <0|1>
*         --out 另打一行 search blocks <块数> evals <候选评估次数> backtracks <回退次数> forced <允许中弹的块数>。
*         末尾 fail 与 done：走到 CLEAR、回到标题、没有残机用完、录像的帧恰好用完时 fail 0。
*         编译与运行见 Main/tools/check/README.md，由 check_sw7_game.sh 的 G16 调用。
******************************************************************************/

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/replay.h"

#include "game/stg_auto.h"
#include "game/stg_game.h"

#define BLOCK        4u
#define HORIZON      64u
#define RING         256u
#define N_CAND       18u
#define D_MAX_LOG2   6u             /* 回退距离至多 2^6 = 64 块 */
#define TRY_SOFT     14u            /* 同一个前沿上失败满这么多次就允许中弹 */
#define FRAME_LIMIT  20000u         /* 一局不击破任何符卡是 10185 帧 */
#define SC_NONE      INT32_MIN      /* 候选不可取：块内中弹且不允许，或残机用完 */

typedef struct {
    uint32_t frame0;                /* 块首的帧号 */
    uint32_t key;                   /* 提交的按键位图 */
    uint64_t ban;                   /* 禁用表：第 k 位为 1 表示按键位图 k 不再选（位图都小于 64） */
} block_rec;

static stg_game  s_cur, s_try, s_roll;
static stg_game  s_snap[RING];      /* 块首状态，按块号取模存放 */
static block_rec s_blk[RING];
static uint32_t  s_keys[FRAME_LIMIT];
static uint8_t   s_file[REPLAY_HEADER_BYTES + REPLAY_RECORD_BYTES * FRAME_LIMIT];
static uint32_t  s_evals, s_backtracks, s_forced;

/* 存档与读档。游戏状态里四个池的 ent 指向同一个结构体里的存储（stg_pools），
   整体拷贝之后还指着原件，所以拷完要把它们改回副本自己的存储；其余指针都指常量表，照拷即可。 */
static void snap_copy(stg_game *dst, const stg_game *src)
{
    stg_pools *p = &dst->f.pools;

    memcpy(dst, src, sizeof *dst);
    p->ebullet.ent  = p->ebullet_mem;
    p->pbullet.ent  = p->pbullet_mem;
    p->enemy.ent    = p->enemy_mem;
    p->particle.ent = p->particle_mem;
}

static void game_begin(stg_game *g)
{
    stg_game_init(g, STG_GAME_SEED);
    stg_game_set_hits(g, 1);
}

/* 走一帧，返回这一帧有没有中弹。 */
static int step(stg_game *g, uint32_t keys)
{
    stg_game_step(g, keys);
    return (g->ev & STG_EV_HIT) != 0u;
}

/* 从 g 往后走至多 n 帧，返回走了几帧没中弹（离开 PLAY 就当活满）。
   use_hold 为 0 时每 BLOCK 帧在块首问一次自动驾驶、块内按住，与搜索的块对齐。 */
static uint32_t rollout(stg_game *g, uint32_t n, uint32_t hold, int use_hold)
{
    uint32_t i, k = hold;

    for (i = 0u; i < n; i++) {
        if (g->phase != STG_PH_PLAY)
            return n;
        if (!use_hold && i % BLOCK == 0u)
            k = stg_auto_keys(g, NULL);
        if (step(g, k))
            return i;
    }
    return n;
}

/* 候选的排列：自动驾驶选的方向（高速）在最前，其余高速按固定顺序，再是九个低速。 */
static void cand_order(const stg_game *g, uint32_t key[N_CAND])
{
    const uint32_t first = stg_auto_keys(g, NULL);
    uint32_t       i, n = 0u;

    key[n++] = first;
    for (i = 0u; i < STG_AUTO_N; i++)
        if (stg_auto_cand_keys(i) != first)
            key[n++] = stg_auto_cand_keys(i);
    for (i = 0u; i < STG_AUTO_N; i++)
        key[n++] = stg_auto_cand_keys(i) | STG_KEY_SLOW;
}

/* 一个候选的得分：块末之后两种走法里活得久的帧数（0~HORIZON）。
   块内中弹时：allow_hit 为 0 或残机用完记 SC_NONE，否则照样推演、得分减 HORIZON + 1。 */
static int32_t cand_score(const stg_game *base, uint32_t key, int allow_hit)
{
    uint32_t i, a, h;
    int32_t  pen = 0;

    s_evals++;
    snap_copy(&s_try, base);
    for (i = 0u; i < BLOCK && s_try.phase == STG_PH_PLAY; i++) {
        if (step(&s_try, key)) {
            if (!allow_hit)
                return SC_NONE;
            pen = (int32_t)HORIZON + 1;
        }
        if ((s_try.ev & STG_EV_OVER) != 0u)
            return SC_NONE;
    }
    if (s_try.phase != STG_PH_PLAY)
        return (int32_t)HORIZON - pen;
    snap_copy(&s_roll, &s_try);
    a = rollout(&s_roll, HORIZON, 0u, 0);
    if (a >= HORIZON)
        return (int32_t)HORIZON - pen;
    snap_copy(&s_roll, &s_try);
    h = rollout(&s_roll, HORIZON, key, 1);
    return (int32_t)((a > h) ? a : h) - pen;
}

/* 在没有被禁用的候选里取得分最高的（同分取排列在前的），写进 *key；都不可取时返回 SC_NONE。 */
static int32_t block_choose(const stg_game *base, uint64_t ban, int allow_hit, uint32_t *key)
{
    uint32_t cand[N_CAND];
    uint32_t i;
    int32_t  best = SC_NONE;

    cand_order(base, cand);
    for (i = 0u; i < N_CAND; i++) {
        int32_t sc;

        if ((ban >> cand[i]) & 1u)
            continue;
        sc = cand_score(base, cand[i], allow_hit);
        if (sc > best) {
            best = sc;
            *key = cand[i];
        }
    }
    return best;
}

/* 从块首状态按 key 提交一块：写进 s_cur 与 s_keys，返回走了几帧（遇到 PLAY 结束就停）。 */
static uint32_t block_commit(const stg_game *base, uint32_t frame0, uint32_t key)
{
    uint32_t i;

    snap_copy(&s_cur, base);
    for (i = 0u; i < BLOCK && s_cur.phase == STG_PH_PLAY; i++) {
        (void)step(&s_cur, key);
        s_keys[frame0 + i] = key;
    }
    return i;
}

/* 搜索。成功返回录像的帧数，失败返回 0。 */
static uint32_t search(uint32_t *out_blocks)
{
    uint32_t frame = 0u, nb = 0u, hi = 0u, front = 0u, tries = 0u;
    int      fresh = 1, allow = 0;

    game_begin(&s_cur);
    for (;;) {
        block_rec *b;
        uint32_t   key = 0u, d, lo, j;
        int32_t    sc;

        if (frame >= FRAME_LIMIT - BLOCK) {
            fprintf(stdout, "err 超过 %u 帧还没回到标题\n", FRAME_LIMIT);
            return 0u;
        }
        if (s_cur.phase != STG_PH_PLAY) {
            (void)step(&s_cur, 0u);
            s_keys[frame++] = 0u;
            if ((s_cur.ev & STG_EV_SEG) != 0u && s_cur.seg == STG_GSEG_TITLE) {
                *out_blocks = nb;
                return frame;
            }
            continue;
        }

        b = &s_blk[nb % RING];
        if (fresh) {                            /* 从前面走过来：存块首、清禁用表 */
            snap_copy(&s_snap[nb % RING], &s_cur);
            b->frame0 = frame;
            b->ban    = 0u;
        }
        sc = block_choose(&s_snap[nb % RING], b->ban, allow, &key);
        if (sc >= 0 || (allow && sc != SC_NONE)) {
            if (allow) {                        /* 允许中弹的这一块提交了：前沿与计数重新开始 */
                s_forced++;
                allow = 0;
                front = nb + 1u;
                tries = 0u;
            }
            b->key = key;
            frame  = b->frame0 + block_commit(&s_snap[nb % RING], b->frame0, key);
            nb++;
            if (nb > hi)
                hi = nb;
            fresh = 1;
            continue;
        }
        if (allow) {
            fprintf(stdout, "err 块 %u 允许中弹之后仍然走不下去\n", nb);
            return 0u;
        }

        /* 这一块走不下去：退 d 块，禁用那一块原来的按键；同一个前沿失败满 TRY_SOFT 次就在这一块允许中弹 */
        s_backtracks++;
        if (nb > front) {
            front = nb;
            tries = 0u;
        }
        tries++;
        if (tries >= TRY_SOFT) {
            allow = 1;
            fresh = 0;
            continue;
        }
        d  = 1u << (((tries - 1u) / 2u < D_MAX_LOG2) ? (tries - 1u) / 2u : D_MAX_LOG2);
        lo = (hi > RING) ? hi - RING : 0u;
        j  = (nb >= lo + d) ? nb - d : lo;
        if (j >= nb) {
            fprintf(stdout, "err 块 %u 退不回去（存档环最早是块 %u）\n", nb, lo);
            return 0u;
        }
        s_blk[j % RING].ban |= (uint64_t)1u << s_blk[j % RING].key;
        nb    = j;
        fresh = 0;
    }
}

/* --- 按录像从头跑一遍，打摘要 ------------------------------------------------ */

static void say_seg(uint32_t frame, uint32_t seg)
{
    fprintf(stdout, "seg %u %s\n", frame, stg_game_seg_name(seg));
}

/* 返回 fail 数。 */
static uint32_t play(const uint32_t *keys, uint32_t n)
{
    uint32_t frame, hits = 0u, over = 0u, clear = 0u, fail = 0u;

    game_begin(&s_cur);
    say_seg(0u, s_cur.seg);
    for (frame = 0u; frame < n; frame++) {
        if (step(&s_cur, keys[frame]))
            hits++;
        if ((s_cur.ev & STG_EV_SEG) != 0u)
            say_seg(frame, s_cur.seg);
        if ((s_cur.ev & STG_EV_SEG) != 0u && s_cur.seg == STG_GSEG_CLEAR) {
            clear = 1u;
            fprintf(stdout, "clear %u score %u graze %u lives %u bombs %u\n",
                    frame, s_cur.score, s_cur.graze, s_cur.lives, s_cur.bombs);
        }
        if ((s_cur.ev & STG_EV_OVER) != 0u)
            over = 1u;
        if ((s_cur.ev & STG_EV_SEG) != 0u && s_cur.seg == STG_GSEG_TITLE) {
            frame++;
            break;
        }
    }
    fprintf(stdout, "end frames %u hits %u over %u\n", frame, hits, over);
    if (!clear || over) {
        fprintf(stdout, "err 没有走到 CLEAR 或残机用完\n");
        fail++;
    }
    if (frame != n) {
        fprintf(stdout, "err 录像有 %u 帧，回到标题时用了 %u 帧\n", n, frame);
        fail++;
    }
    return fail;
}

static int finish(uint32_t fail)
{
    fprintf(stdout, "fail %u\ndone\n", fail);
    return (fail == 0u) ? 0 : 1;
}

static int cmd_out(const char *path)
{
    uint32_t blocks = 0u, n, len, fail;
    FILE    *f;

    n = search(&blocks);
    if (n == 0u)
        return finish(1u);
    fail = play(s_keys, n);
    fprintf(stdout, "search blocks %u evals %u backtracks %u forced %u\n",
            blocks, s_evals, s_backtracks, s_forced);

    len = replay_pack(s_file, (uint32_t)sizeof s_file, STG_GAME_SEED, s_keys, n);
    f = (len != 0u) ? fopen(path, "wb") : NULL;
    if (f == NULL || fwrite(s_file, 1u, len, f) != len) {
        fprintf(stdout, "err 写不出 %s\n", path);
        fail++;
    }
    if (f != NULL && fclose(f) != 0) {
        fprintf(stdout, "err 关不上 %s\n", path);
        fail++;
    }
    return finish(fail);
}

static int cmd_verify(const char *path)
{
    replay_view v;
    FILE       *f = fopen(path, "rb");
    size_t      len;
    uint32_t    i;

    if (f == NULL) {
        fprintf(stdout, "err 打不开 %s\n", path);
        return finish(1u);
    }
    len = fread(s_file, 1u, sizeof s_file, f);
    fclose(f);
    if (replay_open(s_file, (uint32_t)len, &v) != 0 || v.frames > FRAME_LIMIT) {
        fprintf(stdout, "err %s 不是合法的录像\n", path);
        return finish(1u);
    }
    if (v.seed != STG_GAME_SEED) {
        fprintf(stdout, "err 录像的种子 0x%08x 不是游戏的 0x%08x\n", v.seed, STG_GAME_SEED);
        return finish(1u);
    }
    for (i = 0u; i < v.frames; i++)
        (void)replay_key_at(&v, i, &s_keys[i]);
    return finish(play(s_keys, v.frames));
}

int main(int argc, char **argv)
{
    if (argc == 3 && strcmp(argv[1], "--out") == 0)
        return cmd_out(argv[2]);
    if (argc == 3 && strcmp(argv[1], "--verify") == 0)
        return cmd_verify(argv[2]);
    fprintf(stderr, "用法：gen_stg_clear --out <file.g2dr> | --verify <file.g2dr>\n");
    return 2;
}
