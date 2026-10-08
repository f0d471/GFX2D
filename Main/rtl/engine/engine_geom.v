// engine_geom：引擎 P1 B3 的几何层
//
// 接一条已经通过十步检查的 FILL 或 BLIT（I1），算出它实际要画的矩形，拆成行，
// 每行给搬运层发读源、读目的、写目的的描述符（I2），给计算阵列发这一行的边带（I5）。
// 这一层不碰像素，只碰坐标与地址；它一错，错的都是地址。
//
// 四件事：
//
//   1. 五者求交（D34）。目的矩形、游戏区 [0,639]×[0,479]、当前裁剪框、目的表面、
//      源表面；FILL 没有源，只交前四项。中间量一律 32 位有符号，空宽高先判。
//      源表面那一项是 spec 第 01 章 3.6 节第 5 步「源越界只跳过该像素」在矩形级的
//      等价写法：[dst_x−src_x, dst_x−src_x+src_w−1] × 同理的 y，交掉之后像素通路
//      不必再逐像素判源越界。
//
//   2. 行迭代（D33）。多拍顺序实现，每行 4 拍（边带 1 拍、每笔描述符 1 拍、
//      行推进 1 拍）。实测对照搬运层的每行周期数余量 2.4 倍以上，不必做流水。
//      每一行开始之前等这一行要用的那一半行缓冲空着（row_free，D41）。
//
//   3. 描述符（D35、D3 规矩 1）。每行按 16 字节对齐的覆盖区间发：下界向下、上界向上
//      取整到 16 字节，可能带出不属本行的像素（由 D16 的带内标记不写）。覆盖区间
//      按「不跨 4KB」与「不超过 AXI_MAX_BURST_LEN 拍」拆段。P1 里一行最多两段、
//      最多 161 拍（640 像素 = 2560 字节，取整后最多 2576；< 4096 且 < 3072），
//      P2/P3 或改 beat 宽度后要重算。
//
//   4. D7 读写避让（D37）。只有两条判定：读源 ⟺ 操作是 BLIT；读目的 ⟺ 方程不是 COPY。
//      两者都不改变逐像素结果：FILL 是常量源；COPY 不看目的，色键命中的像素走 D16
//      的带内标记（目的原值不动）。
//
// 接口（总览 2.4 节 I1、I2、I5，本章新增的字段已回写）：
//   I1 控制层 → 本层：一条命令的全部字段，start 一拍脉冲提交。零面积命令照常下发，
//      只有本层求交之后才知道它是空的（2026-10-01 订正）。
//   I2 本层 → 搬运层：读描述符与写描述符，每笔恰好一个突发。
//   I5 本层 → 计算阵列：每行一条边带，先于本行的读描述符发出，B4 在数据回来之前
//      就知道这一行的相位与掩码。
//   D38 零面积命令的完成：本层回一拍 cmd_done（带命令索引）。非空命令的完成由
//      最后一个写描述符的 wr_last_row 带出——**只打在最后一行的最后一段上**，
//      最后一行跨 4KB 拆成两段时第一段不打，否则控制层会收到两次完成。
//
// 发出顺序（测试台的期望值按同一顺序，见 tools/check/geom_model.py 的 REC_* 表）：
//   每行 = 边带(I5) → 读源(I2) → 读目的(I2) → 写目的(I2)
//   读描述符的目标行缓冲号 rd_buf = {源/目的, 行号最低位}：位 1 = 0 源 / 1 目的，
//   位 0 是行缓冲的两半（乒乓），按行号最低位翻转。
//
// row_free[1:0]：本层与 B4 之间的握手，电平，每一半一位：row_free[h] 高表示
//   源、目的两块行缓冲的第 h 半都空着（D23 的行缓冲是两块各两半）。
//   本层在**每一行**发边带之前查这一行要用的那一半 row_free[行号最低位]，
//   包括每条命令的第 0 行：行号每条命令从 0 起，上一条命令的最后几行可能还占着第 0 半，
//   只在命令内部的行推进上查会让下一条命令的第 0 行盖掉它（2026-10-01 复核）。
//   约定：B4 只在第 h 半的边带握手之后才把 row_free[h] 拉低，所以本层看到它高之后
//   到边带握手之间它不会掉。B4 已确认这套编码与时机（D45：该半写流的末拍被写核收走之后才拉高）。
//
// 注错（变异测试）：本文件是干净形态，五处变异各包在 `ifdef 内。
// 都不定义时它们不进编译输入（预处理阶段已删除），不依赖综合器优化。
// 注错改的都是本文件里真实的逻辑，由测试台与独立模型逐条比对抓出来；
// 编译开关与 sim/engine/geom/run.sh 的用例一一对应，一次只开一个：
//   INJ_SKIP_SURF    漏掉第五项求交（源表面）
//   INJ_NO_SPLIT     跨 4KB 不拆：一个描述符盖过页边界，违反 D3 规矩 1
//   INJ_ALPHA_AS_COPY ALPHA 也按 COPY 走「不读目的」的避让，读描述符少一半
//   INJ_COPY_READ     COPY 也发读目的描述符（边带仍说不读）：多发的描述符
//   INJ_ROW_NOFREE    行开始之前不看 row_free：下一行的读盖掉还没写完的那一半
//
// 设计依据、算式、位宽、定案 D33~D40 与实测数据见
// Engineering/TechReports/engine/03-几何层.md。
`timescale 1ns/1ps
`default_nettype none

