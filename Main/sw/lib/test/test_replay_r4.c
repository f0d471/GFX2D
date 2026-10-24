/******************************************************************************
* @file test_replay_r4.c: R4 录像格式（R4RP）的主机侧测试
*
* @brief  四组：头部字节序与往返、坏输入（错魔数/版本/保留位/tick_hz/阶梯/
*         截断）、流式录制与一次打包等价、录制文本的字面量。
*         与 .g2dr 的测试（test_replay.c）分开：两个格式互不改语义，
*         这一组红了不允许动 replay.c 去「修」。
******************************************************************************/

#include <stdio.h>
#include <string.h>

#include "core/format.h"
#include "core/replay_r4.h"
#include "test_replay_r4.h"

static uint32_t g_fail;

static void say(const char *s)
{
    fputs(s, stdout);
}

static void ck(const char *tag, int ok)
{
    char     line[128];
    core_out o;

    core_out_init(&o, line, (uint32_t)sizeof line);
    core_out_str(&o, tag);
    core_out_str(&o, ok ? " ok\n" : " FAIL\n");
    say(line);
    if (!ok)
        g_fail++;
}

static void ck_u32(const char *tag, uint32_t expect, uint32_t got)
{
    char     line[160];
    core_out o;

    core_out_init(&o, line, (uint32_t)sizeof line);
    core_out_str(&o, tag);
    core_out_str(&o, " expect=");
    core_out_u32(&o, expect);
    core_out_str(&o, " got=");
    core_out_u32(&o, got);
    core_out_str(&o, (expect == got) ? " ok\n" : " FAIL\n");
    say(line);
    if (expect != got)
        g_fail++;
}

static void r1_roundtrip(void)
{
    uint8_t        buf[64];
    uint32_t       keys[3] = { 0x00000001u, 0x00000200u, 0xFFFFFFFFu };
    replay_r4_view v;
    uint32_t       got;
    uint32_t       i;

    ck_u32("R4RP bytes_for 3", 32u + 12u, replay_r4_bytes_for(3u));
    ck_u32("R4RP bytes_for 0", 32u, replay_r4_bytes_for(0u));
    ck_u32("R4RP len", 44u, replay_r4_pack(buf, (uint32_t)sizeof buf, 9u, 0xAABBCCDDu,
                                           REPLAY_R4_L2, 3u, keys, 3u));

    ck("R4RP 魔数", buf[0] == 'R' && buf[1] == '4' && buf[2] == 'R' && buf[3] == 'P');
    ck("R4RP 版本", buf[4] == 1u && buf[5] == 0u);
    ck("R4RP 标志", buf[6] == 0u && buf[7] == 0u);
    ck("R4RP tick 小端", buf[8] == 30u && buf[9] == 0u && buf[10] == 0u && buf[11] == 0u);
    ck("R4RP 种子小端", buf[12] == 9u && buf[13] == 0u && buf[14] == 0u && buf[15] == 0u);
    ck("R4RP 场景小端",
       buf[16] == 0xDDu && buf[17] == 0xCCu && buf[18] == 0xBBu && buf[19] == 0xAAu);
    ck("R4RP 阶梯小端", buf[20] == 2u && buf[21] == 0u && buf[22] == 0u && buf[23] == 0u);
    ck("R4RP 素材小端", buf[24] == 3u && buf[25] == 0u && buf[26] == 0u && buf[27] == 0u);
    ck("R4RP tick 数小端", buf[28] == 3u && buf[29] == 0u && buf[30] == 0u && buf[31] == 0u);
    ck("R4RP 第 0 tick 小端",
       buf[32] == 0x01u && buf[33] == 0u && buf[34] == 0u && buf[35] == 0u);

    ck("R4RP open", replay_r4_open(buf, 44u, &v) == 0);
    ck_u32("R4RP seed", 9u, v.seed);
    ck_u32("R4RP scene", 0xAABBCCDDu, v.scene_id);
    ck_u32("R4RP ladder", REPLAY_R4_L2, v.ladder);
    ck_u32("R4RP assets", 3u, v.asset_version);
    ck_u32("R4RP frames", 3u, v.frames);
    for (i = 0u; i < 3u; i++) {
        got = 0u;
        ck("R4RP key_at", replay_r4_key_at(&v, i, &got) == 0);
        ck_u32("R4RP key", keys[i], got);
    }
    got = 0x1234u;
    ck("R4RP 越界被拒", replay_r4_key_at(&v, 3u, &got) != 0 && got == 0x1234u);

    ck("R4RP cap 差一字节", replay_r4_pack(buf, 43u, 9u, 1u, REPLAY_R4_L1, 3u, keys, 3u) == 0u);
    ck("R4RP 非法阶梯打包被拒",
       replay_r4_pack(buf, 44u, 9u, 1u, 3u, 3u, keys, 3u) == 0u);
    ck("R4RP 空指针", replay_r4_pack(NULL, 44u, 9u, 1u, REPLAY_R4_L1, 3u, keys, 3u) == 0u);
}

