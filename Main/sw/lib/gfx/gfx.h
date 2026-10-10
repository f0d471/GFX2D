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
*         判据与口径见 Engineering/TechReports/sw/02-命令表构造层与软件执行器.md 第五节。
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

/* 标量 v1/TEX 的规范条款 §5 第二至第十步；v1-I 包用 gfx_instance_packet_check。
   返回 spec 错误码。
   不看命令表状态，也不看 clip，所以构造层与执行器可以共用。 */
int  gfx_cmd_check(const gfx_surface_table *t, const gfx_cmd *c);

/* 调试用：把一条命令写成一行文本，返回写进去的字符数（不含结尾的 0）。 */
uint32_t gfx_cmd_decode(const gfx_cmd *c, char *buf, uint32_t cap);

/* ABI v2 的提交配置；背景/目的必须是等高的 640 宽 DDR XRGB 表面。
   高度 = lstrips << strip_log2，strip_log2 为 3/4/5。
   BAND 是内部路径，表面 space 仍为 DDR；源不得与目的 backing 区间相交。 */
typedef struct {
    uint32_t lstrips, strip_log2, bg_id, dst_id;
} gfx_strip_config;
/* F3 工作区由调用方独占；封表后直到完成前不得复用。 */
typedef struct {
    gfx_cmd cmd;
    uint16_t first_strip, last_strip;
    uint32_t logical_index, reserved[2];
} gfx_strip_plan_item;
_Static_assert(sizeof(gfx_strip_plan_item)==48u,"plan item layout");
typedef struct {
    gfx_strip_plan_item *items;
    uint32_t item_cap;
    uint32_t *refs;
    uint32_t ref_cap;
    uint32_t count[32],offset[32],cursor[32];
    uint32_t items_used,refs_used;
    int sealed;
} gfx_strip_workspace;
typedef struct {
    uint32_t key[17];
    uint32_t leading,trailing,pieces;
    int valid;
} gfx_geometry_entry;
typedef struct {
    gfx_geometry_entry entry[8];
    uint32_t hits,misses;
    uint32_t generation;
    gfx_geometry_entry *active;
} gfx_geometry_cache;
struct gfx_frame_plan;
struct gfx_instance_arena;
int gfx_strip_config_check(const gfx_surface_table *t, const gfx_strip_config *cfg);
int gfx_strip_cmd_check(const gfx_surface_table *t, const gfx_strip_config *cfg,
                        const gfx_cmd *c);

/* --- 构造层 ------------------------------------------------------------- */

typedef struct {
    gfx_cmd                 *buf;
    uint32_t                 cap;            /* buf 能放多少条 */
    uint32_t                 used;           /* 已追加多少条，含 END */
    const gfx_surface_table *t;              /* 显式绑定，不用模块全局 */
    int                      first_error;    /* sticky：首个非零返回值 */
    uint32_t                 rejected;       /* 被拒绝的追加次数 */
    uint32_t                 overflow;       /* 因表满被拒的次数 */
    uint8_t                 *strip_table;    /* NULL 为 v1；v2 表头是计数数组 */
    gfx_strip_config         strips;
    gfx_rect                 build_clip;
    uint32_t instance_generation;
    struct gfx_instance_arena *instances;  /* NULL 使用标量直通 */
    int                      ended;          /* 已经追加过 END */
    gfx_strip_workspace     *planned;        /* NULL 保持即时物化 */
    gfx_geometry_cache      *geometry;       /* NULL 使用原几何算法 */
    struct gfx_frame_plan   *frame;          /* 多 pass 提交上下文，应用显式绑定 */
} gfx_list;

void gfx_list_begin(gfx_list *l, gfx_cmd *buf, uint32_t cap,
                    const gfx_surface_table *t);

/* 条带模式绑定调用方提供的定长表，初始化表头计数；不写 END。
   SET_CLIP API 在构造时吸收，默认全屏；跨帧持续裁剪由调用方显式传入。
   分桶追加有原子性：任何桶溢出都不改该次调用的任何桶，first_error 拒绝提交。
   不自动分帧（混合顺序与背景语义不能靠拆帧保持）。 */
int gfx_list_begin_strips(gfx_list *l, void *table, uint32_t bytes,
                           const gfx_surface_table *t, const gfx_strip_config *cfg,
                           const gfx_rect *initial_clip);
int gfx_strip_append(gfx_list *l, const gfx_cmd *c);
int gfx_list_begin_strips_planned(gfx_list *l, void *table, uint32_t bytes,
                                 const gfx_surface_table *t, const gfx_strip_config *cfg,
                                 const gfx_rect *clip, gfx_strip_workspace *workspace);
