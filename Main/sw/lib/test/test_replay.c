/******************************************************************************
* @file test_replay.c: 录像格式的主机侧测试
*
* @brief  五组：头部字节序、坏输入、流式录制与一次打包等价、录制文本的字面量、
*         以及入库固件的三方对账。
*
*         最后那一组是 Engineering/TechReports/sw/03-PC平台层与帧节拍.md 5.2 节第 7 条判据的本体：同一份录像经
*         `Main/tools/check/replay2c.py` 变成 C 数组之后，C 数组解析出的按键序列
*         必须与直接解析 `.g2dr` 的文件一致；`make -f mk/host.mk test` 会在
*         64 位与 `-m32` 下各编一份，两份输出必须逐字节相同。
*
*         固件那一段还顺手把脚本里的 xorshift32 与 `lib/core/xorshift32.c` 对上：
*         按固件头部记的种子重算一遍按键序列，打包结果必须与固件逐字节相同。
*         脚本是第二份实现，两份不一致时这里会红。
******************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/format.h"
#include "core/replay.h"
#include "core/xorshift32.h"
#include "test_replay.h"

/* C 数组那一半要 `Main/tools/check/replay2c.py` 生成的 replay_fixture.h，
   它在 $(OBJDIR)/test/ 里、不入库。但 `lib/` 有一条结构判据要求每个文件
   在「只有 -Isw/lib」的条件下也编得过（check_sw_layers.sh 第 1 项），
   所以这里用 __has_include 让缺头文件时仍然编得过，缺了就**在运行里报 FAIL**
   （不静默跳过：make test 一定会先生成它，缺了就是哪里不对）。 */
#if defined(__has_include)
#  if __has_include("replay_fixture.h")
#    include "replay_fixture.h"
#    define HAVE_REPLAY_FIXTURE 1
#  endif
#endif
#ifndef HAVE_REPLAY_FIXTURE
#define HAVE_REPLAY_FIXTURE 0
#endif

/* 固件读取用的缓冲。256 帧的固件是 1040 字节，留足余量。 */
#define FIXTURE_MAX_BYTES   (1u << 20)

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

/* --- R1 头部与往返 ------------------------------------------------------- */

static void r1_roundtrip(void)
{
    uint8_t     buf[64];
    uint32_t    keys[3] = { 0x00000001u, 0x00000200u, 0xFFFFFFFFu };
    replay_view v;
    uint32_t    got;
    uint32_t    i;

    ck_u32("R1 bytes_for 3", 16u + 12u, replay_bytes_for(3u));
    ck_u32("R1 bytes_for 0", 16u, replay_bytes_for(0u));
    ck_u32("R1 len", 28u, replay_pack(buf, (uint32_t)sizeof buf, 9u, keys, 3u));

    /* 头部逐字节核：魔数四个 ASCII、版本与标志各两个字节、种子与帧数小端。
       这一条把「显式小端」从注释变成判据。 */
    ck("R1 魔数", buf[0] == 'G' && buf[1] == '2' && buf[2] == 'D' && buf[3] == 'R');
    ck("R1 版本", buf[4] == 1u && buf[5] == 0u);
    ck("R1 标志", buf[6] == 0u && buf[7] == 0u);
    ck("R1 种子小端", buf[8] == 9u && buf[9] == 0u && buf[10] == 0u && buf[11] == 0u);
    ck("R1 帧数小端", buf[12] == 3u && buf[13] == 0u && buf[14] == 0u && buf[15] == 0u);
    ck("R1 第 0 帧小端",
       buf[16] == 0x01u && buf[17] == 0u && buf[18] == 0u && buf[19] == 0u);
    ck("R1 第 2 帧小端",
       buf[24] == 0xFFu && buf[25] == 0xFFu && buf[26] == 0xFFu && buf[27] == 0xFFu);

    ck("R1 open", replay_open(buf, 28u, &v) == 0);
    ck_u32("R1 seed", 9u, v.seed);
    ck_u32("R1 frames", 3u, v.frames);
    for (i = 0u; i < 3u; i++) {
        got = 0u;
        ck("R1 key_at", replay_key_at(&v, i, &got) == 0);
        ck_u32("R1 key", keys[i], got);
    }
    got = 0x1234u;
    ck("R1 越界被拒", replay_key_at(&v, 3u, &got) != 0 && got == 0x1234u);

    /* 差一个字节都不打包：截断过的录像没有意义 */
    ck("R1 cap 差一字节", replay_pack(buf, 27u, 9u, keys, 3u) == 0u);
    ck("R1 空指针", replay_pack(NULL, 28u, 9u, keys, 3u) == 0u);
}

