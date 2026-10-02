/******************************************************************************
* @file gfx_engine.c: 2D 渲染引擎最小驱动的 Ti60F225 实现（引擎 P1 B6）
*
* @brief  协议与取舍见同目录 gfx_engine.h 的文件头与 Engineering/Todo/hw/
*         引擎P1-B6-顶层集成与上板.md。寄存器偏移一律取生成的 gfx_regs.h，
*         本文件不出现第二个偏移真源；页基址是 APB Slave 0（soc.h 的
*         IO_APB_SLAVE_0_INPUT，CPU 地址 0xF810_0000）。
*
*         轮询上界的教训来自 board.c 的 DISP_POLL_LIMIT：比特流里没有该模块、
*         或通路没起来时，无上界的轮询表现像死机。这里两处等待都带上界，
*         超时按 GFX_ENG_E_TIMEOUT 返回而不是停在原地。
******************************************************************************/

#include <stdint.h>

#include "board.h"
#include "gfx_engine.h"

#include "bsp.h"            /* sdk：IO_APB_SLAVE_0_INPUT 基址 */

/* 引擎寄存器页基址。写成对 soc.h 宏的静态断言，宏变了这里编译期就断，
   而不是在板上读错页。 */
#define ENG_BASE            IO_APB_SLAVE_0_INPUT
#if ENG_BASE != 0xF8100000u
#error "gfx_engine.c: 引擎页基址与设计文档 3.4 节的 0xF810_0000 不一致"
#endif

/* 完成等待的轮询上界：最重的命令表（整屏 FILL 加整屏 BLIT）在硬件侧是毫秒级，
   给 1 秒是三个量级以上的余量；超时返回错误，调用方的判据会看见。 */
#define ENG_POLL_LIMIT      BOARD_HZ

static uint32_t eng_rd(uint32_t offset)
{
    return *(volatile uint32_t *)(uintptr_t)(ENG_BASE + offset);
}

static void eng_wr(uint32_t offset, uint32_t value)
{
    *(volatile uint32_t *)(uintptr_t)(ENG_BASE + offset) = value;
    /* 写被拒收（PSLVERR）时硬件没有独立的错误线，拒收场景由调用顺序保证：
       submit 先查 STATUS 的忙/排空位（D54 的拒收条件），其余寄存器不受限。 */
}

int gfx_engine_present(void)
{
    return eng_rd(GFX_ID) == GFX_ID_VALUE;
}

int gfx_engine_soft_reset(void)
{
    uint32_t st;
    uint32_t waited;

    if (!gfx_engine_present())
        return GFX_ENG_E_ABSENT;

    /* EN 与 SOFT_RST 一起写：EN=0 时 DOORBELL 拒收（D54），复位完要能直接提交。
       SOFT_RST 写 1 自清，复位请求在内部排空完成后才撤（D17/D54）。 */
    eng_wr(GFX_CTRL, GFX_CTRL_EN | GFX_CTRL_SOFT_RST);

    waited = 0u;
    do {
        st = eng_rd(GFX_STATUS);
        if ((st & (GFX_STATUS_BUSY | GFX_STATUS_DRAINING)) == 0u)
            return GFX_ENG_OK;
        waited++;
    } while (waited < ENG_POLL_LIMIT);

    return GFX_ENG_E_TIMEOUT;
}

