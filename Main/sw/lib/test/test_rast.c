/******************************************************************************
* @file test_rast.c: 数值与手算契约向量（普通 host.mk test 也执行）
******************************************************************************/
#include "test/rast_ref2.h"
#include "test/test_rast.h"
#include <limits.h>
#include <stdio.h>
#include <string.h>

static uint32_t pixels[2][640 * 16];
static uint16_t depths[2][640 * 16];
static unsigned failures;

static void expect(const char *name, int ok)
{
    if (!ok) { printf("RAST FAIL %s\n", name); ++failures; }
}

static void clear(void)
{
    for (unsigned j = 0; j < 2; ++j)
        for (unsigned i = 0; i < 640 * 16; ++i) {
            pixels[j][i] = 0x12345678; depths[j][i] = 65535;
        }
}

static void execute(rast_record *r, unsigned count, unsigned strip,
                     const uint32_t *texture, unsigned flags)
{
    rast_target t = {pixels[0], depths[0], 640, strip, texture, 2, 2, 2, 0xff000000};
    for (unsigned i = 0; i < count; ++i) rast_put32(&r[i], 0, flags);
    expect("incremental accepted", rast_ref_run(r, count, &t) == 0);
    t.color = pixels[1]; t.depth = depths[1];
    expect("direct accepted", rast_ref2_run(r, count, &t) == 0);
    expect("two executors color", memcmp(pixels[0], pixels[1], sizeof(pixels[0])) == 0);
    expect("two executors depth", memcmp(depths[0], depths[1], sizeof(depths[0])) == 0);
}