/* --- R2 坏输入 ----------------------------------------------------------- */

static void r2_bad_input(void)
{
    uint8_t     good[28];
    uint8_t     bad[32];
    uint32_t    keys[3] = { 1u, 2u, 3u };
    replay_view v;

    (void)replay_pack(good, (uint32_t)sizeof good, 9u, keys, 3u);

    memcpy(bad, good, sizeof good);
    bad[0] = (uint8_t)(bad[0] ^ 0x01u);
    ck("R2 魔数错", replay_open(bad, 28u, &v) == REPLAY_E_MAGIC);

    memcpy(bad, good, sizeof good);
    bad[4] = 2u;
    ck("R2 版本错", replay_open(bad, 28u, &v) == REPLAY_E_VERSION);

    memcpy(bad, good, sizeof good);
    bad[6] = 1u;
    ck("R2 标志错", replay_open(bad, 28u, &v) == REPLAY_E_FLAGS);

    ck("R2 短一字节", replay_open(good, 27u, &v) == REPLAY_E_LEN);
    ck("R2 多一字节", replay_open(good, 29u, &v) == REPLAY_E_LEN);
    ck("R2 不够头部", replay_open(good, 15u, &v) == REPLAY_E_LEN);
    ck("R2 空指针", replay_open(NULL, 28u, &v) == REPLAY_E_LEN);
    ck("R2 key_at 空视图", replay_key_at(NULL, 0u, &keys[0]) != 0);
}

/* --- R3 流式录制与一次打包等价 ------------------------------------------- */

static void r3_writer(void)
{
    uint8_t    a[64];
    uint8_t    b[64];
    replay_rec r;
    uint32_t   keys[5];
    uint32_t   n;
    xs32       g;
    uint32_t   i;

    xs32_seed(&g, 0x1234u);
    for (i = 0u; i < 5u; i++)
        keys[i] = xs32_next(&g);

    n = replay_pack(b, (uint32_t)sizeof b, 0x1234u, keys, 5u);
    ck_u32("R3 pack 长度", 16u + 20u, n);

    replay_rec_begin(&r, a, (uint32_t)sizeof a, 0x1234u);
    for (i = 0u; i < 5u; i++)
        ck("R3 put", replay_rec_put(&r, keys[i]) == 0);
    ck_u32("R3 frames", 5u, replay_rec_frames(&r));
    ck_u32("R3 end", n, replay_rec_end(&r));
    ck("R3 与 pack 逐字节相同", n != 0u && memcmp(a, b, n) == 0);

    /* 满了之后拒绝，且帧数不涨、满的状态是粘的 */
    replay_rec_begin(&r, a, 16u + 8u, 5u);
    ck("R3 容量 2 帧 put0", replay_rec_put(&r, 1u) == 0);
    ck("R3 容量 2 帧 put1", replay_rec_put(&r, 2u) == 0);
    ck("R3 第三帧被拒", replay_rec_put(&r, 3u) != 0);
    ck_u32("R3 帧数停在 2", 2u, replay_rec_frames(&r));
    ck("R3 满状态粘住", replay_rec_put(&r, 4u) != 0);
    ck_u32("R3 end 只算录到的", 16u + 8u, replay_rec_end(&r));

    /* 连头部都放不下：不写、也不越界 */
    replay_rec_begin(&r, a, 8u, 5u);
    ck("R3 放不下头部 put", replay_rec_put(&r, 1u) != 0);
    ck_u32("R3 放不下头部 end", 0u, replay_rec_end(&r));
}

/* --- R4 录制文本 --------------------------------------------------------- */

