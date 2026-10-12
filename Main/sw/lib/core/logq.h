/******************************************************************************
* @file logq.h: 应用侧日志队列
*
* @brief  板上 sdk 的 uart_write 忙等发送 FIFO，115200 波特每帧只发得出约 192 字节；
*         一行 CSV 约 200 字节，一次写完会卡住当帧几毫秒，自己制造掉帧。所以日志先进这个
*         环形队列，每帧只取 board_tx_room() 个字节交给 board_puts()。
*
*         口径：
*           - 固定容量，存储由调用方给，没有 malloc。
*           - **一行要么整行进，要么整行丢**，不拆行；丢掉的行数单独计。
*             拆开的半行会让下游按行解析的 CSV 读错，整行丢掉只是少一行。
*           - 行在存储里可以跨过回绕点；取出来的字节总是按写入顺序连续。
*           - 取数按给定的字节数取，取完在输出末尾补一个 '\0'，可以直接交给 board_puts()。
*
*         用法（每帧）：
*             char out[LOG_TX_MAX + 1];
*             uint32_t room = board_tx_room();
*             if (room > LOG_TX_MAX) room = LOG_TX_MAX;
*             if (logq_take(&q, out, room) > 0u) board_puts(out);
*
*         注错开关（主机测试 lib/test/test_stats.c 用，见 check_sw8_stats.sh）：
*           INJECT_LOGQ_SPLIT  放不下时把放得下的那一截写进去（拆行）
*           INJECT_LOGQ_WRAP   写入回绕时落到下标 1 而不是 0
******************************************************************************/
#ifndef GFX2D_CORE_LOGQ_H
#define GFX2D_CORE_LOGQ_H

#include <stdint.h>

typedef struct {
    char    *buf;
    uint32_t cap;           /* 存储字节数 */
    uint32_t head;          /* 下一个要取的字节 */
    uint32_t len;           /* 已存的字节数 */
    uint32_t dropped;       /* 整行丢掉的行数，累计 */
} logq;

void     logq_init(logq *q, char *storage, uint32_t cap);

/* 追加一行（以 '\0' 结尾的字符串，换行符由调用方写进去）。
   整行放得下返回 0；放不下整行丢掉、丢弃计数加一，返回 -1。空串返回 0、什么也不做。 */
int      logq_put(logq *q, const char *line);

/* 按写入顺序取出至多 max 个字节到 out，并在 out[n] 写 '\0'，返回 n。
   **out 至少要有 max + 1 个字节。** */
uint32_t logq_take(logq *q, char *out, uint32_t max);

uint32_t logq_used(const logq *q);
uint32_t logq_free(const logq *q);
uint32_t logq_dropped(const logq *q);

#endif /* GFX2D_CORE_LOGQ_H */
