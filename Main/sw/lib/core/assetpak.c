/******************************************************************************
* @file assetpak.c: 素材包（G2DP）的解析与校验
*
* @brief  顺序与口径见 assetpak.h 的文件头。
*         全部字段按字节拼装，不把缓冲强制转成结构体：
*         LP64 与 ILP32 上解出的值必须一样，也不依赖编译器的对齐假设。
*
*         不分配内存、不用浮点；条目按需解码，不建索引表
*         （256 项的表要 7KB 栈，而本文件每次只解码一项）。
******************************************************************************/

#include "core/assetpak.h"
#include "core/crc32.h"

#define NO_INDEX ASSETPAK_NO_INDEX

static uint32_t rd_u16(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}

static uint32_t rd_u32(const uint8_t *p)
{
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

static void clear(assetpak *pk)
{
    pk->base        = 0;
    pk->version     = 0;
    pk->total_len   = 0;
    pk->entry_count = 0;
    pk->dir_bytes   = 0;
    pk->pack_crc    = 0;
    pk->fail_index  = NO_INDEX;
    pk->fail_id     = 0;
}

static void fail_at(assetpak *pk, uint32_t index, uint32_t id)
{
    pk->fail_index = index;
    pk->fail_id    = id;
}

/* 头的公共部分：magic/版本/头长/保留/条目数/目录字节数/总长关系与容量。
   先头后正文，所以 probe 与 parse 共用这一份，规则不会在两边漂移。

   `have_body` 区分两个调用者：probe 只拿到 32 字节包头，此时**不能**拿
   头里写的总长去比传入长度——正文还没读，比了必然失败；它只需要确认
   总长没有超过容量预算。parse 拿的是完整缓冲，才比较 total 与 len。 */
static int check_header(const uint8_t *b, uint32_t len, const assetpak_limits *lim,
                        uint32_t *out_total, uint32_t *out_count, int have_body)
{
    uint32_t header_len, count, total, dir_bytes, reserved;

    if (b == 0 || lim == 0)
        return ASSETPAK_E_ARG;
    if (len < ASSETPAK_HEADER_LEN)
        return ASSETPAK_E_HEADER;

    if (rd_u32(b + 0) != ASSETPAK_MAGIC)
        return ASSETPAK_E_MAGIC;
    if (rd_u32(b + 4) != ASSETPAK_VERSION)
        return ASSETPAK_E_VERSION;

    header_len = rd_u32(b + 8);
    count      = rd_u32(b + 12);
    total      = rd_u32(b + 16);
    dir_bytes  = rd_u32(b + 24);
    reserved   = rd_u32(b + 28);

    if (header_len != ASSETPAK_HEADER_LEN || reserved != 0u)
        return ASSETPAK_E_HEADER;
    if (count < 1u || count > ASSETPAK_MAX_ENTRIES)
        return ASSETPAK_E_COUNT;
    /* count 已经 <= 256，乘法不会溢出；仍然写成 64 位是为了不留下隐患。 */
    if ((uint64_t)dir_bytes != (uint64_t)count * ASSETPAK_DIR_ENTRY)
        return ASSETPAK_E_HEADER;
    if ((uint64_t)total < (uint64_t)ASSETPAK_HEADER_LEN + dir_bytes)
        return ASSETPAK_E_HEADER;
    if (have_body && total > len)
        return ASSETPAK_E_TOTAL;
    if (total > lim->max_total)
        return ASSETPAK_E_TOTAL;

    if (out_total != 0)
        *out_total = total;
    if (out_count != 0)
        *out_count = count;
    return ASSETPAK_OK;
}

int assetpak_probe_header(const void *hdr, uint32_t len, const assetpak_limits *lim,
                          uint32_t *out_total, uint32_t *out_count)
{
    return check_header((const uint8_t *)hdr, len, lim, out_total, out_count, 0);
}

const char *assetpak_strerror(int code)
{
    switch (code) {
    case ASSETPAK_OK:          return "ok";
    case ASSETPAK_E_ARG:       return "bad_arg";
    case ASSETPAK_E_MAGIC:     return "bad_magic";
    case ASSETPAK_E_VERSION:   return "bad_version";
    case ASSETPAK_E_HEADER:    return "bad_header";
    case ASSETPAK_E_COUNT:     return "bad_count";
    case ASSETPAK_E_TOTAL:     return "bad_total";
    case ASSETPAK_E_OFFSET:    return "bad_offset";
    case ASSETPAK_E_GEOM:      return "bad_geom";
    case ASSETPAK_E_FORMAT:    return "bad_format";
    case ASSETPAK_E_FLAGS:     return "bad_flags";
    case ASSETPAK_E_OVERLAP:   return "overlap";
    case ASSETPAK_E_BOUNDS:    return "out_of_bounds";
    case ASSETPAK_E_DUP_ID:    return "dup_id";
    case ASSETPAK_E_ENTRY_CRC: return "entry_crc";
    case ASSETPAK_E_PACK_CRC:  return "pack_crc";
    case ASSETPAK_E_NOTFOUND:  return "not_found";
    default:                   return "unknown";
    }
}

/* 目录项第 index 项的字段检查。几何一类的错误在这里一次报掉，
   越界与重叠要跟别的项比，放在后面两轮。 */
static int check_entry(const uint8_t *b, uint32_t index, uint32_t data_start,
                       uint32_t total, uint32_t *out_id, uint32_t *out_off,
                       uint32_t *out_len)
{
    const uint8_t *e = b + ASSETPAK_HEADER_LEN + index * ASSETPAK_DIR_ENTRY;
    uint32_t id    = rd_u32(e + 0);
    uint32_t off   = rd_u32(e + 4);
    uint32_t len   = rd_u32(e + 8);
    uint32_t w     = rd_u16(e + 12);
    uint32_t h     = rd_u16(e + 14);
    uint32_t sl    = (uint32_t)e[16];
    uint32_t fmt   = (uint32_t)e[17];
    uint32_t flags = rd_u16(e + 18);
    uint32_t key   = rd_u32(e + 20);
    uint32_t rsv   = rd_u32(e + 28);

    *out_id  = id;
    *out_off = off;
    *out_len = len;

    if (rsv != 0u)
        return ASSETPAK_E_HEADER;
    if ((flags & ~(uint32_t)ASSETPAK_FLAG_KEY) != 0u)
        return ASSETPAK_E_FLAGS;
    if ((flags & ASSETPAK_FLAG_KEY) == 0u && key != 0u)
        return ASSETPAK_E_FLAGS;
    if ((flags & ASSETPAK_FLAG_KEY) != 0u && key > 0x00FFFFFFu)
        return ASSETPAK_E_FLAGS;

    if (fmt > ASSETPAK_FMT_ARGB8888_ST)
        return ASSETPAK_E_FORMAT;
    if ((flags & ASSETPAK_FLAG_KEY) != 0u && fmt != ASSETPAK_FMT_XRGB8888)
        return ASSETPAK_E_FLAGS;

    if (w == 0u || h == 0u)
        return ASSETPAK_E_GEOM;
    if (sl < 2u || sl > 31u)
        return ASSETPAK_E_GEOM;
    if (w * 4u > (1u << sl))
        return ASSETPAK_E_GEOM;
    /* h << sl 在 32 位里会溢出（h 最大 65535、sl 最大 31），用 64 位算。 */
    if ((uint64_t)len != ((uint64_t)h << sl))
        return ASSETPAK_E_GEOM;

    if ((off & (ASSETPAK_ENTRY_ALIGN - 1u)) != 0u)
        return ASSETPAK_E_OFFSET;
    if (off < data_start)
        return ASSETPAK_E_OFFSET;
    if ((uint64_t)off + len > total)
        return ASSETPAK_E_BOUNDS;

    return ASSETPAK_OK;
}

int assetpak_parse(assetpak *pk, const void *buf, uint32_t len,
                   const assetpak_limits *lim)
{
    const uint8_t *b = (const uint8_t *)buf;
    uint32_t total, count, data_start, i, j;
    int      rc;

    if (pk == 0 || buf == 0 || lim == 0)
        return ASSETPAK_E_ARG;
    clear(pk);

    rc = check_header(b, len, lim, &total, &count, 1);
    if (rc != ASSETPAK_OK)
        return rc;

    data_start = ASSETPAK_HEADER_LEN + count * ASSETPAK_DIR_ENTRY;

    /* 第一轮：逐项字段与几何。 */
    for (i = 0u; i < count; i++) {
        uint32_t id, off, l;

        rc = check_entry(b, i, data_start, total, &id, &off, &l);
        if (rc != ASSETPAK_OK) {
            fail_at(pk, i, id);
            return rc;
        }
    }

    /* 第二轮：ID 不得重复。256 项两两比是 32640 次，比建表省内存。 */
    for (i = 0u; i < count; i++) {
        for (j = i + 1u; j < count; j++) {
            const uint8_t *ei = b + ASSETPAK_HEADER_LEN + i * ASSETPAK_DIR_ENTRY;
            const uint8_t *ej = b + ASSETPAK_HEADER_LEN + j * ASSETPAK_DIR_ENTRY;

            if (rd_u32(ei) == rd_u32(ej)) {
                fail_at(pk, j, rd_u32(ej));
                return ASSETPAK_E_DUP_ID;
            }
        }
    }

    /* 第三轮：数据区不得互相重叠。 */
    for (i = 0u; i < count; i++) {
        const uint8_t *ei = b + ASSETPAK_HEADER_LEN + i * ASSETPAK_DIR_ENTRY;
        uint64_t ai = rd_u32(ei + 4);
        uint64_t bi = ai + rd_u32(ei + 8);

        for (j = i + 1u; j < count; j++) {
            const uint8_t *ej = b + ASSETPAK_HEADER_LEN + j * ASSETPAK_DIR_ENTRY;
            uint64_t aj = rd_u32(ej + 4);
            uint64_t bj = aj + rd_u32(ej + 8);

            if (ai < bj && aj < bi) {
                fail_at(pk, j, rd_u32(ej));
                return ASSETPAK_E_OVERLAP;
            }
        }
    }

    /* 第四轮：逐项数据 CRC，报出出问题的那个 ID。
       注错 INJECT_ASSET_SKIP_CRC 把第四、五轮一起跳过：判据脚本拿它证明
       「坏包被拒」这条判据不是空转——同一个坏包，正常版报 entry_crc、
       注错版照收，画面 CRC 随之与 golden 不同。 */
#ifndef INJECT_ASSET_SKIP_CRC
    {
    uint32_t crc;

    for (i = 0u; i < count; i++) {
        const uint8_t *e = b + ASSETPAK_HEADER_LEN + i * ASSETPAK_DIR_ENTRY;
        uint32_t off = rd_u32(e + 4);
        uint32_t l   = rd_u32(e + 8);

        crc = crc32_ieee(b + off, l);
        if (crc != rd_u32(e + 24)) {
            fail_at(pk, i, rd_u32(e));
            return ASSETPAK_E_ENTRY_CRC;
        }
    }

    /* 第五轮：整包 CRC。头里那 4 个字节按 0 参与计算，与打包工具一致。 */
    {
        crc32_ctx ctx;
        uint8_t   zero[4] = { 0u, 0u, 0u, 0u };

        crc32_begin(&ctx);
        crc32_update(&ctx, b, 20u);
        crc32_update(&ctx, zero, 4u);
        crc32_update(&ctx, b + 24u, total - 24u);
        crc = crc32_end(&ctx);
    }
    if (crc != rd_u32(b + 20)) {
        pk->fail_index = NO_INDEX;
        pk->fail_id    = 0u;
        return ASSETPAK_E_PACK_CRC;
    }
    }
#endif /* !INJECT_ASSET_SKIP_CRC */

    pk->base        = b;
    pk->version     = rd_u32(b + 4);
    pk->total_len   = total;
    pk->entry_count = count;
    pk->dir_bytes   = count * ASSETPAK_DIR_ENTRY;
    pk->pack_crc    = rd_u32(b + 20);
    return ASSETPAK_OK;
}

static void decode_entry(const uint8_t *e, assetpak_entry *out)
{
    out->id          = rd_u32(e + 0);
    out->offset      = rd_u32(e + 4);
    out->length      = rd_u32(e + 8);
    out->width       = (uint16_t)rd_u16(e + 12);
    out->height      = (uint16_t)rd_u16(e + 14);
    out->stride_log2 = e[16];
    out->format      = e[17];
    out->flags       = (uint16_t)rd_u16(e + 18);
    out->key         = rd_u32(e + 20);
    out->data_crc    = rd_u32(e + 24);
}

int assetpak_entry_at(const assetpak *pk, uint32_t index, assetpak_entry *out)
{
    if (pk == 0 || out == 0 || pk->base == 0)
        return ASSETPAK_E_ARG;
    if (index >= pk->entry_count)
        return ASSETPAK_E_NOTFOUND;

    decode_entry(pk->base + ASSETPAK_HEADER_LEN + index * ASSETPAK_DIR_ENTRY, out);
    return ASSETPAK_OK;
}

int assetpak_find(const assetpak *pk, uint32_t id, assetpak_entry *out)
{
    uint32_t i;

    if (pk == 0 || out == 0 || pk->base == 0)
        return ASSETPAK_E_ARG;

    for (i = 0u; i < pk->entry_count; i++) {
        const uint8_t *e = pk->base + ASSETPAK_HEADER_LEN + i * ASSETPAK_DIR_ENTRY;

        if (rd_u32(e) == id) {
            decode_entry(e, out);
            return ASSETPAK_OK;
        }
    }
    return ASSETPAK_E_NOTFOUND;
}

const uint8_t *assetpak_data(const assetpak *pk, const assetpak_entry *e)
{
    if (pk == 0 || e == 0 || pk->base == 0)
        return 0;
    if (e->offset >= pk->total_len)
        return 0;
    return pk->base + e->offset;
}

uint32_t assetpak_stride(const assetpak_entry *e)
{
    if (e == 0 || e->stride_log2 > 31u)
        return 0u;
    return 1u << e->stride_log2;
}

int assetpak_load(assetpak *pk, assetpak_read_fn rd, void *dst, uint32_t dst_bytes,
                  const assetpak_limits *lim, uint32_t *out_len)
{
    uint8_t  hdr[ASSETPAK_HEADER_LEN];
    uint32_t total = 0u, count = 0u;
    int      rc;

    if (pk == 0 || rd == 0 || dst == 0 || lim == 0)
        return ASSETPAK_E_ARG;
    clear(pk);
    if (out_len != 0)
        *out_len = 0u;

    /* 第一步：只读包头。读出多少由校验过的总长决定，不拿未校验的长度去写 dst。 */
    rc = rd(0u, hdr, ASSETPAK_HEADER_LEN);
    if (rc != 0)
        return rc;                              /* 板级层的负错误码原样传出去 */
    rc = check_header(hdr, ASSETPAK_HEADER_LEN, lim, &total, &count, 0);
    if (rc != ASSETPAK_OK)
        return rc;
    if (total > dst_bytes)
        return ASSETPAK_E_TOTAL;

    /* 第二步：按总长读全文并完整解析。 */
    rc = rd(0u, dst, total);
    if (rc != 0)
        return rc;
    rc = assetpak_parse(pk, dst, total, lim);
    if (rc != ASSETPAK_OK)
        return rc;

    if (out_len != 0)
        *out_len = total;
    return ASSETPAK_OK;
}
