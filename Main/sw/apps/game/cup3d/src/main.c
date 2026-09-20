/******************************************************************************
*
* @file main.c: cup3d
*
* @brief  纯软件 3D 光栅器在扫描输出的帧缓冲上转一只茶杯。
*
*         它有三个用途，按重要性排：
*         1. **软件渲染的像素吞吐是什么量级**，用一个真实负载量出来。
*            E7 要的就是这个数，`displayTest` 的 FILL 只量了纯填充。
*         2. 把扫描输出这条通路跑在一个「会动、会挡、每帧不一样」的负载下，
*            而不是只跑纯色块：z-buffer、背面、遮挡都会暴露通路上的问题。
*         3. 对外演示。
*
*         **分辨率的取法。** 光栅到 320x240，1:1 贴进 640x480 帧缓冲的正中间
*         （偏移 160, 120），四周留黑。不铺满全屏是算出来的：
*         `displayTest` 量到整屏 640x480 清一次要 13.6~21.5 毫秒，
*         一帧只有 16.7 毫秒，铺满之后清屏就把预算吃光了，没有余量留给光栅。
*         320x240 的清屏是它的四分之一。要不要改成 2 倍像素倍增铺满全屏，
*         等这一版量出帧率之后再定。
*
*         **着色。** 每个三角面一个亮度，不做逐像素插值：
*         面法线在模型空间里一次算好（启动时），每帧把光向反着转进模型空间，
*         再与面法线点积。这样每帧的着色工作量是「三角形数」而不是「像素数」，
*         396 次点积对这颗 CPU 可以忽略。全整数，没有浮点——
*         这颗 CPU 是 rv32im，没有 FPU，浮点会落到软浮点上。
*
*         **z-buffer** 用 small3dlib 的减量格式（每像素 1 字节）并套上
*         `cup_zbuf.h` 的偏移压缩，理由写在那个文件里。
*
*         第三方：`sw/lib/s3l/small3dlib.h`（CC0），登记见 `sw/README.md`。
*
******************************************************************************/

#include <stdint.h>

#include "board.h"
#include "memmap.h"
#include "core/format.h"

/* --- small3dlib 的配置，必须在 include 之前 --------------------------- */

#define S3L_RESOLUTION_X    320
#define S3L_RESOLUTION_Y    240

/* 减量 z-buffer：每像素 1 字节。320x240 = 76800 字节，放 BSS（在 DDR 里）。
   读会装缓存行，同一条扫描线上的相邻像素命中，所以它不是每像素一次 DDR 往返。 */
#define S3L_Z_BUFFER        2

/* 平面着色：本程序自己按三角面给亮度，不需要库里的逐像素插值。 */
#define S3L_FLAT            1

/* 透视校正对平面着色没有意义，关掉省一大块每像素的除法。 */
#define S3L_PERSPECTIVE_CORRECTION 0

#define S3L_PIXEL_FUNCTION  draw_pixel

#include "cup_zbuf.h"       /* 必须在 small3dlib.h 之前：它重定义 S3L_zBufferFormat */
#include "s3l/small3dlib.h"
#include "cup_model.h"

/* --- 构图参数 ----------------------------------------------------------- */

/* 相机后退 5 个单位，物体重心在原点。改这个要连带核对 cup_zbuf.h 的两个常数。 */
#define CAM_Z           (-5 * S3L_F)
/* 变焦系数。small3dlib 两个方向共用 RESOLUTION_X/2 做比例，4:3 下要按高折算，
   否则垂直可视范围缩到 3/4，杯口会被裁掉。 */
#define ZOOM            4
/* 往前倾一点才看得见杯口内壁。 */
#define TILT_DIV        8

/* 画面在 640x480 里的位置：居中。 */
#define VIEW_X0         ((FB_WIDTH  - S3L_RESOLUTION_X) / 2)
#define VIEW_Y0         ((FB_HEIGHT - S3L_RESOLUTION_Y) / 2)

#ifndef FRAMES
#define FRAMES          240u
#endif

/* --- 状态 --------------------------------------------------------------- */

static uint32_t *g_fb;                          /* 本帧要画的那一面 */
static uint8_t   g_shade[CUP_TRIANGLE_COUNT];   /* 每个三角面这一帧的亮度 0~255 */
static S3L_Vec4  g_normal[CUP_TRIANGLE_COUNT];  /* 模型空间的面法线，启动时算一次 */

