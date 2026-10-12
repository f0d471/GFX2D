/******************************************************************************
* @file format.h: 轻量格式化输出
*
* @brief  应用里不出现 printf，日志要么是固定的字符串，
*         要么是这里拼出来的。不引 libc 的 printf 家族：那会把浮点与一大票
*         格式化逻辑链进来。
*
*         **为什么没有 printf 那样的变参接口。** 两个平台上 `uint32_t` 不是同一个
*         类型：板上这份工具链把它定义成 `unsigned long`，PC（LP64）上是
*         `unsigned int`。变参函数里读参数必须写死一个类型，于是
*         `va_arg(ap, unsigned int)` 在板上就是类型不匹配，而 gcc 的
*         `format(printf)` 检查会直接把它报成错误（实测过）。
*         与其靠「反正都是 32 位、传参方式一样」蒙过去，不如不要变参：
*         这里是一组按类型追加的函数，两端行为与类型都是确定的。
*
*         用法：
*             char line[80];
*             core_out o;
*             core_out_init(&o, line, (uint32_t)sizeof line);
*             core_out_str(&o, "frame ");
*             core_out_u32(&o, f);
*             core_out_str(&o, " crc ");
*             core_out_hex32(&o, crc);
*             core_out_str(&o, "\r\n");
*
*         截断时按 snprintf 的语义：core_out_len 返回「本应写入的长度」，
*         缓冲区总是以 '\0' 结尾（cap 为 0 时一个字都不写）。
******************************************************************************/
#ifndef GFX2D_CORE_FORMAT_H
#define GFX2D_CORE_FORMAT_H

#include <stdint.h>

typedef struct {
    char    *buf;
    uint32_t cap;
    uint32_t n;     /* 本应写入的字符数，可能超过 cap-1 */
} core_out;

void     core_out_init(core_out *o, char *buf, uint32_t cap);

void     core_out_ch(core_out *o, char c);
void     core_out_str(core_out *o, const char *s);

/* 十进制，无前导零 */
void     core_out_u32(core_out *o, uint32_t v);

/* 十进制，右对齐补空格到 width 列（width 小于位数时不截断） */
void     core_out_u32_pad(core_out *o, uint32_t v, uint32_t width);

/* 恰好 8 位小写十六进制，用于 CRC 这类定宽字段 */
void     core_out_hex32(core_out *o, uint32_t v);

/* 本应写入的字符数，与 snprintf 的返回值同义 */
uint32_t core_out_len(const core_out *o);

#endif /* GFX2D_CORE_FORMAT_H */