static void r4_text(void)
{
    char line[64];

    ck_u32("R4 header 长度", 14u, replay_text_header(7u, 256u, line, (uint32_t)sizeof line));
    ck("R4 header 字面量", strcmp(line, "g2dr 1 7 256\r\n") == 0);

    ck_u32("R4 key 长度", 16u, replay_text_key(0u, 0x001CE0E7u, line, (uint32_t)sizeof line));
    ck("R4 key 字面量", strcmp(line, "key 0 001ce0e7\r\n") == 0);

    ck_u32("R4 key 大帧号", 20u, replay_text_key(12345u, 0xFFFFFFFFu, line, (uint32_t)sizeof line));
    ck("R4 key 大帧号字面量", strcmp(line, "key 12345 ffffffff\r\n") == 0);

    /* 缓冲不够：一个字都不留，返回 0 */
    ck("R4 小缓冲被拒", replay_text_key(0u, 1u, line, 4u) == 0u);
    ck("R4 头部小缓冲被拒", replay_text_header(7u, 256u, line, 4u) == 0u);
}

/* --- R5 固件三方对账 ----------------------------------------------------- */

static void r5_fixture(const char *path)
{
    static uint8_t buf[FIXTURE_MAX_BYTES];
    FILE          *f;
    size_t         n;
    replay_view    vf;
    replay_view    vh;
    uint32_t      *keys;
    uint8_t       *packed;
    uint32_t       need;
    uint32_t       mismatch = 0u;
    uint32_t       i;
    uint32_t       k, kg;
    xs32           g;

    (void)kg;
    (void)vh;

    f = fopen(path, "rb");
    if (f == NULL) {
        char     line[160];
        core_out o;

        core_out_init(&o, line, (uint32_t)sizeof line);
        core_out_str(&o, "R5 打不开固件 ");
        core_out_str(&o, path);
        core_out_str(&o, " FAIL\n");
        say(line);
        g_fail++;
        return;
    }
    n = fread(buf, 1u, (size_t)sizeof buf, f);
    fclose(f);

    ck("R5 文件解析", replay_open(buf, (uint32_t)n, &vf) == 0);

#if HAVE_REPLAY_FIXTURE
    ck("R5 C 数组解析", replay_open(replay_fixture_bytes, REPLAY_FIXTURE_LEN, &vh) == 0);
    ck_u32("R5 长度一致", (uint32_t)n, REPLAY_FIXTURE_LEN);
    ck("R5 字节一致", (uint32_t)n == REPLAY_FIXTURE_LEN &&
                      memcmp(buf, replay_fixture_bytes, n) == 0);
    ck_u32("R5 帧数一致", vf.frames, vh.frames);
    ck_u32("R5 种子一致", vf.seed, vh.seed);
#else
    say("R5 没有 replay_fixture.h（先跑 make -f mk/host.mk test 生成它） FAIL\n");
    g_fail++;
#endif

    need   = replay_bytes_for(vf.frames);
    keys   = (uint32_t *)malloc(vf.frames * 4u + 4u);
    packed = (uint8_t *)malloc(need + 4u);
    if (keys == NULL || packed == NULL) {
        say("R5 内存不够 FAIL\n");
        g_fail++;
        free(keys);
        free(packed);
        return;
    }

    /* 按固件头部记的种子重算按键序列，再打包：与固件逐字节比。
       这一条同时把脚本里的 xorshift32 与 lib/core/xorshift32.c 对上。 */
    xs32_seed(&g, vf.seed);
    for (i = 0u; i < vf.frames; i++)
        keys[i] = xs32_next(&g);
    ck_u32("R5 重打包长度", need, replay_pack(packed, need, vf.seed, keys, vf.frames));
    ck("R5 与 xorshift32 重算的字节一致", memcmp(packed, buf, need) == 0);

    for (i = 0u; i < vf.frames; i++) {
        k  = 0u;
        kg = 0u;
        if (replay_key_at(&vf, i, &k) != 0
#if HAVE_REPLAY_FIXTURE
            || replay_key_at(&vh, i, &kg) != 0
#endif
           )
            mismatch++;
#if HAVE_REPLAY_FIXTURE
        else if (k != kg || k != keys[i])
#else
        else if (k != keys[i])
#endif
            mismatch++;
    }
    ck_u32("R5 逐帧不同的帧数", 0u, mismatch);

    free(keys);
    free(packed);
}

uint32_t test_replay_run(const char *fixture)
{
    g_fail = 0u;

    say("libTest replay: 录像格式\n");
    r1_roundtrip();
    r2_bad_input();
    r3_writer();
    r4_text();
    r5_fixture(fixture);

    return g_fail;
}