int test_rast_run(void)
{
    rast_record r[RAST_MAX_FAN], before;
    rast_vertex v[3] = {{0, 0, 100, 0, 0, 0xffff8040},
                         {64, 0, 100, 0, 0, 0xffff8040},
                         {0, 64, 100, 0, 0, 0xffff8040}};
    uint32_t tex[4] = {0xff000000, 0xff80ff40, 0xff4080ff, 0xffffffff};
    failures = 0;
    expect("setup flat top", rast_setup_polygon(v, 3, 2, r, RAST_MAX_FAN, NULL) == 1);
    clear(); execute(r, 1, 0, tex, 2);
    unsigned written = 0;
    for (unsigned i = 0; i < 640 * 16; ++i) written += depths[0][i] != 65535;
    expect("top/left included and right excluded: ten samples", written == 10);
    expect("color high byte zero", pixels[0][0] == 0x00ff8040);
    expect("right diagonal excluded", depths[0][4] == 65535);
    expect("top included", depths[0][3] == 100);
    expect("left included", depths[0][640 * 3] == 100);
    /* 共享对角线，另一半拥有它；4x4 方形共 16 点，外右/下边不拥有。 */
    rast_vertex second[3] = {v[1], {64, 64, 100, 0, 0, 0xffffffff}, v[2]};
    expect("second half setup", rast_setup_polygon(second, 3, 2, r + 1, 1, NULL) == 1);
    execute(r + 1, 1, 0, tex, 2);
    written = 0;
    for (unsigned i = 0; i < 640 * 16; ++i) written += depths[0][i] != 65535;
    expect("shared edge union=16", written == 16);
    /* 相等 LESS 不覆盖；色键在调制前比较且不能写 z。 */
    clear(); execute(r, 1, 0, tex, 7);
    expect("key leaves color and z", pixels[0][0] == 0x12345678 && depths[0][0] == 65535);
    execute(r, 1, 0, tex, 3);
    expect("key disabled writes black", pixels[0][0] == 0 && depths[0][0] == 100);
    rast_put32(r, 80, 0xffffffff);
    execute(r, 1, 0, tex, 2);
    expect("equal LESS leaves black", pixels[0][0] == 0);
    /* UV Q12.4 的 16 是 texel 1；负梯度 CLAMP 到 0，正端 CLAMP 到 size-1。 */
    clear(); rast_put16(r, 52, 16); rast_put16(r, 54, 65535);
    execute(r, 1, 0, tex, 1);
    expect("UV unit and upper clamp", pixels[0][0] == 0x00ffffff);
    rast_put32(r, 56, (uint32_t)-65536); clear();
    execute(r, 1, 0, tex, 1);
    expect("UV negative clamp", pixels[0][2] == 0x004080ff);
    for (unsigned i = 0; i < 3; ++i) v[i].y += 15 * 16;
    expect("cross strip setup", rast_setup_polygon(v, 3, 2, r, RAST_MAX_FAN, NULL) == 1);
    clear(); execute(r, 1, 1, tex, 2);
    expect("strip local y", depths[0][0] == 100 && depths[0][3] == 65535);
    uint8_t bins[256];
    expect("bin two strips", rast_bin(r, 1, bins, sizeof(bins)) == 124 && bins[0] == 1 && bins[6] == 1);
    memset(bins, 0xa5, sizeof(bins));
    expect("bin capacity atomic", rast_bin(r, 1, bins, 4) == -2 && bins[0] == 0xa5);
    expect("empty frame", rast_bin(NULL, 0, bins, sizeof(bins)) == 120 && bins[0] == 0);
    before = *r; rast_put32(r, 124, 1); clear();
    rast_target t = {pixels[0], depths[0], 640, 0, NULL, 0, 0, 0, 0};
    expect("reserved rejects before writes", rast_ref_run(r, 1, &t) == -1 && pixels[0][0] == 0x12345678);
    *r = before;
    v[2] = v[1];
    expect("degenerate culled", rast_setup_polygon(v, 3, 0, r, RAST_MAX_FAN, NULL) == 0);
    rast_vertex huge[3] = {{-60000, -60000, 10, 0, 0, 0xffffffff},
                             {60000, -60000, 10, 0, 0, 0xffffffff},
                             {0, 60000, 10, 0, 0, 0xffffffff}};
    int n = rast_setup_polygon(huge, 3, 0, r, RAST_MAX_FAN, NULL);
    expect("guard prevents s32 overflow", n > 0);
    for (int i = 0; i < n; ++i) expect("guard record valid", rast_record_valid(r + i));
    rast_vertex tiny[3] = {{0, 0, 0, 0, 0, 0}, {1, 0, 65535, 0, 0, 0}, {0, 1, 0, 0, 0, 0}};
    rast_setup_stats st = {0};
    expect("sliver counted", rast_setup_polygon(tiny, 3, 0, r, RAST_MAX_FAN, &st) == 0 && st.sliver == 1);
    /* 每条有理舍入式在 16 位余数域上验证正负半值，不只测几个常数。 */
    for (int64_t i = -65536; i < 65536; ++i) {
        int64_t want = i < -32768 ? -1 : i < 32768 ? 0 : 1;
        expect("Q16 rounding exhaustive", fx16_round_div(i, 65536) == want);
        expect("division exact", fx16_div((int32_t)i, 65536) == i);
        if (i) {
            int64_t reciprocal = fx16_round_div(INT64_C(1) << 32, i < 0 ? -i : i);
            fx16_t exact = fx16_sat(i < 0 ? -reciprocal : reciprocal);
            int64_t error = (int64_t)fx16_recip((int32_t)i) - exact;
            expect("reciprocal exact after residual correction", error == 0);
        }
    }
    expect("mul negative half", fx16_mul(-1, 32768) == 0);
    expect("round INT64_MIN without intermediate overflow",
           fx16_round_div(INT64_MIN, 3) == INT64_C(-3074457345618258603));
    expect("division zero defined", fx16_div(0, 0) == 0 && fx16_div(-1, 0) == INT32_MIN);
    fx16_mat4 identity, projection, product;
    fx16_identity(&identity);
    fx16_vec4 q = {{123, -456, INT32_MIN, 65536}}, answer;
    fx16_mat_vec(&identity, &q, &answer);
    expect("identity exact and alias safe", memcmp(&q, &answer, sizeof(q)) == 0);
    expect("perspective params", fx16_perspective(&projection, 16384, 65536, 65536, 9 * 65536) == 0);
    fx16_mat_mul(&projection, &identity, &product);
    expect("matrix product", memcmp(&projection, &product, sizeof(product)) == 0);
    q.v[0] = q.v[1] = 0; q.v[2] = -65536;
    fx16_mat_vec(&projection, &q, &answer);
    expect("near maps to minus one", fx16_div(answer.v[2], answer.v[3]) == -65536);
    q.v[2] = -9 * 65536; fx16_mat_vec(&projection, &q, &answer);
    expect("far maps to plus one", fx16_div(answer.v[2], answer.v[3]) == 65536);
    rast_view_vertex extreme[3] = {
        {-INT32_MAX, 65536, -65536, 0, 0, 0xffffffff},
        { INT32_MAX, 65536, -65536, 0, 0, 0xffffffff},
        {-INT32_MAX,-65536, -65536, 0, 0, 0xffffffff}};
    expect("full Q16 viewport reaches guard clip", rast_setup_view(extreme, &projection,
        65536, 9 * 65536, 0, r, RAST_MAX_FAN, NULL) > 0);
    rast_view_vertex converted = {0};
    for (int32_t i = -32768; i <= 32767; ++i) {
        expect("S3L 16-bit boundary exhaustive", rast_from_s3l(i, -i, i, &converted) == 0 &&
            converted.x == i * 128 && converted.y == -i * 128 && converted.z == -i * 128);
    }
    expect("S3L out of range", rast_from_s3l(INT32_MAX, 0, 0, &converted) == -1);
    /* 子视口：域检查（帧内矩形、正宽高）、视口外裁剪、右边最大格点、近远面组合。 */
    {
        const rast_viewport bad[5] = {{-1, 0, 4, 4}, {0, -1, 4, 4}, {2, 3, 0, 4},
                                      {2, 3, 4, 0}, {630, 0, 20, 4}};
        const rast_viewport vp = {2, 3, 4, 4};
        rast_vertex inside[3] = {{32, 48, 100, 0, 0, 0xffffffff},
                                 {160, 48, 100, 0, 0, 0xffffffff},
                                 {32, 160, 100, 0, 0, 0xffffffff}};
        for (unsigned i = 0; i < 5u; ++i)
            expect("viewport domain rejected",
                   rast_setup_polygon_vp(inside, 3, &bad[i], 2, r, RAST_MAX_FAN, NULL) == -1);
        expect("viewport null rejected",
               rast_setup_polygon_vp(inside, 3, NULL, 2, r, RAST_MAX_FAN, NULL) == -1);
        expect("viewport view null rejected",
               rast_setup_view_vp(NULL, 3u, &projection, 65536, 9 * 65536, &vp, 2,
                                  r, RAST_MAX_FAN, NULL) == -1);
        int n = rast_setup_polygon_vp(inside, 3, &vp, 2, r, RAST_MAX_FAN, NULL);
        /* 右边在回绕边与出边各产生一个相同交点，重复点扇形面积为 0 被剔除：
           实际是四边形 v0-(95,105)-(88,111)-(32,111)，两条记录。 */
        expect("vp outside clipped", n == 2);
        for (int i = 0; i < n; ++i) {
            int16_t x0 = (int16_t)rast_get16(r + i, 72), x1 = (int16_t)rast_get16(r + i, 74);
            int16_t y0 = (int16_t)rast_get16(r + i, 76), y1 = (int16_t)rast_get16(r + i, 78);
            expect("vp bbox inside rect", x0 >= 2 && x1 <= 5 && y0 >= 3 && y1 <= 6);
            expect("vp record valid", rast_record_valid(r + i));
        }
        /* 右边界：最大格点 16*(x+w)-1 拥有该列；再往外一个格点即被裁掉。 */
        rast_vertex last[3] = {{80, 48, 100, 0, 0, 0xffffffff},
                               {95, 48, 100, 0, 0, 0xffffffff},
                               {80, 63, 100, 0, 0, 0xffffffff}};
        const rast_viewport tight = {5, 3, 1, 1};
        expect("vp last grid point owned",
               rast_setup_polygon_vp(last, 3, &tight, 2, r, RAST_MAX_FAN, NULL) == 1 &&
               rast_get16(r, 72) == 5 && rast_get16(r, 74) == 5 &&
               rast_get16(r, 76) == 3 && rast_get16(r, 78) == 3);
        rast_vertex beyond[3] = {{80, 48, 100, 0, 0, 0xffffffff},
                                 {96, 48, 100, 0, 0, 0xffffffff},
                                 {80, 63, 100, 0, 0, 0xffffffff}};
        n = rast_setup_polygon_vp(beyond, 3, &tight, 2, r, RAST_MAX_FAN, NULL);
        expect("vp beyond clipped to column", n == 2);
        for (int i = 0; i < n; ++i)
            expect("vp clipped column bound", rast_get16(r + i, 72) == 5 &&
                   rast_get16(r + i, 74) == 5 && rast_record_valid(r + i));
        /* 视空间：近面保持 z<=-near（端点恰在平面上保留）、远面 z>=-far；子视口内 bbox。 */
        rast_view_vertex tri[3] = {{-65536, 65536, -65536, 0, 0, 0xffffffff},
                                   {65536, 65536, -65536, 16, 0, 0xff80ff40},
                                   {0, -65536, -131072, 0, 16, 0xff4080ff}};
        const rast_viewport center = {160, 120, 320, 240};
        n = rast_setup_view_vp(tri, 3u, &projection, 65536, 131072, &center, 3,
                               r, RAST_MAX_FAN, NULL);
        expect("view vp near/far endpoints kept", n > 0);
        for (int i = 0; i < n; ++i) {
            int16_t x0 = (int16_t)rast_get16(r + i, 72), x1 = (int16_t)rast_get16(r + i, 74);
            int16_t y0 = (int16_t)rast_get16(r + i, 76), y1 = (int16_t)rast_get16(r + i, 78);
            expect("view vp bbox inside", x0 >= 160 && x1 < 480 && y0 >= 120 && y1 < 360);
            expect("view vp record valid", rast_record_valid(r + i));
        }
        expect("view vp near clips all",
               rast_setup_view_vp(tri, 3u, &projection, 131072, 262144, &center, 3,
                                  r, RAST_MAX_FAN, NULL) == 0);
    }
    printf("RAST directed/math fail %u\n", failures);
    return (int)failures;
}