static char      s_line[120];
static core_out  s_out;

static void line_begin(const char *s)
{
    core_out_init(&s_out, s_line, (uint32_t)sizeof s_line);
    core_out_str(&s_out, s);
}

static void line_end(void)
{
    core_out_str(&s_out, "\r\n");
    board_puts(s_line);
}

/* --- 像素回调 ----------------------------------------------------------- */

/* 亮度 -> XRGB8888。杯子给一个偏暖的白瓷色，亮度只缩放三个通道。 */
static uint32_t shade_to_rgb(uint32_t i)
{
    uint32_t r = (i * 255u) >> 8;
    uint32_t g = (i * 246u) >> 8;
    uint32_t b = (i * 230u) >> 8;
    return (r << 16) | (g << 8) | b;
}

void draw_pixel(S3L_PixelInfo *p)
{
    uint32_t *row = g_fb + (uint32_t)(VIEW_Y0 + p->y) * FB_ROW_PIXELS + VIEW_X0;
    row[p->x] = shade_to_rgb(g_shade[p->triangleIndex]);
}

/* --- 启动时算一次：模型空间的面法线 ------------------------------------ */

static void build_face_normals(void)
{
    uint16_t t;

    for (t = 0; t < CUP_TRIANGLE_COUNT; t++) {
        S3L_Index i0 = cupTriangles[t * 3 + 0];
        S3L_Index i1 = cupTriangles[t * 3 + 1];
        S3L_Index i2 = cupTriangles[t * 3 + 2];
        S3L_Vec4 v0, v1, v2, n;

        S3L_vec4Set(&v0, cupVertices[i0 * 3 + 0], cupVertices[i0 * 3 + 1],
                         cupVertices[i0 * 3 + 2], 0);
        S3L_vec4Set(&v1, cupVertices[i1 * 3 + 0], cupVertices[i1 * 3 + 1],
                         cupVertices[i1 * 3 + 2], 0);
        S3L_vec4Set(&v2, cupVertices[i2 * 3 + 0], cupVertices[i2 * 3 + 1],
                         cupVertices[i2 * 3 + 2], 0);

        /* 库自带的：叉积加归一化，结果以 S3L_F 为单位长。 */
        S3L_triangleNormal(v0, v1, v2, &n);
        g_normal[t] = n;
    }
}

/* --- 每帧：把光向转进模型空间，给每个面算亮度 -------------------------- */

static void shade_faces(S3L_Unit ay)
{
    /* 世界光向取右上前方，归一化后的分量直接写死（整数，S3L_F 为 1）。
       模型绕 Y 转了 ay，等价于光向绕 Y 反转 ay 之后再在模型空间里点积——
       这样每帧只转一个向量，而不是转 396 条法线。 */
    S3L_Unit c = S3L_cos(-ay);
    S3L_Unit s = S3L_sin(-ay);

    const S3L_Unit lx = (S3L_Unit)((S3L_F * 40) / 100);
    const S3L_Unit ly = (S3L_Unit)((S3L_F * 70) / 100);
    const S3L_Unit lz = (S3L_Unit)((-S3L_F * 59) / 100);

    S3L_Unit mx = (lx * c - lz * s) / S3L_F;
    S3L_Unit my = ly;
    S3L_Unit mz = (lx * s + lz * c) / S3L_F;

    uint16_t t;
    for (t = 0; t < CUP_TRIANGLE_COUNT; t++) {
        S3L_Vec4 n = g_normal[t];
        /* 点积除以 S3L_F 之后落在 [-S3L_F, S3L_F]。 */
        int32_t d = (n.x * mx + n.y * my + n.z * mz) / S3L_F;
        int32_t i;

        if (d < 0)
            d = -d;                 /* 两面都画（看得见内壁），背面按同样的亮度 */

        /* 环境光 56 + 漫反射 200，落在 56~256，夹到 255。 */
        i = 56 + (d * 200) / S3L_F;
        if (i > 255) i = 255;
        g_shade[t] = (uint8_t)i;
    }
}

/* --- 每帧：只清画面那一块，不清整屏 ------------------------------------ */

static void clear_view(uint32_t *fb)
{
    uint32_t y;
    for (y = 0; y < S3L_RESOLUTION_Y; y++) {
        uint32_t *row = fb + (uint32_t)(VIEW_Y0 + y) * FB_ROW_PIXELS + VIEW_X0;
        uint32_t x;
        for (x = 0; x < S3L_RESOLUTION_X; x++)
            row[x] = 0;
    }
}

/* 两面帧缓冲的边框各清一次就够：画面区域之外每帧都不再写。 */
static void clear_border_once(void)
{
    int i;
    for (i = 0; i < BOARD_FB_COUNT; i++) {
        uint32_t *fb = board_fb(i);
        uint32_t y;
        for (y = 0; y < FB_HEIGHT; y++) {
            uint32_t *row = fb + y * FB_ROW_PIXELS;
            uint32_t x;
            for (x = 0; x < FB_WIDTH; x++)
                row[x] = 0;
        }
    }
}

int main(void)
{
    S3L_Model3D model;
    S3L_Scene   scene;
    uint32_t    frame;
    /* 累加量一律 64 位：board_cycles() 是 32 位回绕的，100MHz 下 2^32 个周期只有
       42.9 秒（board.h 写明了这一条）。本程序一帧就要 500 万个周期，
       900 帧是 45 亿——既不能拿首尾两个读数相减（区间超过 42.9 秒就回绕），
       32 位的累加器也存不下。每帧取一次区间（区间本身远小于 42.9 秒，相减成立），
       累加到 64 位里。 */
    uint32_t    t_frame;
    uint64_t    t_total = 0;
    uint64_t    acc_clear = 0, acc_shade = 0, acc_raster = 0, acc_present = 0;
    int         idx = 0;

    board_init();
    line_begin("cup3d tris=");
    core_out_u32(&s_out, CUP_TRIANGLE_COUNT);
    core_out_str(&s_out, " res=");
    core_out_u32(&s_out, S3L_RESOLUTION_X);
    core_out_ch(&s_out, 'x');
    core_out_u32(&s_out, S3L_RESOLUTION_Y);
    line_end();

    build_face_normals();
    clear_border_once();

    S3L_model3DInit((S3L_Unit *)cupVertices, CUP_VERTEX_COUNT,
                    (S3L_Index *)cupTriangles, CUP_TRIANGLE_COUNT, &model);
    /* 凹物体，看得见内壁：关剔除，两面都画，靠 z-buffer 定前后。 */
    model.config.backfaceCulling = 0;

    S3L_sceneInit(&model, 1, &scene);
    scene.camera.transform.translation.z = CAM_Z;
    scene.camera.focalLength = (ZOOM * S3L_F * S3L_RESOLUTION_Y) / S3L_RESOLUTION_X;

    for (frame = 0; frame < FRAMES; frame++) {
        S3L_Unit ay = (S3L_Unit)((frame * (S3L_F / 48)) % (4 * S3L_F));
        uint32_t m;
        uint32_t f0 = board_cycles();

        g_fb = board_fb(idx);

        m = board_cycles();
        clear_view(g_fb);
        S3L_zBufferClear();
        acc_clear += board_cycles() - m;

        m = board_cycles();
        model.transform.rotation.x = S3L_F / TILT_DIV;
        model.transform.rotation.y = ay;
        shade_faces(ay);
        acc_shade += board_cycles() - m;

        m = board_cycles();
        S3L_drawScene(scene);
        acc_raster += board_cycles() - m;

        m = board_cycles();
        board_present(idx);
        acc_present += board_cycles() - m;

        idx ^= 1;
        t_frame = board_cycles() - f0;
        t_total += t_frame;
    }

    {
        uint32_t per_frame = (uint32_t)(t_total / FRAMES);

        line_begin("frames=");
        core_out_u32(&s_out, FRAMES);
        core_out_str(&s_out, " per_frame=");
        core_out_u32(&s_out, per_frame);
        core_out_str(&s_out, " fps_x100=");
        core_out_u32(&s_out, (BOARD_HZ / per_frame) * 100u);
        line_end();

        line_begin("per_frame clear=");
        core_out_u32(&s_out, (uint32_t)(acc_clear / FRAMES));
        core_out_str(&s_out, " shade=");
        core_out_u32(&s_out, (uint32_t)(acc_shade / FRAMES));
        core_out_str(&s_out, " raster=");
        core_out_u32(&s_out, (uint32_t)(acc_raster / FRAMES));
        core_out_str(&s_out, " present=");
        core_out_u32(&s_out, (uint32_t)(acc_present / FRAMES));
        line_end();
    }

    board_puts("done\r\n");
    return 0;
}
