/******************************************************************************
* @file gfx.h: 命令表构造层与软件执行器的接口
*
* @brief  三组接口：表面表的登记、命令表的构造、命令表的执行。
*         行为的权威是 spec 第 01、02 两章，本文件只给签名与各字段的口径。
*
*         构造层与执行器共用同一个记录校验 `gfx_cmd_check()`：
*         规范条款 §5 的十步里，第一步（取记录）是执行器的有界取数，
*         其余九步与命令表状态无关，写成一个纯函数，两处调用同一份，
*         规则不会在两边漂移。构造时早检不替代执行时重查，
*         因为表面表可能在两次调用之间被改写。
*
*         判据与口径见 Engineering/Todo/sw/SW3-命令表构造层与软件执行器.md 第 3 节。
******************************************************************************/
#ifndef GFX2D_GFX_H
#define GFX2D_GFX_H

#include "gfx/gfx_abi.h"

/* --- 表面表 ------------------------------------------------------------- */

/* 校验结构并登记，最后置 valid。返回 0 成功，GFX_BUILD_BAD_ARG 表示
   参数不能无损编码或描述符结构不合法（此时该槽位不被改写）。 */
int  gfx_surface_set(gfx_surface_table *t, int id, const gfx_surface *s);

/* 按给定的三个字原样写入，不做任何校验。
   用来构造非法描述符的测试输入，以及把一份外部向量原样放进表里。 */
void gfx_surface_set_raw(gfx_surface_table *t, int id,
                         uint32_t base, uint32_t dim, uint32_t ctrl);

/* 软复位：清掉全部 valid。spec 第 02 章规范条款 §2。 */
void gfx_surface_reset(gfx_surface_table *t);

/* 结构谓词，规范条款 §2 的六条。返回非 0 表示结构合法且 valid。 */
int  gfx_surface_struct_ok(const gfx_surface_table *t, uint32_t id);

/* --- 记录校验 ----------------------------------------------------------- */

/* 规范条款 §5 十步里的第二至第十步，返回 spec 错误码。
   不看命令表状态，也不看 clip，所以构造层与执行器可以共用。 */
int  gfx_cmd_check(const gfx_surface_table *t, const gfx_cmd *c);

/* 调试用：把一条命令写成一行文本，返回写进去的字符数（不含结尾的 0）。 */
uint32_t gfx_cmd_decode(const gfx_cmd *c, char *buf, uint32_t cap);

/* --- 构造层 ------------------------------------------------------------- */

typedef struct {
    gfx_cmd                 *buf;
    uint32_t                 cap;            /* buf 能放多少条 */
    uint32_t                 used;           /* 已追加多少条，含 END */
    const gfx_surface_table *t;              /* 显式绑定，不用模块全局 */
    int                      first_error;    /* sticky：首个非零返回值 */
    uint32_t                 rejected;       /* 被拒绝的追加次数 */
    uint32_t                 overflow;       /* 因表满被拒的次数 */
    int                      ended;          /* 已经追加过 END */
} gfx_list;

void gfx_list_begin(gfx_list *l, gfx_cmd *buf, uint32_t cap,
                    const gfx_surface_table *t);

/* 五个追加函数的返回值口径一致：
     0            已追加
     正值         spec 错误码，该调用不追加，rejected 加一
     GFX_BUILD_*  构造层错误，同样不追加
   被拒绝的调用不占记录索引，后续成功记录的 w7 仍等于它在表里的零基索引。 */
int  gfx_nop(gfx_list *l);
int  gfx_fill(gfx_list *l, int dst, gfx_rect r, uint32_t argb, int eq, uint8_t c);
int  gfx_blit(gfx_list *l, int src, int sx, int sy, int dst, int dx, int dy,
              int w, int h, int eq, uint8_t c, int key_en, uint32_t key);
int  gfx_set_clip(gfx_list *l, gfx_rect r);
int  gfx_list_end(gfx_list *l);

/* --- 软件执行器 --------------------------------------------------------- */

/* 跨提交的状态。SET_CLIP 的框持续到下一条 SET_CLIP 或软复位，
   既不属于某一次提交，也不能放进模块全局，所以由调用方持有。 */
