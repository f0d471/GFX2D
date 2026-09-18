/*
 * bootSizeTest 的数据块：约 300KB，让镜像超过官方 bootloader 的 124KB 拷贝长度。
 * 第 i 个字 = (i * 2654435761 + 0x5A5A5A5A) 取低 32 位。内容随位置变化，
 * 截断、错位、全零都会让校验和对不上。
 */
    .equ BLOB_WORDS, 75000

    .section .rodata.blob, "a"
    .balign 4
    .global blob_start
    .global blob_end
blob_start:
    .set i, 0
    .rept BLOB_WORDS
    .long ((i * 2654435761) + 0x5A5A5A5A) & 0xFFFFFFFF
    .set i, i + 1
    .endr
blob_end:
