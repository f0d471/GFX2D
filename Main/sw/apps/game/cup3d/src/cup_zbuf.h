/* cup_zbuf.h —— 茶杯专用的减量 z-buffer 深度压缩（板上与 PC 预览共用）
 * ============================================================================
 * 必须在 #include "small3dlib.h" **之前** include。
 *
 * 问题: small3dlib 的减量 z-buffer 是 `min(255, depth >> G)`, 默认 G=5。
 * 它把 8 位精度摊在 [0, 255<<5] = [0, 8160] 的整个深度轴上, 而本场景里
 * 物体只占 1982..3138 这一小段:
 *      相机在 z = -5*S3L_F = -2560, 物体重心在原点,
 *      模型旋转半径 sqrt(404^2+256^2+323^2) = 578
 *   => 深度区间 2560 +- 578
 * 于是整只杯子只能分出 (3138>>5) - (1982>>5) = 98-61 = **37 级**。
 * 把手与杯身在深度上本来就挨得近, 37 级不够分 —— 表现为把手边缘与杯身
 * 互相判错遮挡, 转到某些角度时把手像是"断开/飞出去"。
 *
 * 解法: 先减掉偏移再压缩, 把 8 位全用在有效区间上。
 *      G=3, offset=1900 => (1982-1900)>>3 = 10 .. (3138-1900)>>3 = 154
 *   => **144 级**, 是原来的 3.9 倍, 且零内存代价(还是 uint8)。
 *
 * 两个边界都必须夹:
 *    - 下界: depth < offset 时 (depth-offset) 为负, 右移负数是 UB ⇒ 必须 max(0,...)。
 *    - 上界: 超出区间的远景压到 255 = 背景深度, 正好被当成"最远", 语义正确。
 * 改相机距离 / 模型尺度 / LS_ZOOM 之外的构图参数时, 要回来核对这两个常数。
 *    判据在 tools/object3d_bake/: 用 PV_ZBUF=1(32 位 z-buffer)出金标准, 逐像素比。
 */
#ifndef CUP_ZBUF_H
#define CUP_ZBUF_H

#define S3L_REDUCED_Z_BUFFER_GRANULARITY 3
#define CUP_Z_OFFSET 1900

#define S3L_zBufferFormat(depth)                                   \
    S3L_min(255, S3L_max(0, ((depth) - CUP_Z_OFFSET))              \
                 >> S3L_REDUCED_Z_BUFFER_GRANULARITY)

#endif /* CUP_ZBUF_H */
