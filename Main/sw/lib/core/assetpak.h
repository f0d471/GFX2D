/******************************************************************************
* @file assetpak.h: 素材包（G2DP）的解析与校验
*
* @brief  包的字节格式与校验顺序以本文件为准，生成器与校验工具都照这里实现；
*         格式改动先改这里。PC 与板上共用这一份，所以它属于 `lib/core`：
*         不包含任何平台头文件，也不认识本项目的具体素材编号
*         （编号在生成的 `assets.h` 里，只有应用包含它）。
*
*         校验顺序是固定的，一条同时有多处错的包只报一个码：
*           头（magic/版本/头长/保留/条目数/目录字节数/总长）
*           → 逐项字段与几何（偏移对齐、行跨、长度、格式、标志）
*           → 逐项越界与互相重叠 → 逐项数据 CRC → 整包 CRC
*         前两段是纯结构检查，任一失败都不返回可用于绘图的表面。
*
*         包 ID **不是表面槽号**：`gfx_surface_table` 只有 16 槽，而包最多 256 项。
*         解析器不登记任何表面，应用按当前画面需要选条目、自己登记。
******************************************************************************/
#ifndef GFX2D_CORE_ASSETPAK_H
#define GFX2D_CORE_ASSETPAK_H

#include <stdint.h>

/* --- 磁盘格式常量 --------------------------------------------------------- */
/* magic 是 4 个字节 'G','2','D','P'，按小端读成 u32 就是这个值。 */
#define ASSETPAK_MAGIC        0x50443247u
#define ASSETPAK_VERSION      1u
#define ASSETPAK_HEADER_LEN   32u
#define ASSETPAK_DIR_ENTRY    32u
#define ASSETPAK_MAX_ENTRIES  256u
#define ASSETPAK_ENTRY_ALIGN  64u

/* 格式号与 gfx_abi.h 的 v1 定义一致；v1 的素材包不产出 INDEX8（格式 3）。 */
#define ASSETPAK_FMT_XRGB8888     0u
#define ASSETPAK_FMT_ARGB8888_PM  1u
#define ASSETPAK_FMT_ARGB8888_ST  2u

#define ASSETPAK_FLAG_KEY     0x0001u

/* --- 错误码 --------------------------------------------------------------- */
#define ASSETPAK_OK           0
#define ASSETPAK_E_ARG        1   /* 调用参数不合法 */
#define ASSETPAK_E_MAGIC      2
#define ASSETPAK_E_VERSION    3
#define ASSETPAK_E_HEADER     4   /* 头字段自相矛盾，或条目保留字段非 0 */
#define ASSETPAK_E_COUNT      5
#define ASSETPAK_E_TOTAL      6   /* 总长超过传入长度或运行预算 */
#define ASSETPAK_E_OFFSET     7   /* 偏移未按 64 字节对齐，或落进头/目录里 */
#define ASSETPAK_E_GEOM       8   /* 宽高/行跨/数据长不成关系 */
#define ASSETPAK_E_FORMAT     9
#define ASSETPAK_E_FLAGS      10  /* 标志位或色键字违反约定 */
#define ASSETPAK_E_OVERLAP    11
#define ASSETPAK_E_BOUNDS     12  /* 条目越过包末端 */
#define ASSETPAK_E_DUP_ID     13
#define ASSETPAK_E_ENTRY_CRC  14
#define ASSETPAK_E_PACK_CRC   15
#define ASSETPAK_E_NOTFOUND   16

#define ASSETPAK_NO_INDEX     0xFFFFFFFFu

/* 装载入口在读取正文之前用的界限。物理上限（4 MiB Flash 分区）由调用方给，
   解析器自己不写死任何分区地址。 */
typedef struct {
    uint32_t max_total;
} assetpak_limits;

/* 解码后的一项。它不是磁盘布局（磁盘每项固定 32 字节），只是给调用方用的形状。 */
typedef struct {
    uint32_t id;
    uint32_t offset;        /* 相对包起点 */
    uint32_t length;        /* 必须等于 height << stride_log2 */
    uint16_t width;
    uint16_t height;
    uint8_t  stride_log2;
    uint8_t  format;
    uint16_t flags;
    uint32_t key;
    uint32_t data_crc;
} assetpak_entry;

typedef struct {
    const uint8_t *base;    /* 包起点，调用方持有，本结构不复制数据 */
    uint32_t version;       /* 解析出来的包版本，与生成的 assets.h 对账用 */
    uint32_t total_len;
    uint32_t entry_count;
    uint32_t dir_bytes;
    uint32_t pack_crc;
    uint32_t fail_index;    /* 最近一次失败落在第几项，没失败或包级错误是 ASSETPAK_NO_INDEX */
    uint32_t fail_id;       /* 对应条目的 ID，包级错误时是 0 */
} assetpak;

/* --- 接口 ----------------------------------------------------------------- */

/* 错误码的稳定名字，给串口日志用（不返回 NULL）。 */
const char *assetpak_strerror(int code);

/* 只校验 32 字节包头，不动目录与数据。装载入口用它决定「要不要继续读正文」：
   读之前先确认总长没有超过容量，不拿未校验的长度去写 DDR。
   注意它**不**拿头里的总长去比 len——正文还没读；调用方给 32 字节即可。
   成功时把总长与条目数写进 out_total / out_count（两者都可以传 NULL）。 */
int assetpak_probe_header(const void *hdr, uint32_t len, const assetpak_limits *lim,
                          uint32_t *out_total, uint32_t *out_count);

/* 完整校验：头、目录、逐项字段与几何、重叠、逐项 CRC、整包 CRC。
   len 是调用方实际可读的字节数（可以大于 total_len）。
   失败时 pk->fail_index / fail_id 指明落在哪一项，包级错误时为 NO_INDEX / 0。 */
int assetpak_parse(assetpak *pk, const void *buf, uint32_t len,
                   const assetpak_limits *lim);

/* 取第 index 项（0 基，顺序就是打包时的 manifest 顺序）。 */
int assetpak_entry_at(const assetpak *pk, uint32_t index, assetpak_entry *out);

/* 按不稳定编号之外的显式 ID 查一项。 */
int assetpak_find(const assetpak *pk, uint32_t id, assetpak_entry *out);

/* 条目的像素数据起点（在 base 之上偏移）。 */
const uint8_t *assetpak_data(const assetpak *pk, const assetpak_entry *e);

/* 行跨字节数 = 1 << stride_log2。 */
uint32_t assetpak_stride(const assetpak_entry *e);

/* --- 两段式装载 ----------------------------------------------------------- */
/* 读一段字节的抽象：offset 相对**素材分区起点**，不是 Flash 物理地址。
   板级层实现它（板上走 SPI Flash，PC 走 assets.pak 文件），lib 不认识平台。
   返回 0 成功；失败返回板级层自己的负错误码，本文件原样传出去。 */
typedef int (*assetpak_read_fn)(uint32_t offset, void *dst, uint32_t bytes);

/* 先读 32 字节包头、校验总长没有超过预算与 dst 容量，再按总长读进 dst 并完整解析。
   顺序是 2.3 节要求的「不能先信任未校验的总长去写 DDR」：dst 由调用方给
   （正常就是 board_region(REGION_ASSET).ptr），读出多少由校验过的 total 决定。
   成功后 pk 指向 dst 里的包，*out_len 是实际字节数。 */
int assetpak_load(assetpak *pk, assetpak_read_fn rd, void *dst, uint32_t dst_bytes,
                  const assetpak_limits *lim, uint32_t *out_len);

#endif /* GFX2D_CORE_ASSETPAK_H */