static void r2_bad_input(void)
{
    uint8_t        good[44];
    uint8_t        bad[48];
    uint32_t       keys[3] = { 1u, 2u, 3u };
    replay_r4_view v;

    (void)replay_r4_pack(good, (uint32_t)sizeof good, 9u, 1u, REPLAY_R4_L1, 2u, keys, 3u);

    memcpy(bad, good, sizeof good);
    bad[0] = (uint8_t)(bad[0] ^ 0x01u);
    ck("R4RP 魔数错", replay_r4_open(bad, 44u, &v) == REPLAY_R4_E_MAGIC);

    memcpy(bad, good, sizeof good);
    bad[4] = 2u;
    ck("R4RP 版本错", replay_r4_open(bad, 44u, &v) == REPLAY_R4_E_VERSION);

    memcpy(bad, good, sizeof good);
    bad[6] = 1u;
    ck("R4RP 标志错", replay_r4_open(bad, 44u, &v) == REPLAY_R4_E_FLAGS);

    memcpy(bad, good, sizeof good);
    bad[8] = 60u;
    ck("R4RP tick_hz 错", replay_r4_open(bad, 44u, &v) == REPLAY_R4_E_TICK);

    memcpy(bad, good, sizeof good);
    bad[20] = 0u;
    ck("R4RP 阶梯 0 拒", replay_r4_open(bad, 44u, &v) == REPLAY_R4_E_LADDER);

    memcpy(bad, good, sizeof good);
    bad[20] = 3u;
    ck("R4RP 阶梯 3 拒", replay_r4_open(bad, 44u, &v) == REPLAY_R4_E_LADDER);

    ck("R4RP 短一字节", replay_r4_open(good, 43u, &v) == REPLAY_R4_E_LEN);
    ck("R4RP 多一字节", replay_r4_open(good, 45u, &v) == REPLAY_R4_E_LEN);
    ck("R4RP 不够头部", replay_r4_open(good, 31u, &v) == REPLAY_R4_E_LEN);
    ck("R4RP 空指针", replay_r4_open(NULL, 44u, &v) == REPLAY_R4_E_LEN);
    ck("R4RP key_at 空视图", replay_r4_key_at(NULL, 0u, &keys[0]) != 0);
}

static void r3_writer(void)
{
    uint8_t       a[64];
    uint8_t       b[64];
    replay_r4_rec r;
    uint32_t      keys[5] = { 10u, 20u, 30u, 40u, 50u };
    uint32_t      n;
    uint32_t      i;

    n = replay_r4_pack(b, (uint32_t)sizeof b, 7u, 8u, REPLAY_R4_L1, 9u, keys, 5u);
    ck_u32("R4RP pack 长度", 32u + 20u, n);

    replay_r4_rec_begin(&r, a, (uint32_t)sizeof a, 7u, 8u, REPLAY_R4_L1, 9u);
    for (i = 0u; i < 5u; i++)
        ck("R4RP put", replay_r4_rec_put(&r, keys[i]) == 0);
    ck_u32("R4RP frames", 5u, replay_r4_rec_frames(&r));
    ck_u32("R4RP end", n, replay_r4_rec_end(&r));
    ck("R4RP 与 pack 逐字节相同", n != 0u && memcmp(a, b, n) == 0);

    /* 满了之后拒绝，且 tick 数不涨、满的状态是粘的 */
    replay_r4_rec_begin(&r, a, 32u + 8u, 5u, 6u, REPLAY_R4_L2, 7u);
    ck("R4RP 容量 2 tick put0", replay_r4_rec_put(&r, 1u) == 0);
    ck("R4RP 容量 2 tick put1", replay_r4_rec_put(&r, 2u) == 0);
    ck("R4RP 第三个被拒", replay_r4_rec_put(&r, 3u) != 0);
    ck_u32("R4RP tick 数停在 2", 2u, replay_r4_rec_frames(&r));
    ck("R4RP 满状态粘住", replay_r4_rec_put(&r, 4u) != 0);
    ck_u32("R4RP end 只算录到的", 32u + 8u, replay_r4_rec_end(&r));

    /* 非法阶梯：一开始就置满，不写缓冲 */
    replay_r4_rec_begin(&r, a, (uint32_t)sizeof a, 5u, 6u, 9u, 7u);
    ck("R4RP 非法阶梯 put 拒", replay_r4_rec_put(&r, 1u) != 0);
    ck_u32("R4RP 非法阶梯 end", 0u, replay_r4_rec_end(&r));

    /* 连头部都放不下 */
    replay_r4_rec_begin(&r, a, 8u, 5u, 6u, REPLAY_R4_L1, 7u);
    ck("R4RP 放不下头部 put", replay_r4_rec_put(&r, 1u) != 0);
    ck_u32("R4RP 放不下头部 end", 0u, replay_r4_rec_end(&r));
}

static void r4_text(void)
{
    char line[96];

    ck_u32("R4RP header 长度", 20u,
           replay_r4_text_header(7u, 8u, REPLAY_R4_L2, 9u, 256u, line, (uint32_t)sizeof line));
    ck("R4RP header 字面量", strcmp(line, "r4rp 1 7 8 2 9 256\r\n") == 0);

    ck_u32("R4RP key 长度", 18u, replay_r4_text_key(0u, 0x001CE0E7u, line, (uint32_t)sizeof line));
    ck("R4RP key 字面量", strcmp(line, "r4key 0 001ce0e7\r\n") == 0);

    /* 缓冲不够：一个字都不留，返回 0 */
    ck("R4RP 小缓冲被拒", replay_r4_text_key(0u, 1u, line, 4u) == 0u);
    ck("R4RP 头部小缓冲被拒",
       replay_r4_text_header(7u, 8u, REPLAY_R4_L1, 9u, 256u, line, 4u) == 0u);
}

uint32_t test_replay_r4_run(void)
{
    g_fail = 0u;

    say("libTest replay_r4: R4 录像格式\n");
    r1_roundtrip();
    r2_bad_input();
    r3_writer();
    r4_text();

    return g_fail;
}