module engine_geom #(
    // D3 规矩 1 的拍数上限，与搬运层两个核的 AXI_MAX_BURST_LEN 同一个值
    parameter integer MAX_BEATS = 192
)(
    input  wire        clk,
    input  wire        rst,

    // ---- I1：控制层 → 几何层 ----
    input  wire        start,        // 一拍脉冲：提交一条已经通过十步检查的 FILL/BLIT
    input  wire [7:0]  op,           // 1 FILL、2 BLIT（其余不到本层）
    input  wire [3:0]  eq,           // 0 COPY、1 ALPHA、2 ADD、3 CONST、4 MUL、5 ALPHA_STRAIGHT
    input  wire        key_en,
    input  wire [31:0] key_w5,       // 色键字
    input  wire [31:0] const_w4,     // FILL 的常量源字
    input  wire [7:0]  c,            // CONST 方程用的 c
    input  wire [3:0]  sfmt,         // 源格式（FILL 由方程决定，BLIT 取源表面）
    input  wire [31:0] src_base,
    input  wire [15:0] src_w,
    input  wire [15:0] src_h,
    input  wire [4:0]  src_slog2,
    input  wire        src_tex,       // 源在片上 TEX（G1，I1 的新字段；仅 BLIT 源会置 1）
    input  wire [31:0] dst_base,
    input  wire [15:0] dst_w,
    input  wire [15:0] dst_h,
    input  wire [4:0]  dst_slog2,
    input  wire signed [15:0] src_x,
    input  wire signed [15:0] src_y,
    input  wire signed [15:0] dst_x,
    input  wire signed [15:0] dst_y,
    input  wire [15:0] w,
    input  wire [15:0] h,
    input  wire signed [15:0] clip_x0,
    input  wire signed [15:0] clip_y0,
    input  wire signed [15:0] clip_x1,
    input  wire signed [15:0] clip_y1,
    input  wire [3:0]  cidx,         // 命令索引（D19 的错误归属）
    input  wire [1:0]  row_free,     // B4 的行缓冲空闲握手（电平，每一半一位，D41）

    output reg         busy,
    output reg         cmd_done,     // D38：零面积命令的完成脉冲
    output reg  [3:0]  done_cidx,

    // ---- I2：读描述符 ----
    output reg  [31:0] rd_addr,
    output reg  [11:0] rd_beats,
    output reg  [1:0]  rd_buf,
    output reg         rd_tex,        // 源读描述符的片上标记（G1）：仅 BLIT 源可能置 1
    output reg  [3:0]  rd_cidx,
    output reg         rd_valid,
    input  wire        rd_ready,

    // ---- I2：写描述符 ----
    output reg  [31:0] wr_addr,
    output reg  [11:0] wr_beats,
    output reg  [3:0]  wr_cidx,
    output reg         wr_last_row,
    output reg         wr_valid,
    input  wire        wr_ready,

    // ---- I5：每行边带 ----
    output reg  [15:0] sb_row,       // 本命令内的零基行号
    output reg  [15:0] sb_py,        // 目的面上的绝对 y
    output reg  [10:0] sb_npx,       // 本行像素数
    output reg  [7:0]  sb_beats,     // 本行写拍数（两段之和）
    output reg  [1:0]  sb_lead_dst,  // 目的行首地址的拍内像素相位
    output reg  [1:0]  sb_lead_src,  // 源行首地址的拍内像素相位
    output reg  [7:0]  sb_op,
    output reg  [3:0]  sb_eq,
    output reg         sb_key_en,
    output reg  [31:0] sb_key,
    output reg  [31:0] sb_w4,
    output reg  [7:0]  sb_c,
    output reg  [3:0]  sb_sfmt,
    output reg         sb_read_dst,  // 这一行读不读目的（D7）
    output reg         sb_valid,
    input  wire        sb_ready
);
    localparam [7:0] OP_FILL = 8'd1;
    localparam [7:0] OP_BLIT = 8'd2;
    localparam [3:0] EQ_COPY = 4'd0;
    localparam [3:0] EQ_ALPHA = 4'd1;

    localparam [2:0] S_IDLE = 3'd0, S_ROW = 3'd1, S_ADV = 3'd2, S_DONE = 3'd3,
                     S_GATE = 3'd4;

    reg [2:0]  st;
    reg [2:0]  seg_i;                    // 0 边带、1/2 读源、3/4 读目的、5/6 写目的
    reg [15:0] row_i;
    reg [31:0] dst_cur, src_cur;         // 本行的行首字节地址
    reg [31:0] nb;                       // 本行字节数 = npx*4
    reg [31:0] dst_stride, src_stride;   // 1 << stride_log2
    reg [15:0] nrows;
    reg [10:0] npx;
    reg signed [15:0] py;

    // I1 的锁存副本
    reg [7:0]  r_op;
    reg [3:0]  r_eq;
    reg        r_key_en;
    reg [31:0] r_key, r_w4;
    reg [7:0]  r_c;
    reg [3:0]  r_sfmt;
    reg        r_src_tex;
    reg [3:0]  r_cidx;

    // ---------------------------------------------------------------- 五者求交
    // 纯组合，输入只用 I1 端口值：start 那一拍算一次，结果与全部 I1 字段一起锁存，
    // 命令执行期间不再看 I1。所以这条比较链两头都是寄存器（上游是控制层 B5 的 I1
    // 寄存器，单独综合里是 LFSR；下游是锁存副本与行首地址），D34 的实测就是这条链。
    wire signed [31:0] p_dx = dst_x;
    wire signed [31:0] p_dy = dst_y;
    wire signed [31:0] p_sx = src_x;
    wire signed [31:0] p_sy = src_y;
    wire        [31:0] p_w  = w;
    wire        [31:0] p_h  = h;
    wire        [31:0] p_ssw = src_w;
    wire        [31:0] p_ssh = src_h;
    wire        [31:0] p_dsw = dst_w;
    wire        [31:0] p_dsh = dst_h;

    reg signed [31:0] t_x0, t_y0, t_x1, t_y1;
    reg signed [31:0] dx1, dy1;
    reg signed [31:0] s_hi_x, s_hi_y;
    reg               i_empty;

    always @* begin
        // 纯组合块：先把所有中间量与输出给默认值，少给一条路径就会推断出锁存器
        // （研究探针首版漏了 dx1/dy1 的默认值，Efinity 报 latch inferred）。
        dx1 = 32'sd0; dy1 = 32'sd0;
        t_x0 = 32'sd0; t_y0 = 32'sd0; t_x1 = -32'sd1; t_y1 = -32'sd1;
        s_hi_x = 32'sd0; s_hi_y = 32'sd0;
        i_empty = 1'b1;
        if ((p_w == 32'd0) || (p_h == 32'd0)) begin
            i_empty = 1'b1;
        end else begin
            dx1 = p_dx + $signed(p_w) - 32'sd1;
            dy1 = p_dy + $signed(p_h) - 32'sd1;

            t_x0 = (p_dx > 32'sd0) ? p_dx : 32'sd0;
            t_y0 = (p_dy > 32'sd0) ? p_dy : 32'sd0;
            if (clip_x0 > t_x0) t_x0 = clip_x0;
            if (clip_y0 > t_y0) t_y0 = clip_y0;

            t_x1 = (dx1 < 32'sd639) ? dx1 : 32'sd639;
            t_y1 = (dy1 < 32'sd479) ? dy1 : 32'sd479;
            if (clip_x1 < t_x1) t_x1 = clip_x1;
            if (clip_y1 < t_y1) t_y1 = clip_y1;
            if ($signed(p_dsw) - 32'sd1 < t_x1) t_x1 = $signed(p_dsw) - 32'sd1;
            if ($signed(p_dsh) - 32'sd1 < t_y1) t_y1 = $signed(p_dsh) - 32'sd1;

            // 第五项：源表面
`ifdef INJ_SKIP_SURF
            // 注错：整块跳过第五项求交
`else
            if (op == OP_BLIT) begin
                if (p_dx - p_sx > t_x0) t_x0 = p_dx - p_sx;
                if (p_dy - p_sy > t_y0) t_y0 = p_dy - p_sy;
                s_hi_x = p_dx - p_sx + $signed(p_ssw) - 32'sd1;
                s_hi_y = p_dy - p_sy + $signed(p_ssh) - 32'sd1;
                if (s_hi_x < t_x1) t_x1 = s_hi_x;
                if (s_hi_y < t_y1) t_y1 = s_hi_y;
            end
`endif

            if ((t_x1 < t_x0) || (t_y1 < t_y0)) i_empty = 1'b1;
            else                                    i_empty = 1'b0;
        end
    end

    // 源的行首 x 偏移：sx0 = src_x + (x0 - dst_x)
    wire signed [31:0] sx_off = p_sx + (t_x0 - p_dx);

    // D7 的两条判定（D37）。
    // 声明放在用它的段计算之前：先用后声明在 iverilog 与 Efinity 里能过，换一个工具未必。
    wire d7_read_dst;
`ifdef INJ_ALPHA_AS_COPY
    assign d7_read_dst = ((r_eq == EQ_COPY) || (r_eq == EQ_ALPHA));  // 注错：ALPHA 也按 COPY
`else
    assign d7_read_dst = !(r_eq == EQ_COPY);
`endif
    // 本行发不发读目的描述符。正式配置就是 d7_read_dst；注错版让 COPY 也发，
    // 而边带的 sb_read_dst 仍按 D7 说不读——测试台的握手计数要抓住多出来的那几笔。
`ifdef INJ_COPY_READ
    wire seg_read_dst = 1'b1;             // 注错：COPY 也发读目的描述符
`else
    wire seg_read_dst = d7_read_dst;
`endif

    // ---------------------------------------------------------------- 本行的段
    // 组合量：从 dst_cur / src_cur / nb 算出本行最多六个候选段与一个边带槽。
    reg [31:0] a0, a1, alen;
    reg [11:0] b0, b1;
    reg [31:0] s_a0, s_a1, s_len;
    reg [11:0] s_b0, s_b1;
    reg [12:0] page_left, page_left_s;
    reg [11:0] max_bytes;
    reg        xcross, sxcross;

    reg [31:0] seg_addr  [0:6];
    reg [11:0] seg_beats [0:6];
    reg        seg_iswr  [0:6];
    reg        seg_isrd  [0:6];
    reg        seg_dir   [0:6];      // 读：0 源、1 目的
    reg [6:0]  seg_pres;
    reg [2:0]  nxt_i;
    reg [11:0] wr_total;
    integer    k;

    always @* begin
        max_bytes = MAX_BEATS * 12'd16;      // 192 × 16 = 3072
        // 目的行的覆盖区间
        a0   = {dst_cur[31:4], 4'b0000};
        a1   = (dst_cur + nb + 32'd15) & 32'hFFFFFFF0;
        alen = a1 - a0;
        page_left = 13'h1000 - {1'b0, a0[11:0]};
`ifdef INJ_NO_SPLIT
        xcross = 1'b0;                       // 注错：跨 4KB 不拆
`else
        xcross = (({19'b0, a0[11:0]} + {19'b0, alen[11:0]}) > 32'h1000);
`endif
        if (xcross) begin
            b0 = (page_left > {1'b0, max_bytes}) ? max_bytes : page_left[11:0];
            b1 = alen[11:0] - b0;
        end else begin
            b0 = (alen > {20'b0, max_bytes}) ? max_bytes : alen[11:0];
            b1 = alen[11:0] - b0;
        end
        // 源行的覆盖区间（长度与目的相同，相位可能不同）
        s_a0  = {src_cur[31:4], 4'b0000};
        s_a1  = (src_cur + nb + 32'd15) & 32'hFFFFFFF0;
        s_len = s_a1 - s_a0;
        page_left_s = 13'h1000 - {1'b0, s_a0[11:0]};
`ifdef INJ_NO_SPLIT
        sxcross = 1'b0;                      // 注错：跨 4KB 不拆
`else
        sxcross = (({19'b0, s_a0[11:0]} + {19'b0, s_len[11:0]}) > 32'h1000);
`endif
        if (sxcross) begin
            s_b0 = (page_left_s > {1'b0, max_bytes}) ? max_bytes : page_left_s[11:0];
            s_b1 = s_len[11:0] - s_b0;
        end else begin
            s_b0 = (s_len > {20'b0, max_bytes}) ? max_bytes : s_len[11:0];
            s_b1 = s_len[11:0] - s_b0;
        end
        wr_total = (b0 >> 4) + (b1 >> 4);

        for (k = 0; k <= 6; k = k + 1) begin
            seg_addr[k]  = 32'd0;
            seg_beats[k] = 12'd0;
            seg_iswr[k]  = 1'b0;
            seg_isrd[k]  = 1'b0;
            seg_dir[k]   = 1'b0;
            seg_pres[k]  = 1'b0;
        end
        seg_pres[0] = 1'b1;                                  // 边带每行一条

        // 读源：D7 —— 只有 BLIT 有
        if (r_op == OP_BLIT) begin
            seg_addr[1] = s_a0; seg_beats[1] = s_b0 >> 4; seg_isrd[1] = (s_b0 != 0);
            seg_addr[2] = s_a0 + {{20{1'b0}}, s_b0}; seg_beats[2] = s_b1 >> 4;
            seg_isrd[2] = (s_b1 != 0);
        end
        // 读目的：D7 —— 只有方程不是 COPY 才有。注错位 2 让 ALPHA 也走 COPY 的避让，
        // 注错位 3 让 COPY 也发。
        if (seg_read_dst) begin
            seg_addr[3] = a0; seg_beats[3] = b0 >> 4; seg_isrd[3] = (b0 != 0);
            seg_dir[3]  = 1'b1;
            seg_addr[4] = a0 + {{20{1'b0}}, b0}; seg_beats[4] = b1 >> 4;
            seg_isrd[4] = (b1 != 0); seg_dir[4] = 1'b1;
        end
        // 写目的：每段一笔
        seg_addr[5] = a0; seg_beats[5] = b0 >> 4; seg_iswr[5] = (b0 != 0);
        seg_addr[6] = a0 + {{20{1'b0}}, b0}; seg_beats[6] = b1 >> 4; seg_iswr[6] = (b1 != 0);

        seg_pres[1] = seg_isrd[1]; seg_pres[2] = seg_isrd[2];
        seg_pres[3] = seg_isrd[3]; seg_pres[4] = seg_isrd[4];
        seg_pres[5] = seg_iswr[5]; seg_pres[6] = seg_iswr[6];
    end

    // 跳到 seg_i 之后第一个有货的槽（没有就是 7）
    always @* begin
        nxt_i = 3'd7;
        for (k = 0; k <= 6; k = k + 1)
            if ((k[2:0] > seg_i) && seg_pres[k] && (nxt_i == 3'd7))
                nxt_i = k[2:0];
    end

    // ---------------------------------------------------------------- 发射
    wire cur_sb = (st == S_ROW) && (seg_i == 3'd0);
    wire cur_on = (st == S_ROW) && (seg_i != 3'd0) && (seg_i != 3'd7) && seg_pres[seg_i];
    wire cur_wr = cur_on && seg_iswr[seg_i];
    wire cur_rd = cur_on && seg_isrd[seg_i];
    wire last_wr_seg = (seg_i == 3'd6) || (seg_pres[6] == 1'b0);
    wire last_row_now = (row_i == (nrows - 16'd1));

    // 行缓冲那一半空没空（D41）。
`ifdef INJ_ROW_NOFREE
    // 注错：不看 row_free，直接开下一行
    wire rf_first = 1'b1;                 // 命令的第 0 行
    wire rf_cur   = 1'b1;                 // S_GATE 里等的这一行
    wire rf_next  = 1'b1;                 // S_ADV 要推进到的下一行
`else
    wire rf_first = row_free[0];          // 命令的第 0 行
    wire rf_cur   = row_free[row_i[0]];   // S_GATE 里等的这一行
    wire rf_next  = row_free[~row_i[0]];  // S_ADV 要推进到的下一行
`endif

    always @* begin
        sb_valid      = cur_sb;
        // 边带的数据在 S_ROW 的整个期间都有效，接收方按 sb_valid 取
        sb_row        = row_i;
        sb_py         = py[15:0];
        sb_npx        = npx;
        sb_beats      = wr_total[7:0];
        sb_lead_dst   = dst_cur[3:2];
        // FILL 没有源行，这个字段无意义；固定发 0，独立模型也按 0 记（B4 不看它）
        sb_lead_src   = (r_op == OP_BLIT) ? src_cur[3:2] : 2'd0;
        sb_op         = r_op;
        sb_eq         = r_eq;
        sb_key_en     = r_key_en;
        sb_key        = r_key;
        sb_w4         = r_w4;
        sb_c          = r_c;
        sb_sfmt       = r_sfmt;
        sb_read_dst   = d7_read_dst;

        rd_valid      = cur_rd;
        rd_addr       = seg_addr[seg_i];
        rd_beats      = seg_beats[seg_i];
        rd_buf        = {seg_dir[seg_i], row_i[0]};
        // 片上标记只上源读描述符（G1）：目的读恒走 DDR
        rd_tex        = cur_rd && !seg_dir[seg_i] && r_src_tex;
        rd_cidx       = r_cidx;

        wr_valid      = cur_wr;
        wr_addr       = seg_addr[seg_i];
        wr_beats      = seg_beats[seg_i];
        wr_cidx       = r_cidx;
        wr_last_row   = cur_wr && last_row_now && last_wr_seg;
    end

    // ---------------------------------------------------------------- 时序
    always @(posedge clk) begin
        if (rst) begin
            st <= S_IDLE; busy <= 1'b0;
            cmd_done <= 1'b0; done_cidx <= 4'd0;
            seg_i <= 3'd0; row_i <= 16'd0;
            dst_cur <= 32'd0; src_cur <= 32'd0; nb <= 32'd0;
            dst_stride <= 32'd0; src_stride <= 32'd0;
            nrows <= 16'd0; npx <= 11'd0; py <= 16'sd0;
            r_op <= 8'd0; r_eq <= 4'd0; r_key_en <= 1'b0; r_key <= 32'd0;
            r_w4 <= 32'd0; r_c <= 8'd0; r_sfmt <= 4'd0; r_src_tex <= 1'b0; r_cidx <= 4'd0;
        end else begin
            cmd_done <= 1'b0;
            case (st)
            S_IDLE: begin
                busy <= 1'b0;
                if (start) begin
                    // start 那一拍端口值有效：组合求交的结果当拍锁存
                    r_op <= op; r_eq <= eq; r_key_en <= key_en; r_key <= key_w5;
                    r_w4 <= const_w4; r_c <= c; r_sfmt <= sfmt;
                    r_src_tex <= src_tex; r_cidx <= cidx;
                    done_cidx <= cidx;
                    if (i_empty) begin
                        cmd_done <= 1'b1;                 // D38：零面积命令
                        st <= S_IDLE;
                    end else begin
                        busy      <= 1'b1;
                        nrows     <= t_y1[15:0] - t_y0[15:0] + 16'd1;
                        npx       <= t_x1[10:0] - t_x0[10:0] + 11'd1;
                        nb        <= ({21'b0, (t_x1[10:0] - t_x0[10:0] + 11'd1)}) << 2;
                        py        <= t_y0[15:0];
                        dst_stride <= 32'd1 << dst_slog2;
                        src_stride <= 32'd1 << src_slog2;
                        dst_cur   <= dst_base + (t_y0[31:0] << dst_slog2) +
                                     (t_x0[31:0] << 2);
                        src_cur   <= src_base +
                                     ((p_sy + (t_y0 - p_dy)) << src_slog2) +
                                     (sx_off[31:0] << 2);
                        row_i     <= 16'd0;
                        seg_i     <= 3'd0;
                        // 第 0 行也要等它那一半空着：上一条命令的最后几行可能还占着
                        st        <= rf_first ? S_ROW : S_GATE;
                    end
                end
            end
            S_GATE: begin
                if (rf_cur) st <= S_ROW;
            end
            S_ROW: begin
                if (seg_i == 3'd7) begin
                    st <= S_ADV;                          // 本行发完
                end else if (seg_i == 3'd0) begin
                    if (sb_ready) seg_i <= nxt_i;         // 边带握手（本行第一件事）
                end else if (!seg_pres[seg_i]) begin
                    seg_i <= nxt_i;                       // 空槽不占拍
                end else if (seg_iswr[seg_i] ? wr_ready : rd_ready) begin
                    seg_i <= nxt_i;                       // 握手当拍完成
                end
            end
            S_ADV: begin
                // 行缓冲的乒乓：下一行要用的那一半空着才推（row_free 由 B4 给）
                if (last_row_now) begin
                    st <= S_DONE;
                end else if (rf_next) begin
                    dst_cur <= dst_cur + dst_stride;
                    src_cur <= src_cur + src_stride;
                    py      <= py + 16'sd1;
                    row_i   <= row_i + 16'd1;
                    seg_i   <= 3'd0;
                    st      <= S_ROW;
                end
            end
            S_DONE: begin
                busy <= 1'b0;
                st   <= S_IDLE;
            end
            default: st <= S_IDLE;
            endcase
        end
    end
endmodule

`default_nettype wire
