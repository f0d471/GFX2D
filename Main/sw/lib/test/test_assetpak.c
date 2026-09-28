/******************************************************************************
* @file test_assetpak.c: 素材包解析器的主机侧单元测试
*
* @brief  这里**自己手写**一个小包（不用打包工具），把 assetpak.h 的每条规则
*         逐条注错，看它报的是不是指定的那个码、fail_id 是不是那一条。
*         打包工具的产物不拿来当输入：那样只能证明两者互相一致，
*         同一个错误会一起错过去（spec 第 01 章 4.1 节的教训）。
*
*         手算的包布局（与 sw 第 04 章 3.6 节一致）：
*           头 32 字节；目录 2 项 × 32 = 64 字节，到 96；
*           第 0 项 id=0x11，3x2，stride_log2=4（行跨 16、像素 12、行尾 4 字节 0），
*                    offset=128、length=32；
*           第 1 项 id=0x22，1x1，stride_log2=2，带色键 0x00112233，
*                    offset=192、length=4；
*           总长 196。
*         第 0 项特意取宽 3，让行跨（16）大于像素（12），行尾那 4 个字节才有东西可验。
******************************************************************************/

#include <stdio.h>
#include <string.h>

#include "core/assetpak.h"
#include "core/crc32.h"
#include "test_assetpak.h"

#define BUF_BYTES   512u
#define ENT0_OFF    128u
#define ENT0_LEN    32u
#define ENT1_OFF    192u
#define ENT1_LEN    4u
#define TOTAL       196u
#define DIR_BYTES   (2u * ASSETPAK_DIR_ENTRY)
#define DATA_START  (ASSETPAK_HEADER_LEN + DIR_BYTES)

static uint8_t  g_buf[BUF_BYTES];
static uint32_t g_fail;

static void out(const char *s)
{
    fputs(s, stdout);
}

static void check(const char *tag, int ok)
{
    out(tag);
    out(ok ? " ok\n" : " FAIL\n");
    if (!ok)
        g_fail++;
}

static void check_code(const char *tag, int expect, int got)
{
    char line[128];

    snprintf(line, sizeof line, "%s expect=%s got=%s %s\n", tag,
             assetpak_strerror(expect), assetpak_strerror(got),
             (expect == got) ? "ok" : "FAIL");
    out(line);
    if (expect != got)
        g_fail++;
}

/* --- 手写一个包 ---------------------------------------------------------- */

enum {
    F_NONE = 0, F_MAGIC, F_VERSION, F_HEADER_LEN, F_RSVD, F_COUNT_ZERO, F_COUNT_BIG,
    F_DIR_BYTES, F_TOTAL_SHORT, F_TOTAL_BIG, F_ENT_RSVD, F_FLAGS_BIT, F_KEY_NOFLAG,
    F_KEY_BAD_FMT,
    F_FORMAT, F_W0, F_SL_LOW, F_SL_HIGH, F_W_GT_STRIDE, F_LEN, F_OFF_ALIGN,
    F_OFF_LOW, F_BOUNDS, F_DUP_ID, F_OVERLAP, F_DATA_FLIP, F_PACK_CRC
};

static void wr16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

/* 整包 CRC：头里那 4 个字节（偏移 20）按 0 参与计算，与打包工具一致。 */
static uint32_t pack_crc_of(uint32_t total)
{
    crc32_ctx ctx;
    uint8_t   zero[4] = { 0u, 0u, 0u, 0u };

    crc32_begin(&ctx);
    crc32_update(&ctx, g_buf, 20u);
    crc32_update(&ctx, zero, 4u);
    crc32_update(&ctx, g_buf + 24u, total - 24u);
    return crc32_end(&ctx);
}