typedef struct {
    gfx_rect clip;
} exec_sw_state;

/* 统计。字段名与硬件计数器同名（设计文档 3.4 节末），口径写在这里：
     cmd            成功执行完的记录数，含 NOP 与 SET_CLIP，不含 END，不含出错那条
     records_read   取数次数，含 END、含出错那条、含取数失败那次
     pixel_total    进入逐像素循环的目的像素数，即各命令裁剪后窗口面积之和
     pixel_written  真正写回目的表面的像素数
     key_skip       色键命中而跳过的像素数
     src_oob        源坐标越界而跳过的像素数（软件侧附加项，硬件不要求实现）
   对账等式：pixel_total == pixel_written + key_skip + src_oob。 */
typedef struct {
    uint32_t cmd;
    uint32_t records_read;
    uint32_t pixel_total;
    uint32_t pixel_written;
    uint32_t key_skip;
    uint32_t src_oob;
} exec_sw_stats;

/* 错误三元组，与硬件的 STATUS / ERROR_INDEX / ERROR_TAG 同口径。
   index 是当前提交表内的零基记录索引；FETCH 时 tag 取 0。 */
typedef struct {
    int      code;
    uint32_t index;
    uint32_t tag;
} exec_sw_error;

/* 软复位：clip 回到整个游戏区。 */
void exec_sw_reset(exec_sw_state *es);

/* 跑一次提交。stats 累加（不清零，多次提交可以接着统计），err 每次覆盖。
   stats 与 err 都可以传 NULL。返回 spec 错误码。 */
int  exec_sw_run(exec_sw_state *es, const gfx_surface_table *t, const gfx_mem *m,
                 const gfx_cmd *list, uint32_t list_count, uint32_t readable_count,
                 exec_sw_stats *st, exec_sw_error *err);

/* 把统计清零。 */
void exec_sw_stats_clear(exec_sw_stats *st);

/* --- 整帧用例（E7 与引擎仿真共用的金标准输入） ------------------------- */

#define GFX_CASE_N 11

/* 第 idx 个用例的名字，idx 越界时返回 NULL。 */
const char *gfx_case_name(uint32_t idx);

/* 按用例号建好表面表与命令表，并把两张表面的初始内容画进 mem。
   fb 与 tex 是两个表面在设备地址空间里的基址，调用方保证它们在 mem 窗口内、
   且按各自的 stride 与尺寸互不重叠。
   cmds 至少要能放 2 * GFX_CASE_MAX_CMD 条；返回第一张表的条数（即 LIST_COUNT）。
   两张表的用例（set-clip-state）把第二张表的条数写进 *count2、
   记录接在第一张表后面，其余用例 *count2 为 0。 */
#define GFX_CASE_MAX_CMD 8

uint32_t gfx_case_build(uint32_t idx, gfx_surface_table *t, const gfx_mem *m,
                        uint32_t fb, uint32_t tex,
                        gfx_cmd *cmds, uint32_t *count2);

/* 用例的帧缓冲与纹理尺寸。帧缓冲与板上一致：640x480、stride_log2 = 12。 */
#define GFX_CASE_FB_STRIDE_LOG2 12u
#define GFX_CASE_TEX_W          64u
#define GFX_CASE_TEX_H          64u
#define GFX_CASE_TEX_STRIDE_LOG2 8u

/* 用例在设备地址空间里的落点：帧缓冲在前，纹理从 0x20_0000 起，两者不重叠
   （帧缓冲占 480 x 4096 = 0x1E_0000 字节，纹理 64 x 256 = 0x4000 字节）。
   主机侧单元测试与 PC 平台层的判据程序共用这一组值：各写一遍会漂移，
   而漂移的后果是同一个用例在两处算出不同的 CRC、与 golden.txt 对不上却查不出原因。 */
#define GFX_CASE_FB_BASE        0x00001000u
#define GFX_CASE_TEX_BASE       0x00200000u
#define GFX_CASE_DEV_BYTES      0x00210000u

#endif /* GFX2D_GFX_H */