int gfx_engine_surf_set(int slot, uint32_t base, uint32_t width,
                        uint32_t height, uint32_t stride_log2, uint32_t format)
{
    uint32_t ctrl;

    if (!gfx_engine_present())
        return GFX_ENG_E_ABSENT;
    if (slot < 0 || slot >= 16)
        return GFX_ENG_E_ARG;
    if (width == 0u || width > 0xFFFFu || height == 0u || height > 0xFFFFu)
        return GFX_ENG_E_ARG;    /* DIM 的两个字段都是 16 位，不能静默截断 */
    if (stride_log2 < 2u || stride_log2 > 31u)
        return GFX_ENG_E_ARG;
    if (format > 3u)
        return GFX_ENG_E_ARG;

    /* +0 base、+4 [15:0] 宽 / [31:16] 高、+8 控制字：[4:0] stride_log2、
       [9:8] space（恒 DDR=0）、[15:12] format、[31] valid，其余位必须为 0
       （设计文档 3.4 节，违例报 BAD_SURFACE）。valid 最后置 1。
       space/format 的取值写在位段里：space 只开放 DDR 一档（v1），
       format 的枚举在 gfx_abi.h，那是 lib 层的头，本层引用它就破坏了分层。 */
    eng_wr(GFX_SURF((uint32_t)slot, 0u), base);
    eng_wr(GFX_SURF((uint32_t)slot, 1u), (height << 16) | (width & 0xFFFFu));
    ctrl = (stride_log2 & 0x1Fu) | (0u << 8)
         | ((format & 0xFu) << 12);
    eng_wr(GFX_SURF((uint32_t)slot, 2u), ctrl | 0x80000000u);
    return GFX_ENG_OK;
}

int gfx_engine_submit(uint32_t list_base, uint32_t list_count)
{
    uint32_t st;

    if (!gfx_engine_present())
        return GFX_ENG_E_ABSENT;
    /* D53：非空表 LIST_BASE[4:0] 必须为 0，硬件对未对齐报 FETCH（索引 0、TAG 0）。
       在这里拦下来能把「调用方分配命令表没对齐」变成一眼可见的驱动错误码，
       而不是一张查不出原因的 FETCH。 */
    if (list_count != 0u && (list_base & 0x1Fu) != 0u)
        return GFX_ENG_E_ALIGN;

    st = eng_rd(GFX_STATUS);
    if ((st & (GFX_STATUS_BUSY | GFX_STATUS_DRAINING)) != 0u ||
        (eng_rd(GFX_CTRL) & GFX_CTRL_EN) == 0u)
        return GFX_ENG_E_REJECT;    /* 忙、排空或 EN=0 时 DOORBELL 均会被拒收 */

    eng_wr(GFX_LIST_BASE, list_base);
    eng_wr(GFX_LIST_COUNT, list_count); /* D53：寄存器与命令层都是完整 32 位 */
    eng_wr(GFX_DOORBELL, 1u);
    return GFX_ENG_OK;
}

int gfx_engine_wait(uint32_t *err_code, uint32_t *err_index, uint32_t *err_tag)
{
    uint32_t st;
    uint32_t waited;

    if (!gfx_engine_present())
        return GFX_ENG_E_ABSENT;

    /* D54：DONE/ERR 只在引擎静止（在途命令收尾、预取收完）之后置位，
       所以「DONE 或 ERR 且不忙」就代表可以读计数器与错误定位。 */
    waited = 0u;
    do {
        st = eng_rd(GFX_STATUS);
        if ((st & (GFX_STATUS_DONE | GFX_STATUS_ERR)) != 0u
            && (st & GFX_STATUS_BUSY) == 0u) {
            if (err_code != NULL)
                *err_code = (st >> 4) & 0xFu;
            if (err_index != NULL)
                *err_index = eng_rd(GFX_ERROR_INDEX);
            if (err_tag != NULL)
                *err_tag = eng_rd(GFX_ERROR_TAG);
            return GFX_ENG_OK;
        }
        waited++;
    } while (waited < ENG_POLL_LIMIT);

    return GFX_ENG_E_TIMEOUT;
}

int gfx_engine_perf_read(uint32_t *values, uint32_t n)
{
    uint32_t i;

    if (!gfx_engine_present())
        return GFX_ENG_E_ABSENT;
    if (n > GFX_ENG_PERF_N)
        return GFX_ENG_E_ARG;
    if (n != 0u && values == NULL)
        return GFX_ENG_E_ARG;
    for (i = 0u; i < n; i++)
        values[i] = eng_rd(GFX_PERF(i));
    return GFX_ENG_OK;
}

int gfx_engine_perf_clear(void)
{
    if (!gfx_engine_present())
        return GFX_ENG_E_ABSENT;
    eng_wr(GFX_CTRL, GFX_CTRL_EN | GFX_CTRL_CNT_CLR);
    return GFX_ENG_OK;
}
