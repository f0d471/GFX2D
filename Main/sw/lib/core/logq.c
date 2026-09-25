/******************************************************************************
* @file logq.c: 应用侧日志队列
*
* @brief  口径见 logq.h 与 SW8 2.4 节。存储是一段环形缓冲，head 指下一个要取的字节，
*         len 是已存的字节数；写入位置是 head + len（回绕到存储里）。不用取模，比较一次再减。
******************************************************************************/

#include "core/logq.h"

void logq_init(logq *q, char *storage, uint32_t cap)
{
    q->buf     = storage;
    q->cap     = (storage == 0) ? 0u : cap;
    q->head    = 0u;
    q->len     = 0u;
    q->dropped = 0u;
}

int logq_put(logq *q, const char *line)
{
    uint32_t n = 0u, i, tail;

    while (line[n] != '\0')
        n++;
    if (n == 0u)
        return 0;

    if (n > q->cap - q->len) {
#ifdef INJECT_LOGQ_SPLIT
        n = q->cap - q->len;            /* 注错：把放得下的那一截写进去 */
        if (n == 0u) {
            q->dropped++;
            return -1;
        }
#else
        q->dropped++;
        return -1;
#endif
    }

    tail = q->head + q->len;
    if (tail >= q->cap)
        tail -= q->cap;
    for (i = 0u; i < n; i++) {
        q->buf[tail] = line[i];
        tail++;
        if (tail == q->cap)
#ifdef INJECT_LOGQ_WRAP
            tail = 1u;                  /* 注错：回绕落到下标 1 */
#else
            tail = 0u;
#endif
    }
    q->len += n;
    return 0;
}

uint32_t logq_take(logq *q, char *out, uint32_t max)
{
    uint32_t n = (max < q->len) ? max : q->len;
    uint32_t i;

    for (i = 0u; i < n; i++) {
        out[i] = q->buf[q->head];
        q->head++;
        if (q->head == q->cap)
            q->head = 0u;
    }
    q->len -= n;
    out[n] = '\0';
    return n;
}

uint32_t logq_used(const logq *q)
{
    return q->len;
}

uint32_t logq_free(const logq *q)
{
    return q->cap - q->len;
}

uint32_t logq_dropped(const logq *q)
{
    return q->dropped;
}