static void entry(uint8_t *buf, uint32_t i, uint32_t id, uint32_t off, uint32_t len,
                  uint32_t w, uint32_t h, uint32_t sl, uint32_t fmt, uint32_t flags,
                  uint32_t key)
{
    uint8_t *e = buf + ASSETPAK_HEADER_LEN + i * ASSETPAK_DIR_ENTRY;

    wr32(e + 0, id);
    wr32(e + 4, off);
    wr32(e + 8, len);
    wr16(e + 12, w);
    wr16(e + 14, h);
    e[16] = (uint8_t)sl;
    e[17] = (uint8_t)fmt;
    wr16(e + 18, flags);
    wr32(e + 20, key);
    wr32(e + 24, 0u);           /* 数据 CRC 最后填 */
    wr32(e + 28, 0u);
}

/* 建一个包；fault 指定要注的错。返回头里写的包总长。 */
static uint32_t build(uint32_t fault)
{
    uint32_t count = 2u;
    uint32_t total = TOTAL;
    uint32_t e0off = ENT0_OFF, e1off = ENT1_OFF, e0len = ENT0_LEN;
    uint32_t e0id = 0x11u, e1id = 0x22u;
    uint32_t e0sl = 4u, e1sl = 2u, e0fmt = ASSETPAK_FMT_ARGB8888_PM;
    uint32_t e1w = 1u;
    uint32_t e0flags = 0u, e1flags = ASSETPAK_FLAG_KEY;
    uint32_t e0key = 0u, e1key = 0x00112233u;
    uint32_t rsvd = 0u, dirb = DIR_BYTES, vlen = ASSETPAK_HEADER_LEN;
    uint32_t version = ASSETPAK_VERSION;
    uint32_t magic = ASSETPAK_MAGIC;
    uint32_t tail_zero = 0u;
    static const uint8_t px[24] = {
        0x01, 0x02, 0x03, 0x04,  0x11, 0x12, 0x13, 0x14,
        0x21, 0x22, 0x23, 0x24,  0x31, 0x32, 0x33, 0x34,
        0x41, 0x42, 0x43, 0x44,  0x51, 0x52, 0x53, 0x54
    };

    memset(g_buf, 0, sizeof g_buf);

    switch (fault) {
    case F_MAGIC:        magic = 0x12345678u; break;
    case F_VERSION:      version = 2u; break;
    case F_HEADER_LEN:   vlen = 28u; break;
    case F_RSVD:         rsvd = 1u; break;
    case F_COUNT_ZERO:   count = 0u; break;
    case F_COUNT_BIG:    count = ASSETPAK_MAX_ENTRIES + 1u; break;
    case F_DIR_BYTES:    dirb = DIR_BYTES + 32u; break;
    case F_TOTAL_SHORT:  total = ASSETPAK_HEADER_LEN + DIR_BYTES - 1u; break;
    case F_TOTAL_BIG:    total = BUF_BYTES + 64u; break;
    case F_ENT_RSVD:     tail_zero = 4u; break;          /* 第 0 项的保留字段 */
    case F_FLAGS_BIT:    e0flags = 0x0002u; break;
    case F_KEY_NOFLAG:   e1flags = 0u; break;            /* 有色键字却没标志 */
    case F_KEY_BAD_FMT:  e0flags = ASSETPAK_FLAG_KEY; e0key = 0x00112233u; break;
    case F_FORMAT:       e0fmt = 3u; break;              /* INDEX8 */
    case F_W0:           e1w = 0u; break;
    case F_SL_LOW:       e1sl = 1u; break;
    case F_SL_HIGH:      e1sl = 32u; break;
    case F_W_GT_STRIDE:  e1w = 8u; break;                /* 8*4=32 > 2^4 */
    case F_LEN:          e0len = ENT0_LEN + 4u; break;
    case F_OFF_ALIGN:    e0off = ENT0_OFF + 1u; break;
    case F_OFF_LOW:      e0off = DATA_START - 32u; break;
    case F_BOUNDS:       e0off = 0x1000u; break;
    case F_DUP_ID:       e1id = e0id; break;
    case F_OVERLAP:      e1off = ENT0_OFF; break;
    default:             break;
    }

    wr32(g_buf + 0, magic);
    wr32(g_buf + 4, version);
    wr32(g_buf + 8, vlen);
    wr32(g_buf + 12, count);
    wr32(g_buf + 16, total);
    wr32(g_buf + 20, 0u);
    wr32(g_buf + 24, dirb);
    wr32(g_buf + 28, rsvd);

    entry(g_buf, 0, e0id, e0off, e0len, 3u, 2u, e0sl, e0fmt, e0flags, e0key);
    entry(g_buf, 1, e1id, e1off, ENT1_LEN, e1w, 1u, e1sl,
          ASSETPAK_FMT_XRGB8888, e1flags, e1key);
    if (tail_zero != 0u)
        wr32(g_buf + ASSETPAK_HEADER_LEN + 28u, tail_zero);

    /* 像素数据始终落在物理位置 ENT0_OFF / ENT1_OFF 上：
       注错只改目录里记的偏移，不改数据放在哪，否则注错会越界写测试缓冲。 */
    memcpy(g_buf + ENT0_OFF, px, 12u);          /* 第 0 行 3 个像素 */
    memcpy(g_buf + ENT0_OFF + 16u, px + 12u, 12u);   /* 第 1 行 3 个像素 */
    g_buf[ENT1_OFF + 0] = 0x33u;
    g_buf[ENT1_OFF + 1] = 0x22u;
    g_buf[ENT1_OFF + 2] = 0x11u;
    g_buf[ENT1_OFF + 3] = 0x00u;

    wr32(g_buf + ASSETPAK_HEADER_LEN + 24u, crc32_ieee(g_buf + ENT0_OFF, ENT0_LEN));
    wr32(g_buf + ASSETPAK_HEADER_LEN + ASSETPAK_DIR_ENTRY + 24u,
         crc32_ieee(g_buf + ENT1_OFF, ENT1_LEN));

    if (fault == F_DATA_FLIP)
        g_buf[ENT0_OFF + 5] ^= 0xFFu;

    /* total 比缓冲还大时（F_TOTAL_BIG）解析会在头这一步就拒绝，
       根本走不到 CRC，此时不要把整包 CRC 算到缓冲外面去。 */
    wr32(g_buf + 20u, (total <= BUF_BYTES) ? pack_crc_of(total) : 0u);
    if (fault == F_PACK_CRC)
        g_buf[20] ^= 0xFFu;

    return total;
}