/* 每张表显式绑定并清空缓存；命中仍检查当前坐标的全包含条件。 */
int gfx_list_geometry(gfx_list *l, gfx_geometry_cache *cache);
int gfx_list_geometry_generation(gfx_list *l,uint32_t generation);

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

/* --- 批量提交（sw 第 06 章 3.6 节）--------------------------------------------- */

/* 一串 BLIT 共用的部分。字段与 gfx_blit() 的同名参数同型。 */
typedef struct {
    int      src;
    int      dst;
    int      w, h;
    int      eq;
    uint8_t  c;
    int      key_en;
    uint32_t key;
} gfx_blit_tmpl;

/* 逐条输入：目的左上角与源左上角。int32，与 gfx_blit() 的坐标参数同型，
   所以越出 int16 的坐标与逐条调用一样报 GFX_BUILD_BAD_ARG。 */
typedef struct {
    int32_t dx, dy;
    int32_t sx, sy;
} gfx_blit_pos;

/* 对 pos[0..n) 依次追加 BLIT，**结果与对每条依次调用 gfx_blit() 逐字节相同**：
   缓冲区、used、rejected、overflow、first_error、ended 都一样；返回成功追加的条数
   （即逐条调用时返回 0 的次数）。l 或 l->t 为空时与 gfx_blit() 一样什么也不动，返回 0。

   做法：每次调用开头按模板打包一条记录、跑一次 gfx_cmd_check()，之后每条只填
   w1、w2 与 w7。模板校验放在调用开头而不是初始化时，因为表面表可能在两次调用之间被改写。
   **源表面等于目的表面时每条退回完整的 gfx_cmd_check()**：规范条款 §5 的第十步
   （同一表面上两个矩形不得重叠）依赖每条自己的坐标，模板校验盖不住。 */
uint32_t gfx_blit_run(gfx_list *l, const gfx_blit_tmpl *t,
                      const gfx_blit_pos *pos, uint32_t n);

/* v1-I 的数组由本帧独占，完成或排空后方可复用。 */
typedef struct gfx_instance_arena {
    uint8_t *host;
    uint32_t dev_base, bytes, used, generation;
} gfx_instance_arena;
typedef struct {
    gfx_blit_tmpl shared;
    uint32_t layout;
    int32_t fixed_sx, fixed_sy;
} gfx_instance_desc;
typedef struct {
    const uint8_t *host;
    uint32_t dev_base, bytes;
} gfx_instance_view;
typedef struct { uint32_t packet, instance; } gfx_instance_id;
int gfx_instances_append(gfx_list *list, const gfx_instance_desc *desc,
                         const gfx_blit_pos *pos, uint32_t count,
                         gfx_instance_arena *arena);
int gfx_instance_packet_check(const gfx_surface_table *t, const gfx_cmd *c);
int gfx_instance_at(const gfx_cmd *c, const gfx_instance_view *views,
                    uint32_t view_count, uint32_t index, gfx_cmd *scalar);
/* 提交前展开；先全量检查容量与数组，不留下部分输出。 */
int gfx_instances_expand(const gfx_surface_table *t, const gfx_cmd *in, uint32_t count,
                          const gfx_instance_view *views, uint32_t view_count,
                          gfx_cmd *out, gfx_instance_id *ids, uint32_t cap, uint32_t *used);

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

/* 显式只读数组 view；旧执行入口继续只解释 v1 标量。 */
int exec_sw_run_instances(exec_sw_state *es, const gfx_surface_table *t, const gfx_mem *m,
                           const gfx_cmd *list, uint32_t count, uint32_t readable_count,
                           const gfx_instance_view *views, uint32_t view_count,
                           exec_sw_stats *st, exec_sw_error *err,
                           uint32_t *error_instance, uint32_t *instances);

/* 执行 ABI v2 表；readable_bytes 给出宿主取数上界。
   scratch 为独占软件 BAND 影子，至少 (4096 << strip_log2) 字节，4 字节对齐。
   每桶 LOAD→局部命令→FLUSH，失败桶不写回；错误 index = strip*1024 + k。
   源读取始终使用原始 m，clip 状态不受条带执行改变。 */
int exec_sw_run_strips(const gfx_surface_table *t, const gfx_mem *m,
                       const void *table, uint32_t readable_bytes,
                       const gfx_strip_config *cfg, void *scratch, uint32_t scratch_bytes,
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
