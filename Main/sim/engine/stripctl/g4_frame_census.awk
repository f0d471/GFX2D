# G4 前期研究：rcsv 探针行的逐档聚合（engine 第 10 章 3.3 节）。
#
# 输入是带 GFX2D_STRIP_RESEARCH=1 构建的 gfx2dGame（host 平台）打的行：
#   rcsv,u1,u2,pk16,cyc2,u3,pk32,cyc3,err2,err3,eqok
# 两种用法：
#   全程搜索日志（含 nmax probe/result 行，按 result 切段：空段号=16x16、
#   段 1=8x8）：
#     gawk -f g4_frame_census.awk g4_mode3.log
#   固定旋钮日志（无 nmax 行，段号固定为 F，档位从 -v 传）：
#     gawk -v fixed_k=2400 -f g4_frame_census.awk g4_k2400.log
#
# 输出每档一行：u1/u2/u3 均值、峰值桶 max（两口径）、组帧周期均值、
# 错误合计与对账通过帧数。err 应为 0，eqok 应等于帧数。
/^nmax result / { seg++ }
/^nmax probe [0-9]+ (double|bisect|confirm) knob / {
    split($0, f, " ")
    K = seg ":" f[6] + 0
}
/^rcsv,/ {
    n = split($0, a, ",")
    if (fixed_k != "") key = "F:" fixed_k
    else {
        if (K == "") K = "0:0"
        key = K
    }
    u1[key] += a[2]
    u2[key] += a[3]
    u3[key] += a[6]
    if (a[4] + 0 > p16[key]) p16[key] = a[4] + 0
    if (a[7] + 0 > p32[key]) p32[key] = a[7] + 0
    if (a[2] + 0 > m1[key]) m1[key] = a[2] + 0
    c2[key] += a[5]
    c3[key] += a[8]
    err[key] += a[9] + a[10]
    eq[key] += (a[11] == 1)
    cnt[key]++
}
END {
    for (k in cnt)
        printf "%s frames=%d u1avg=%.1f u1max=%d u2avg=%.1f ratio16=%.3f pk16=%d u3avg=%.1f ratio32=%.3f pk32=%d cyc2=%.0f cyc3=%.0f err=%d eqok=%d\n", \
            k, cnt[k], u1[k] / cnt[k], m1[k], u2[k] / cnt[k], u2[k] / u1[k], p16[k], u3[k] / cnt[k], u3[k] / u1[k], p32[k], c2[k] / cnt[k], c3[k] / cnt[k], err[k], eq[k]
}