/* --- 正常路径 ------------------------------------------------------------ */

static void t_valid(void)
{
    assetpak        pak;
    assetpak_entry  e;
    assetpak_limits lim;
    uint32_t        total = 0u, count = 0u;
    int             rc;

    build(F_NONE);
    lim.max_total = 16u * 1024u * 1024u;

    rc = assetpak_probe_header(g_buf, ASSETPAK_HEADER_LEN, &lim, &total, &count);
    check_code("A1 probe_header 合法包", ASSETPAK_OK, rc);
    check("A1 probe 总长与条目数", total == TOTAL && count == 2u);

    rc = assetpak_parse(&pak, g_buf, TOTAL, &lim);
    check_code("A2 parse 合法包", ASSETPAK_OK, rc);
    if (rc != ASSETPAK_OK)
        return;

    check("A2 条目数与目录字节数", pak.entry_count == 2u && pak.dir_bytes == DIR_BYTES);
    check("A2 总长与整包 CRC", pak.total_len == TOTAL && pak.fail_index == ASSETPAK_NO_INDEX);

    rc = assetpak_entry_at(&pak, 0u, &e);
    check_code("A3 entry_at 0", ASSETPAK_OK, rc);
    check("A3 第 0 项字段",
          e.id == 0x11u && e.offset == ENT0_OFF && e.length == ENT0_LEN
          && e.width == 3u && e.height == 2u && e.stride_log2 == 4u
          && e.format == ASSETPAK_FMT_ARGB8888_PM && e.flags == 0u && e.key == 0u);
    check("A3 stride 与数据长", assetpak_stride(&e) == 16u
          && e.length == (uint32_t)e.height * assetpak_stride(&e));

    {
        const uint8_t *d = assetpak_data(&pak, &e);
        uint32_t       i, tail = 0u;

        check("A4 数据指针", d == g_buf + ENT0_OFF);
        /* 行内 3 个像素：首像素、第二像素、末像素（边界像素） */
        check("A4 首像素逐字节（B,G,R,A）",
              d[0] == 0x01u && d[1] == 0x02u && d[2] == 0x03u && d[3] == 0x04u);
        check("A4 行内末像素", d[8] == 0x21u && d[11] == 0x24u);
        /* 第二行首/末像素：行首在 stride 处，不是 width*4 处 */
        check("A4 第二行首像素在 stride 偏移上",
              d[16] == 0x31u && d[19] == 0x34u);
        check("A4 第二行末像素", d[24] == 0x51u && d[27] == 0x54u);
        for (i = 0u; i < 4u; i++) {
            if (d[12u + i] != 0u)       /* 第 0 行行尾 */
                tail++;
            if (d[28u + i] != 0u)       /* 第 1 行行尾 */
                tail++;
        }
        check("A4 两行行尾共 8 字节全 0", tail == 0u);
    }

    rc = assetpak_find(&pak, 0x22u, &e);
    check_code("A5 find 0x22", ASSETPAK_OK, rc);
    check("A5 第 1 项字段",
          e.width == 1u && e.height == 1u && e.stride_log2 == 2u
          && e.format == ASSETPAK_FMT_XRGB8888
          && (e.flags & ASSETPAK_FLAG_KEY) != 0u && e.key == 0x00112233u);
    {
        const uint8_t *d = assetpak_data(&pak, &e);
        check("A5 色键字就是它的存储字",
              d[0] == 0x33u && d[1] == 0x22u && d[2] == 0x11u && d[3] == 0x00u);
    }

    rc = assetpak_find(&pak, 0x99u, &e);
    check_code("A6 find 不存在的 ID", ASSETPAK_E_NOTFOUND, rc);
    rc = assetpak_entry_at(&pak, 2u, &e);
    check_code("A6 entry_at 越界", ASSETPAK_E_NOTFOUND, rc);
    {
        assetpak_entry bad;
        memset(&bad, 0, sizeof bad);
        bad.stride_log2 = 32u;
        check("A7 stride_log2=32 时 stride 返回 0", assetpak_stride(&bad) == 0u);
    }
}

