/******************************************************************************
* @file replay.h: 录像的格式与逐帧取键
*
* @brief  一段按键序列。板上没有文件系统，录像要编进程序，所以格式定成
*         **定长、无对齐洞、多字节量显式小端**：LP64 与 ILP32 解析出同一串按键，
*         从二进制一步转成 C 数组即可（`Main/tools/check/replay2c.py`）。
*
*         这一段是纯算法，两个平台编同一份，不依赖任何平台头文件：
*         文件读写不在这一层（PC 上也不需要，录制结果就是日志），
*         逐帧取键与逐帧追加是应用的事，本层不认识「回放模式」。
*
*         字节布局（偏移：含义），总长 16 + 4N：
*
*             0  4  魔数 'G','2','D','R'，小端读作 REPLAY_MAGIC
*             4  2  版本号 REPLAY_VERSION
*             6  2  标志位，当前必须是 0
*             8  4  随机种子
*            12  4  帧数 N
*            16  4N 每帧一个 32 位按键位图，位分配同 board.h 的 BOARD_KEY_*
*
*         解析逐字节读写，不把缓冲强制转成结构体：对齐、别名与端序三件事
*         一件都不用赌。
******************************************************************************/
#ifndef GFX2D_CORE_REPLAY_H
#define GFX2D_CORE_REPLAY_H

#include <stdint.h>

#define REPLAY_MAGIC        0x52443247u     /* 文件头前四字节是 'G','2','D','R' */
#define REPLAY_VERSION      1u
#define REPLAY_HEADER_BYTES 16u
#define REPLAY_RECORD_BYTES 4u

/* 解析失败的原因。都不是按键内容的问题，是证据载体坏了。 */
#define REPLAY_E_MAGIC      (-1)
#define REPLAY_E_VERSION    (-2)
#define REPLAY_E_FLAGS      (-3)
#define REPLAY_E_LEN        (-4)
#define REPLAY_E_RANGE      (-5)

/* 一段 frames 帧的录像占多少字节。 */
uint32_t replay_bytes_for(uint32_t frames);

/* 把 frames 帧按键一次打包进 buf。成功返回总字节数；
   cap 不够时一个字都不写，返回 0（不截断，截断过的录像没有意义）。 */
uint32_t replay_pack(uint8_t *buf, uint32_t cap, uint32_t seed,
                     const uint32_t *keys, uint32_t frames);

/* --- 流式录制 ----------------------------------------------------------- */
/* 一帧一条按键，缓冲满就拒绝。满不是失败：录到哪算哪，
   帧数以 replay_rec_end 写回头部的为准。 */
typedef struct {
    uint8_t *buf;
    uint32_t cap;
    uint32_t frames;        /* 已成功追加的帧数 */
    uint32_t seed;
    int      full;
} replay_rec;

void     replay_rec_begin(replay_rec *r, uint8_t *buf, uint32_t cap, uint32_t seed);
int      replay_rec_put(replay_rec *r, uint32_t keys);   /* 0 成功，-1 缓冲满 */
uint32_t replay_rec_frames(const replay_rec *r);

/* 收尾：把头部的帧数改成实际录到的帧数。返回总字节数。 */
uint32_t replay_rec_end(replay_rec *r);

/* --- 读取 --------------------------------------------------------------- */
typedef struct {
    const uint8_t *buf;
    uint32_t       len;
    uint32_t       seed;
    uint32_t       frames;
} replay_view;

/* 校验并解析。返回 0 成功，负值是 REPLAY_E_*。out 只在成功时被改写。 */
int replay_open(const uint8_t *buf, uint32_t len, replay_view *out);

/* 取第 frame 帧的按键。越界返回非 0，*keys 不被改写。 */
int replay_key_at(const replay_view *v, uint32_t frame, uint32_t *keys);

/* --- 录制文本 ----------------------------------------------------------- */
/* 录制结果就是日志：应用把打包好的录像按下面两种行经 board_puts 打出，
   `Main/tools/check/replay2c.py` 再把这段文本收成 .g2dr 或 C 数组。
   行的格式只在这里定义一处，脚本照它解析。

     g2dr 1 <seed> <frames>            头一行
     key <frame> <8 位十六进制>         之后每帧一行

   返回写进去的字符数；缓冲不够返回 0。 */
uint32_t replay_text_header(uint32_t seed, uint32_t frames, char *buf, uint32_t cap);
uint32_t replay_text_key(uint32_t frame, uint32_t keys, char *buf, uint32_t cap);

#endif /* GFX2D_CORE_REPLAY_H */