/* --- 注错 ---------------------------------------------------------------- */

static void t_faults(void)
{
    static const struct {
        uint32_t    fault;
        int         expect;
        const char *tag;
    } cases[] = {
        { F_MAGIC,       ASSETPAK_E_MAGIC,     "B01 magic 不符" },
        { F_VERSION,     ASSETPAK_E_VERSION,   "B02 版本不符" },
        { F_HEADER_LEN,  ASSETPAK_E_HEADER,    "B03 头长不符" },
        { F_RSVD,        ASSETPAK_E_HEADER,    "B04 头保留字段非 0" },
        { F_COUNT_ZERO,  ASSETPAK_E_COUNT,     "B05 条目数为 0" },
        { F_COUNT_BIG,   ASSETPAK_E_COUNT,     "B06 条目数超上限" },
        { F_DIR_BYTES,   ASSETPAK_E_HEADER,    "B07 目录字节数与条目数不符" },
        { F_TOTAL_SHORT, ASSETPAK_E_HEADER,    "B08 总长小于头加目录" },
        { F_TOTAL_BIG,   ASSETPAK_E_TOTAL,     "B09 总长超过传入长度" },
        { F_ENT_RSVD,    ASSETPAK_E_HEADER,    "B10 条目保留字段非 0" },
        { F_FLAGS_BIT,   ASSETPAK_E_FLAGS,     "B11 标志位越界" },
        { F_KEY_NOFLAG,  ASSETPAK_E_FLAGS,     "B12 无色键标志却有键字" },
        { F_KEY_BAD_FMT, ASSETPAK_E_FLAGS,     "B12a 预乘条目不能带色键" },
        { F_FORMAT,      ASSETPAK_E_FORMAT,    "B13 INDEX8 格式被拒" },
        { F_W0,          ASSETPAK_E_GEOM,      "B14 宽度为 0" },
        { F_SL_LOW,      ASSETPAK_E_GEOM,      "B15 stride_log2 低于下限 2" },
        { F_SL_HIGH,     ASSETPAK_E_GEOM,      "B16 stride_log2 高于上限 31" },
        { F_W_GT_STRIDE, ASSETPAK_E_GEOM,      "B17 宽度乘 4 大于行跨" },
        { F_LEN,         ASSETPAK_E_GEOM,      "B18 数据长与行跨不符" },
        { F_OFF_ALIGN,   ASSETPAK_E_OFFSET,    "B19 偏移未按 64 对齐" },
        { F_OFF_LOW,     ASSETPAK_E_OFFSET,    "B20 偏移落进头或目录" },
        { F_BOUNDS,      ASSETPAK_E_BOUNDS,    "B21 越过包末端" },
        { F_DUP_ID,      ASSETPAK_E_DUP_ID,    "B22 ID 重复" },
        { F_OVERLAP,     ASSETPAK_E_OVERLAP,   "B23 数据区重叠" },
        { F_DATA_FLIP,   ASSETPAK_E_ENTRY_CRC, "B24 数据翻一位" },
        { F_PACK_CRC,    ASSETPAK_E_PACK_CRC,  "B25 整包 CRC 不符" },
    };
    assetpak        pak;
    assetpak_limits lim;
    uint32_t        i;

    lim.max_total = 16u * 1024u * 1024u;

    for (i = 0u; i < sizeof cases / sizeof cases[0]; i++) {
        uint32_t total = build(cases[i].fault);
        /* 调用方手里最多只有 BUF_BYTES 个字节：F_TOTAL_BIG 的头里写了比它更大的总长，
           这一条测的正是「头里的总长超过实际可读字节数」。 */
        uint32_t len   = (total <= BUF_BYTES) ? total : BUF_BYTES;
        int      rc    = assetpak_parse(&pak, g_buf, len, &lim);

        check_code(cases[i].tag, cases[i].expect, rc);
        if (cases[i].fault == F_DATA_FLIP && rc == ASSETPAK_E_ENTRY_CRC)
            check("B24 报的是出错的那一项", pak.fail_index == 0u && pak.fail_id == 0x11u);
    }

    /* 截断：可读字节数比头里写的总长少一个 */
    {
        uint32_t total = build(F_NONE);
        int      rc    = assetpak_parse(&pak, g_buf, total - 1u, &lim);

        check_code("B26 传入长度比总长少 1", ASSETPAK_E_TOTAL, rc);
    }

    /* 运行预算收紧（16 MiB DDR 边界、E6 之后的更小预算都是这一条） */
    {
        uint32_t       total = build(F_NONE);
        assetpak_limits small;
        int            rc;

        small.max_total = 100u;
        rc = assetpak_parse(&pak, g_buf, total, &small);
        check_code("B27 预算收到 100 字节", ASSETPAK_E_TOTAL, rc);

        rc = assetpak_probe_header(g_buf, ASSETPAK_HEADER_LEN, &small, 0, 0);
        check_code("B27 probe_header 同样受预算限制", ASSETPAK_E_TOTAL, rc);
    }

    /* 头不足 32 字节 */
    {
        build(F_NONE);
        check_code("B28 头不足 32 字节", ASSETPAK_E_HEADER,
                   assetpak_probe_header(g_buf, ASSETPAK_HEADER_LEN - 1u, &lim, 0, 0));
    }

    /* 参数检查 */
    {
        build(F_NONE);
        check_code("B29 pk 为空", ASSETPAK_E_ARG, assetpak_parse(0, g_buf, TOTAL, &lim));
        check_code("B29 buf 为空", ASSETPAK_E_ARG, assetpak_parse(&pak, 0, TOTAL, &lim));
        check_code("B29 limits 为空", ASSETPAK_E_ARG, assetpak_parse(&pak, g_buf, TOTAL, 0));
    }
}

uint32_t test_assetpak_run(void)
{
    g_fail = 0u;
    out("assetpak 素材包解析（手写包 + 25 处注错）\n");
    t_valid();
    t_faults();
    return g_fail;
}
